#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/random.h>

#include "tinyjail.h"
#include "utils.h"
#include "network.h"
#include "logging.h"
#include "cgroup.h"
#include "userns.h"

struct ContainerChildLauncherArgs {
    char* containerDir;
    char** commandList;
    char** environment;
    // Pipe used by the parent to signal to the child that its namespaces are initialized and it may execve() now.
    int syncPipeWrite;
    int syncPipeRead;
};

static int containerChildLaunch(struct ContainerChildLauncherArgs *args) {
    // We won't need the writing end of the sync pipe (and in case the parent crashes, we want to avoid being stuck waiting on ourselves)
    close(args->syncPipeWrite);
    // Wait to get a message "OK" over the sync pipe. Only if we get that are we sure that our parent has initialized everything.
    char result[2];
    if (read(args->syncPipeRead, result, 2) != 2 || strncmp(result, "OK", 2) != 0) {
        tinyjailLogError("Waiting for the go-ahead signal from the launcher encountered an error: %s", strerror(errno));
        return -1;
    }
    close(args->syncPipeRead);

    // Unshare the cgroup namespace here (after our parent has had the chance to move us to our cgroup)
    if (unshare(CLONE_NEWCGROUP) != 0) {
        tinyjailLogError("Child could not unshare cgroup namespace: %s", strerror(errno));
        return -1;
    }

    // Set our UID and GID.
    if (setuid(0) != 0 || setgid(0) != 0) {
        tinyjailLogError("Child could not switch UID and GID: %s", strerror(errno));
        return -1;
    }

    // Bind-mount the container dir to itself then pivot to it
    if (mount(args->containerDir, args->containerDir, "none", MS_BIND | MS_PRIVATE | MS_REC | MS_NOSUID, NULL) != 0) {
        tinyjailLogError("Child could not create a bind-mount at %s: %s", args->containerDir, strerror(errno));
        return -1;
    }
    if (chdir(args->containerDir) != 0) {
        tinyjailLogError("Child could chdir to %s: %s", args->containerDir, strerror(errno));
        return -1;
    }

    // Pivot the filesystem root
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        tinyjailLogError("Child could pivot_root to %s: %s", args->containerDir, strerror(errno));
        return -1;
    }
    if (umount2(".", MNT_DETACH) != 0) {
        tinyjailLogError("Child could unmount old root after pivot_root: %s", strerror(errno));
        return -1;
    }

    // All good, execute the target command.
    execve(args->commandList[0], (args->commandList + 1), args->environment);

    // If we got here, the execve() call failed. We already cleaned the temporary directory though, so just exit.
    tinyjailLogError("execve() failed: %s", strerror(errno));
    return -1;
}

int tinyjailLaunchContainer(struct tinyjailContainerParams containerParams) {
    int childPid = -1;

    // Validate container parameters
    if (!containerParams.commandList 
        || !containerParams.containerDir 
        || !containerParams.environment
        || (!containerParams.networkBridgeName && (containerParams.networkIpAddr || containerParams.networkDefaultRoute))
    ) {
        tinyjailLogError("Invalid containerParams.");
        return -1;
    }

    // Determine the UID and GID for the container as the owner of the container directory
    struct stat containerDirStat;
    if (stat(containerParams.containerDir, &containerDirStat) != 0) { 
        tinyjailLogError("Could not stat container directory %s: %d", containerParams.containerDir, strerror(errno));
        return -1;
    }
    unsigned int uid = containerDirStat.st_uid;
    unsigned int gid = containerDirStat.st_gid;
    
    // Set up the sync pipe for signalling the child process to begin execution
    int syncPipe[2] = { -1, -1 };
    if (pipe(syncPipe) != 0) { 
        tinyjailLogError("Could not set up sync pipe: %s", strerror(errno));
        return -1;
    }
    int syncPipeWrite = syncPipe[1];

    // Start the child process and close the read end of the sync pipe (it is for the child process only)
    {
        int syncPipeRead = syncPipe[0];
        // Do not unshare the cgroup namespace just yet - the subprocess will do this, after we have moved it to the right cgroup. 
        struct ContainerChildLauncherArgs args = {
            .containerDir = containerParams.containerDir,
            .commandList = containerParams.commandList,
            .environment = containerParams.environment,
            .syncPipeRead = syncPipeRead,
            .syncPipeWrite = syncPipeWrite
        };
        // The stack memory of the child is a local 4K buffer allocated in this function. 
        // This is fine, since the child runs in its own address space - even if we freed this region in our process, the child would be unaffected.
        childPid = clone(
            (int (*)(void *)) containerChildLaunch, 
            (void*) (((char*) alloca(4096)) + 4095), 
            CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWUSER | CLONE_NEWTIME | CLONE_NEWNET | SIGCHLD,
            (void*) &args
        );
        if (childPid < 0) { 
            tinyjailLogError("Could not launch child process: %s", strerror(errno));
        }
        close(syncPipeRead);
    }

    // Limit the container ID to 12 characters since it's used to generate names for the network interfaces (which are capped at 15 characters)
    unsigned long random;
    getrandom(&random, sizeof(random), 0);
    ALLOC_LOCAL_FORMAT_STRING(containerId, "tj_%lx", random & 0xffffffffff);
    int childExitInfo = 0;

    int retval = 0;
    if (
        childPid < 0
        || tinyjailSetupContainerCgroup(containerId, childPid, uid, gid, &containerParams) != 0
        || tinyjailSetupContainerUserNamespace(childPid, uid, gid) != 0
        || (containerParams.networkBridgeName && tinyjailSetupContainerNetwork(childPid, containerId, &containerParams) != 0)
        || write(syncPipeWrite, "OK", 2) != 2 // TODO: logError("Could not give the child the go-ahead signal: %s", strerror(errno))
        || waitpid(childPid, &childExitInfo, __WALL) < 0 // TODO: logError("waitpid() failed: %s", strerror(errno)); 
    ) {
        kill(childPid, SIGKILL);
        retval = -1;
    } else {
        // Container ran and exited - determine the exit status
        if (WIFEXITED(childExitInfo)) { 
            retval = WEXITSTATUS(childExitInfo); 
            if (retval != 0) {
                tinyjailLogError("Container exited with nonzero exit code: %d", retval);
            }
        } else if (WIFSIGNALED(childExitInfo)) {
            tinyjailLogError("Container killed by signal: %d", WTERMSIG(childExitInfo));
        }
    }
    // As we clean up, make sure to vacuum up any leftover child processes
    close(syncPipeWrite);
    while (wait(&childExitInfo) > 0) {}
    tinyjailDestroyCgroup(containerId);

    return retval;
}