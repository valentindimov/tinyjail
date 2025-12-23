// SPDX-License-Identifier: MIT

// _GNU_SOURCE is needed for setns(), unshare(), clone(), and namespace-related flags for clone().
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tinyjail.h"
#include "utils.h"
#include "cgroup.h"
#include "network.h"
#include "userns.h"

struct ContainerInitArgs {
    char* containerDir;
    char** commandList;
    char** environment;
    char* workDir;
    char* hostname;
    // Pipe used by the parent to signal to the child that its namespaces are initialized and it may execve() now.
    int syncPipeWrite;
    int syncPipeRead;
    // Pipe used by the child to send error messages to the parent.
    int errorPipeWrite;
    int errorPipeRead;
};

/// @brief Runs the initial part of the container init process. Runs in a separate process.
/// @param args Arguments for container initialization
/// @return Nothing if it gets to execve()-ing the container entrypoint, otherwise returns -1 on failure.
static int runContainerInit(struct ContainerInitArgs *args) {
#define RETURN_WITH_ERROR(...) { ALLOC_LOCAL_FORMAT_STRING(error, __VA_ARGS__); write(args->errorPipeWrite, error, strlen(error)); return -1; }

    // We won't need the writing end of the sync pipe (and in case the parent crashes, we want to avoid being stuck waiting on ourselves)
    close(args->syncPipeWrite);
    close(args->errorPipeRead);
    
    // Wait to get a message "OK" over the sync pipe. Only if we get that are we sure that our parent has initialized everything.
    char result[2];
    if (read(args->syncPipeRead, result, 2) != 2 || strncmp(result, "OK", 2) != 0) {
        RETURN_WITH_ERROR("Child could not read() on sync pipe: %s", strerror(errno));
    }
    close(args->syncPipeRead);

    // Set our UID and GID.
    if (setuid(0) != 0 || setgid(0) != 0) {
        RETURN_WITH_ERROR("Child could not switch UID or GID: %s", strerror(errno));
    }

    // Set the container init process to be a subreaper, since most init processes expect it.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        RETURN_WITH_ERROR("Could not set container init as subreaper: %s", strerror(errno));
    }

    // Unshare the cgroup namespace here (after our parent has had the chance to move us to our cgroup)
    if (unshare(CLONE_NEWCGROUP) != 0) {
        RETURN_WITH_ERROR("Unsharing cgroup namespace in child failed: %s", strerror(errno));
    }

    // Make sure the container root is a mountpoint
    if (mount(args->containerDir, args->containerDir, "none", MS_BIND | MS_PRIVATE | MS_REC | MS_NOSUID, NULL) != 0) {
        RETURN_WITH_ERROR("Could not bind-mount container roor dir: %s", strerror(errno));
    }
    // Pivot to the filesystem root
    if (chdir(args->containerDir) != 0) {
        RETURN_WITH_ERROR("Child could not chdir to container roor dir: %s", strerror(errno));
    }
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        RETURN_WITH_ERROR("Child could not pivot_root to container roor dir: %s", strerror(errno));
    }
    if (umount2(".", MNT_DETACH) != 0) {
        RETURN_WITH_ERROR("Child could not unmount old root dir: %s", strerror(errno));
    }

    // If a working directory was set, make sure to set that before execve-ing
    if (args->workDir != NULL && chdir(args->workDir) != 0) {
        RETURN_WITH_ERROR("Child could not chdir to chosen workdir: %s", strerror(errno));
    }

    // Set up the hostname
    if (sethostname(args->hostname, strlen(args->hostname)) < 0) {
        RETURN_WITH_ERROR("fcntl() on error pipe failed: %s", strerror(errno));
    }

    // Make sure that if we successfully execve(), the errorPipeWrite is closed
    if (fcntl(args->errorPipeWrite, F_SETFD, FD_CLOEXEC) < 0) {
        RETURN_WITH_ERROR("fcntl() on error pipe failed: %s", strerror(errno));
    }

    // All good, execute the target command.
    execve(args->commandList[0], (args->commandList + 1), args->environment);

    // If we got here, the execve() call failed.
    RETURN_WITH_ERROR("execve() failed: %s", strerror(errno));

#undef RETURN_WITH_ERROR
}

