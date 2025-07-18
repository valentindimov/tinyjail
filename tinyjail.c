#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "tinyjail.h"
#include "utils.h"
#include "network.h"
#include "logging.h"
#include "cgroup.h"
#include "userns.h"
#include "child.h"

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
            .workDir = containerParams.workDir,
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