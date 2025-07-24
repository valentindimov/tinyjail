// SPDX-License-Identifier: MIT

// _GNU_SOURCE is needed for setns(), unshare(), clone(), and namespace-related flags for clone().
#define _GNU_SOURCE

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tinyjail.h"

/// @brief Allocates a locally-scoped (via alloca()) string using the provided format.
#define ALLOC_LOCAL_FORMAT_STRING(VARNAME, FORMAT, ...) \
    int len##VARNAME = snprintf("", 0, FORMAT, __VA_ARGS__); \
    char* VARNAME = alloca((len##VARNAME + 1) * sizeof(char)); \
    snprintf(VARNAME, len##VARNAME + 1, FORMAT, __VA_ARGS__);

/// @brief Closes a fire descriptor and sets it to -1 if it is nonnegative. If it is negative, does nothing.
/// @param fd Pointer to the file descriptor variable
static void closep(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/// @brief Defines a special type of FD that gets automatically closed when it exits scope. Use closep() to close it earlier, that function is idempotent.
/// Unfortunately this is a non-standard extension, so it will only compile with gcc or clang. But it's so useful for simplifying error handling, it's worth losing that portability...
#define RAII_FD __attribute__((cleanup(closep))) int

/// @brief Checks if a given string represents a filename (not a path) which is not "." or ".."
/// @param filename The filename to check
/// @return 1 if the string represents a regular filename, and 0 if it does not.
/// Generally you can use this function to see if a user-supplied filename is safe to use.
/// The function will reject filenames which can cause path traversal.
static int stringIsRegularFilename(const char* filename) {
    // Disallow empty strings and the special filenames "." and ".."
    if (*filename == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        return 0;
    }
    // Reject any string containing slashes
    while(*filename) {
        if (*filename == '/') {
            return 0;
        }
        filename++;
    }
    return 1;
}

static int configureContainerCgroup(
    const char* containerCgroupPath,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    RAII_FD cgroupPathFd = open(containerCgroupPath, 0);
    if (cgroupPathFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open cgroup %s: %s.", containerCgroupPath, strerror(errno));
        return -1;
    }
    // Set up delegation
    if (fchownat(cgroupPathFd, ".", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.procs", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.procs: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.subtree_control", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.subtree_control: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.threads", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.threads: %s", strerror(errno));
        return -1; 
    }

    // Apply cgroup configuration options
    for (char** curOptPtr = containerParams->cgroupOptions; *curOptPtr != NULL; curOptPtr++) {
        // Make a copy of the option and make sure it's null-terminated
        // Later on we'll replace the first "=" in this copy with a NULL.
        // The first part (before the NULL) will be the filename in the cgroup folder
        // The second part (after the NULL) will be the contents to write there
        unsigned long szOptionStr = strlen(*curOptPtr) + 1;
        char* filename = alloca(szOptionStr);
        memset(filename, 0, szOptionStr);
        strncpy(filename, *curOptPtr, szOptionStr - 1);

        // Start from the beginning of the string and iterate until the string ends, or we find '='
        char* contents = filename;
        while (*contents != '\0' && *contents != '=') {
            contents++;
        }
        if (*contents == '=') {
            // Found '=', replace it with a NULL and use the remainder of the string as contents.
            *contents = '\0';
            contents++;
            // Make sure we only try writing to files in the cgroup directory
            if (!stringIsRegularFilename(filename)) {
                snprintf(result->errorInfo, ERROR_INFO_SIZE, "Invalid cgroup option name: %s", filename);
                return -1;
            }
            RAII_FD cgroupOptionFd = openat(cgroupPathFd, filename, O_WRONLY);
            size_t lencontents = strlen(contents);
            if (cgroupOptionFd < 0 || write(cgroupOptionFd, contents, lencontents) < lencontents) {
                snprintf(result->errorInfo,ERROR_INFO_SIZE,"Failed to apply cgroup option %s: %s", filename, strerror(errno));
                return -1;
            }
        } else {
            // We did not find an '=' sign, the string was malformed
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Cgroup option %s is missing a value (missing '=')", filename);
            return 1;
        }
    }

    // Move the child process to the cgroup
    ALLOC_LOCAL_FORMAT_STRING(childPidStr, "%d", childPid);
    RAII_FD cgroupChildPidFd = openat(cgroupPathFd, "cgroup.procs", O_WRONLY);
    if (cgroupChildPidFd < 0 || write(cgroupChildPidFd, childPidStr, lenchildPidStr) < lenchildPidStr) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not move container process to cgroup: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int configureContainerUserNamespace(
    int childPid, 
    int uid, 
    int gid, 
    struct tinyjailContainerResult *result
) {
    ALLOC_LOCAL_FORMAT_STRING(procfsProcPath, "/proc/%d", childPid);

    RAII_FD procFd = open(procfsProcPath, 0);
    if (procFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open child process's /proc: %s. Is /proc mounted?", strerror(errno));
        return -1;
    }
    ALLOC_LOCAL_FORMAT_STRING(uidMapContents, "0 %u 1\n", uid);
    RAII_FD uidMapFd = openat(procFd, "uid_map", O_WRONLY);
    if (uidMapFd < 0 || write(uidMapFd, uidMapContents, lenuidMapContents) < lenuidMapContents) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not set uid_map for child process: %s", strerror(errno));
        return -1;
    }
    RAII_FD setgroupsFd = openat(procFd, "setgroups", O_WRONLY);
    if (setgroupsFd < 0 || write(setgroupsFd, "deny", strlen("deny")) < strlen("deny")) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not set setgroups for child process: %s", strerror(errno));
        return -1;
    }
    ALLOC_LOCAL_FORMAT_STRING(gidMapContents, "0 %u 1\n", gid);
    RAII_FD gidMapFd = openat(procFd, "gid_map", O_WRONLY);
    if (gidMapFd < 0 || write(gidMapFd, gidMapContents, lengidMapContents) < lengidMapContents) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not set gid_map for child process: %s", strerror(errno));
        return -1;
    }
    return 0;
}

// TODO: Challenge accepted (and postponed until I have time to learn how to use rtnetlink), do all of this without invoking the iproute2 tool

static int createVethPair(char* if1, char* if2) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link add dev %s type veth peer %s", if1, if2);
    return system(command);
}

static int setMasterOfInterface(char* interface, char* master) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s master %s", interface, master);
    return system(command);
}