static int finishConfiguringAndAwaitContainerProcess(
    const struct tinyjailContainerParams *containerParams,
    struct tinyjailContainerResult *result,
    int childPid,
    int syncPipeWrite,
    int errorPipeRead
) {
    if (setupContainerCgroup(childPid, containerParams, result) != 0) {
        return -1;
    }
    if (setupContainerUserNamespace(childPid, containerParams, result) != 0) {
        return -1;
    }
    if (setupContainerNetwork(childPid, containerParams, result) != 0) {
        return -1;
    }
    if (write(syncPipeWrite, "OK", 2) != 2) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not give the child the go-ahead signal: %s", strerror(errno));
        return -1;
    }
    if (read(errorPipeRead, result->errorInfo, ERROR_INFO_SIZE - 1) > 0) {
        return -1;
    }
    if (waitpid(childPid, &(result->containerExitStatus), __WALL) < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "waitpid() failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void launchContainer(
    const struct tinyjailContainerParams *containerParams, 
    struct tinyjailContainerResult *result
) {
#define RETURN_WITH_ERROR(...) result->containerStartedStatus = -1; snprintf(result->errorInfo, ERROR_INFO_SIZE, __VA_ARGS__); return;

    // The container launcher already sets up the mounts for the container, so it runs in its own mount namespace.
    if (unshare(CLONE_NEWNS) != 0) {
        RETURN_WITH_ERROR("Unsharing mount namespace in child failed: %s", strerror(errno));
    }
    // Transition shared mounts to private mounts to prevent anything we do with mounts from propagating outside of the countainer
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        RETURN_WITH_ERROR("Could not set all mounts to private: %s", strerror(errno));
    }
    
    // Set up the sync pipe for signalling the child process to begin execution, and one for passing error messages back
    int syncPipe[2] = { -1, -1 };
    int errorPipe[2] = { -1, -1 };
    int pipeSuccess = (pipe(syncPipe) == 0 && pipe(errorPipe) == 0);
    RAII_FD syncPipeRead = syncPipe[0];
    RAII_FD syncPipeWrite = syncPipe[1];
    RAII_FD errorPipeRead = errorPipe[0];
    RAII_FD errorPipeWrite = errorPipe[1];
    if (!pipeSuccess) {
        RETURN_WITH_ERROR("pipe() failed: %s", strerror(errno));
    }

    // Set tinyjail itself as a subreaper, so that if the container init dies, we are the ones vacuuming up leftover children.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        RETURN_WITH_ERROR("Could not set container init as subreaper: %s", strerror(errno));
    }

    // Start the child process and close the read end of the sync pipe (it is for the child process only)
    // Do not unshare the cgroup namespace just yet - the subprocess will do this, after we have moved it to the right cgroup. 
    struct ContainerInitArgs args = {
        .containerDir = containerParams->containerDir,
        .commandList = containerParams->commandList,
        .environment = containerParams->environment,
        .hostname = containerParams->hostname,
        .workDir = containerParams->workDir,
        .syncPipeRead = syncPipeRead,
        .syncPipeWrite = syncPipeWrite,
        .errorPipeRead = errorPipeRead,
        .errorPipeWrite = errorPipeWrite
    };
    int cloneFlags = (CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWUSER | CLONE_NEWTIME | SIGCHLD);
    // Only unshare the network namespace if useHostNetwork is not set
    if (containerParams->useHostNetwork == 0) {
        cloneFlags |= CLONE_NEWNET;
    }
    // The stack memory of the child is a local 4K buffer allocated in this function. 
    // This should be enough, but in either case, the child has its own memory map so even if it overruns the buffer, it shouldn't cause problems for us.
    int childPid = clone((int (*)(void *)) runContainerInit, (void*) (((char*) alloca(4096)) + 4095), cloneFlags, (void*) &args);
    if (childPid < 0) {
        RETURN_WITH_ERROR("clone() failed: %s", strerror(errno));
    }
    closep(&syncPipeRead); // closep() is idempotent because it also sets the FD variable to -1
    closep(&errorPipeWrite); // closep() is idempotent because it also sets the FD variable to -1

    // Create a cgroup for the child process. Note that we run in our own network namespaces and we've set all mounts to private, so the host should not see this.
    {
        if (mount("none", containerParams->containerDir, "cgroup2", 0, NULL) != 0) {
            RETURN_WITH_ERROR("Could not mount cgroupfs: %s", strerror(errno));
        }
        ALLOC_LOCAL_FORMAT_STRING(cgroupPath, "%s/%s", containerParams->containerDir, containerParams->containerId);
        int mkdirResult = mkdir(cgroupPath, 0770);
        int mkdirErrno = errno;
        int umount2Result = umount2(containerParams->containerDir, MNT_DETACH);
        int umount2Errno = errno;
        if (mkdirResult != 0) {
            RETURN_WITH_ERROR("Could not create cgroup: %s.", strerror(mkdirErrno));
        }
        if (umount2Result!= 0) {
            RETURN_WITH_ERROR("Could not umount temporary cgroupfs mount: %s", strerror(umount2Errno));
        }
    }
    // There is only one return point from this point on, so we're sure we will delete the container cgroup before returning.

    if (finishConfiguringAndAwaitContainerProcess(containerParams, result, childPid, syncPipeWrite, errorPipeRead) != 0) {
        kill(childPid, SIGKILL);
        // The subroutines should have set an error message already
        result->containerStartedStatus = -1;
    }

    // Success. Now attempt final cleanup...
    // Make sure to vacuum up any leftover child processes
    int tmp;
    while (wait(&tmp) > 0) {}
    // Make sure to remove the cgroup
    cleanContainerCgroup(containerParams);

    return;

#undef RETURN_WITH_ERROR
}