static int enableInterface(char* interface) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s up", interface);
    return system(command);
}

static int moveInterfaceToNamespaceByFd(char* interface, int fd) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s netns /proc/self/fd/%d", interface, fd);
    return system(command);
}

static int addAddressToInterface(char* interface, char* address) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip addr add %s dev %s", address, interface);
    return system(command);
}

static int addDefaultRoute(char* targetAddress, char* targetInterface) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip route add default via %s dev %s", targetAddress, targetInterface);
    return system(command);
}

static int configureNetwork(
    int childPidFd,
    int myNetNsFd,
    const char* containerId,
    struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    // Create the vEth pair -inside- the container, then move it outside of it by using the parent PID as the namespace PID.
    // This saves us from having to delete the interface to clean up - when the container dies, the interface is automatically cleaned up.
    ALLOC_LOCAL_FORMAT_STRING(vethNameInside, "i_%s", containerId);
    ALLOC_LOCAL_FORMAT_STRING(vethNameOutside, "o_%s", containerId);

    if (setns(childPidFd, CLONE_NEWNET) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "setns() to enter the container network namespace failed: %s", strerror(errno));
        return -1;
    }
    if (createVethPair(vethNameInside, vethNameOutside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to create vEth pair %s-%s.", vethNameOutside, vethNameInside);
        return -1;
    }
    if (moveInterfaceToNamespaceByFd(vethNameOutside, myNetNsFd) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to move interface %s to outside network namespace.", vethNameOutside);
        return -1;
    }
    if (enableInterface(vethNameInside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to enable inside interface %s.", vethNameInside);
        return -1;
    }
    if (params->networkIpAddr) {
        if (addAddressToInterface(vethNameInside, params->networkIpAddr) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add address %s to inside interace %s.", params->networkIpAddr, vethNameInside);
            return -1;
        }
    }
    if (params->networkDefaultRoute) {
        if (addDefaultRoute(params->networkDefaultRoute, vethNameInside) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add default route %s to inside interace %s.", params->networkDefaultRoute, vethNameInside);
            return -1;
        }
    }
    if (setns(myNetNsFd, CLONE_NEWNET) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "setns() to go back to the host network namespace failed: %s", strerror(errno));
        return -1;
    }

    if (params->networkPeerIpAddr) {
        if (addAddressToInterface(vethNameOutside, params->networkPeerIpAddr) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add address %s to outside interace %s.", params->networkPeerIpAddr, vethNameOutside);
            return -1;
        }
    }
    if (params->networkBridgeName) {
        if (setMasterOfInterface(vethNameOutside, params->networkBridgeName) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not attach outside interace %s to bridge %s.", vethNameOutside, params->networkBridgeName);
            return -1;
        }
    }
    if (enableInterface(vethNameOutside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to enable outside interface %s.", vethNameOutside);
        return -1;
    }
    return 0;
}

static int setupContainerNetwork(
    int childPid, 
    char* containerId, 
    struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    // Get a handle on both the inside and outside network namespaces
    RAII_FD myNetNsFd = open("/proc/self/ns/net", O_RDONLY);
    if (myNetNsFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open /proc/self/ns/net: %s", strerror(errno));
        return -1;
    }
    RAII_FD childPidFd = syscall(SYS_pidfd_open, childPid, 0);
    if (childPidFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "pidfd_open() on child PID failed: %s", strerror(errno));
        return -1;
    }
    int retval = configureNetwork(childPidFd, myNetNsFd, containerId, params, result);
    // Make sure we're in our own network namespace even if we failed
    setns(myNetNsFd, CLONE_NEWNET);
    return retval;
}

struct ContainerChildLauncherArgs {
    char* containerDir;
    char** commandList;
    char** environment;
    char* workDir;
    // Pipe used by the parent to signal to the child that its namespaces are initialized and it may execve() now.
    int syncPipeWrite;
    int syncPipeRead;
    // Pipe used by the child to send error messages to the parent.
    int errorPipeWrite;
    int errorPipeRead;
};

static int containerChildLaunch(struct ContainerChildLauncherArgs *args) {
    // We won't need the writing end of the sync pipe (and in case the parent crashes, we want to avoid being stuck waiting on ourselves)
    close(args->syncPipeWrite);
    close(args->errorPipeRead);
    
    // Wait to get a message "OK" over the sync pipe. Only if we get that are we sure that our parent has initialized everything.
    char result[2];
    if (read(args->syncPipeRead, result, 2) != 2 || strncmp(result, "OK", 2) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not read() on sync pipe: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }
    close(args->syncPipeRead);

    // Unshare the cgroup namespace here (after our parent has had the chance to move us to our cgroup)
    if (unshare(CLONE_NEWCGROUP) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Unsharing cgroup namespace in child failed: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Set our UID and GID.
    if (setuid(0) != 0 || setgid(0) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not switch UID or GID: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Bind-mount the container dir to itself then pivot to it
    if (mount(args->containerDir, args->containerDir, "none", MS_BIND | MS_PRIVATE | MS_REC | MS_NOSUID, NULL) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not bind-mount container roor dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }
    if (chdir(args->containerDir) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not chdir to container roor dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Pivot the filesystem root
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not pivot_root to container roor dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }
    if (umount2(".", MNT_DETACH) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not unmount old root dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // If a working directory was set, make sure to set that before execve-ing
    if (args->workDir != NULL && chdir(args->workDir) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not chdir to chosen workdir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Make sure that if we successfully execve(), the errorPipeWrite is closed
    if (fcntl(args->errorPipeWrite, F_SETFD, FD_CLOEXEC) < 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "fcntl() on error pipe failed: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // All good, execute the target command.
    execve(args->commandList[0], (args->commandList + 1), args->environment);

    // If we got here, the execve() call failed.
    ALLOC_LOCAL_FORMAT_STRING(error, "execve() failed: %s", strerror(errno))
    write(args->errorPipeWrite, error, strlen(error));
    return -1;
}

static int finishConfiguringAndAwaitContainerProcess(
    struct tinyjailContainerParams *containerParams,
    struct tinyjailContainerResult *result,
    char* containerId,
    char* containerCgroupPath,
    int childPid,
    int uid,
    int gid,
    int syncPipeWrite,
    int errorPipeRead
) {
    if (configureContainerCgroup(containerCgroupPath, childPid, uid, gid, containerParams, result) != 0) {
        return -1;
    }
    if (configureContainerUserNamespace(childPid, uid, gid, result) != 0) {
        return -1;
    }
    if (setupContainerNetwork(childPid, containerId, containerParams, result) != 0) {
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

struct tinyjailContainerResult tinyjailLaunchContainer(struct tinyjailContainerParams containerParams) {
    struct tinyjailContainerResult result = {0};

    if (getuid() != 0) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "tinyjail requires root permissions to run.");
        return result;
    }

    // Validate container parameters
    if (!containerParams.commandList) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "containerParams missing required parameter: commandList.");
        return result;
    }
    if (!containerParams.containerDir) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "containerParams missing required parameter: containerDir.");
        return result;
    }
    if (!containerParams.environment) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "containerParams missing required parameter: environment.");
        return result;
    }
    if (containerParams.networkBridgeName && containerParams.networkPeerIpAddr) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "containerParams cannot have both networkBridgeName and networkPeerIPAddr set.");
        return result;
    }

    // Limit the container ID to 12 characters since it's used to generate names for the network interfaces (which are capped at 15 characters)
    unsigned long random;
    getrandom(&random, sizeof(random), 0);
    ALLOC_LOCAL_FORMAT_STRING(containerId, "tj_%lx", random & 0xffffffffff);

    // Determine the UID and GID for the container as the owner of the container directory
    struct stat containerDirStat;
    if (stat(containerParams.containerDir, &containerDirStat) != 0) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "Could not stat %s: %s", containerParams.containerDir, strerror(errno));
        return result;
    }
    unsigned int uid = containerDirStat.st_uid;
    unsigned int gid = containerDirStat.st_gid;
    
    // Set up the sync pipe for signalling the child process to begin execution, and one for passing error messages back
    int syncPipe[2] = { -1, -1 };
    int errorPipe[2] = { -1, -1 };
    int pipeSuccess = (pipe(syncPipe) == 0 && pipe(errorPipe) == 0);
    RAII_FD syncPipeRead = syncPipe[0];
    RAII_FD syncPipeWrite = syncPipe[1];
    RAII_FD errorPipeRead = errorPipe[0];
    RAII_FD errorPipeWrite = errorPipe[1];
    if (!pipeSuccess) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "pipe() failed: %s", strerror(errno));
        return result;
    }

    // Start the child process and close the read end of the sync pipe (it is for the child process only)
    // Do not unshare the cgroup namespace just yet - the subprocess will do this, after we have moved it to the right cgroup. 
    struct ContainerChildLauncherArgs args = {
        .containerDir = containerParams.containerDir,
        .commandList = containerParams.commandList,
        .environment = containerParams.environment,
        .workDir = containerParams.workDir,
        .syncPipeRead = syncPipeRead,
        .syncPipeWrite = syncPipeWrite,
        .errorPipeRead = errorPipeRead,
        .errorPipeWrite = errorPipeWrite
    };
    int cloneFlags = (
        CLONE_NEWNS
        | CLONE_NEWIPC
        | CLONE_NEWPID
        | CLONE_NEWUTS
        | CLONE_NEWUSER
        | CLONE_NEWTIME
        | CLONE_NEWNET
        | SIGCHLD
    );
    // The stack memory of the child is a local 4K buffer allocated in this function. 
    // This should be enough, but in either case, the child has its own memory map 
    // so even if it overruns the buffer, it shouldn't cause problems for us.
    int childPid = clone(
        (int (*)(void *)) containerChildLaunch, 
        (void*) (((char*) alloca(4096)) + 4095), 
        cloneFlags,
        (void*) &args
    );
    if (childPid < 0) {
        result.containerStartedStatus = -1;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "clone() failed: %s", strerror(errno));
        return result;
    }
    closep(&syncPipeRead); // closep() is idempotent because it also sets the FD variable to -1
    closep(&errorPipeWrite); // closep() is idempotent because it also sets the FD variable to -1

    // Create a cgroup for the child process
    ALLOC_LOCAL_FORMAT_STRING(containerCgroupPath, "/sys/fs/cgroup/container_%s", containerId);
    if (mkdir(containerCgroupPath, 0770) != 0) {
        containerCgroupPath = NULL;
        snprintf(result.errorInfo, ERROR_INFO_SIZE, "Could not create cgroup: %s. Make sure /sys/fs/cgroup is mounted.", strerror(errno));
        return result;
    }
    // There is only one return point from this point on, so we're sure we will delete the container cgroup before returning.

    if (finishConfiguringAndAwaitContainerProcess(
        &containerParams,
        &result,
        containerId,
        containerCgroupPath,
        childPid,
        uid,
        gid,
        syncPipeWrite,
        errorPipeRead
    ) != 0) {
        kill(childPid, SIGKILL);
        // The subroutines should have set an error message already
        result.containerStartedStatus = -1;
    }

    // Before returning, make sure to vacuum up any leftover child processes and remove the cgroup
    int tmp;
    while (wait(&tmp) > 0) {}
    rmdir(containerCgroupPath);

    return result;
}