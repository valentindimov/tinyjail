#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "tinyjail.h"
#include "utils.h"
#include "network.h"
#include "cgroup.h"
#include "userns.h"
#include "child.h"

struct tinyjailContainerResult tinyjailLaunchContainer(struct tinyjailContainerParams containerParams) {
    struct tinyjailContainerResult result = {0};
    
    int childPid = -1;

    // Validate container parameters
    if (!containerParams.commandList) {
        result.containerStartedStatus = -1;
        snprintf(
            result.errorInfo,
            ERROR_INFO_SIZE,
            "containerParams missing required parameter: commandList."
        );
        return result;
    }
    if (!containerParams.containerDir) {
        result.containerStartedStatus = -1;
        snprintf(
            result.errorInfo,
            ERROR_INFO_SIZE,
            "containerParams missing required parameter: containerDir."
        );
        return result;
    }
    if (!containerParams.environment) {
        result.containerStartedStatus = -1;
        snprintf(
            result.errorInfo,
            ERROR_INFO_SIZE,
            "containerParams missing required parameter: environment."
        );
        return result;
    }
    if (containerParams.networkBridgeName && containerParams.networkPeerIpAddr) {
        result.containerStartedStatus = -1;
        snprintf(
            result.errorInfo,
            ERROR_INFO_SIZE,
            "containerParams cannot have both networkBridgeName and networkPeerIPAddr set."
        );
        return result;
    }

    // Determine the UID and GID for the container as the owner of the container directory
    struct stat containerDirStat;
    if (stat(containerParams.containerDir, &containerDirStat) != 0) {
        result.containerStartedStatus = -1;
        snprintf(
            result.errorInfo,
            ERROR_INFO_SIZE,
            "Could not stat %s: %s",
            containerParams.containerDir,
            strerror(errno)
        );
        return result;
    }
    unsigned int uid = containerDirStat.st_uid;
    unsigned int gid = containerDirStat.st_gid;
    
    // Set up the sync pipe for signalling the child process to begin execution
    int syncPipe[2] = { -1, -1 };
    if (pipe(syncPipe) != 0) {
        result.containerStartedStatus = -1;
        snprintf(
            result.errorInfo,
            ERROR_INFO_SIZE,
            "pipe() failed: %s",
            strerror(errno)
        );
        return result;
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
        childPid = clone(
            (int (*)(void *)) containerChildLaunch, 
            (void*) (((char*) alloca(4096)) + 4095), 
            cloneFlags,
            (void*) &args
        );
        close(syncPipeRead);
        if (childPid < 0) { 
            result.containerStartedStatus = -1;
            snprintf(
                result.errorInfo,
                ERROR_INFO_SIZE,
                "clone() failed: %s",
                strerror(errno)
            );
            close(syncPipeWrite);
            return result;
        }
    }

    // Limit the container ID to 12 characters since it's used to generate names for the network interfaces (which are capped at 15 characters)
    unsigned long random;
    getrandom(&random, sizeof(random), 0);
    ALLOC_LOCAL_FORMAT_STRING(containerId, "tj_%lx", random & 0xffffffffff);

    if (
        childPid < 0
        || tinyjailSetupContainerCgroup(containerId, childPid, uid, gid, &containerParams, &result) != 0
        || tinyjailSetupContainerUserNamespace(childPid, uid, gid, &result) != 0
        || tinyjailSetupContainerNetwork(childPid, containerId, &containerParams, &result) != 0
        || write(syncPipeWrite, "OK", 2) != 2 // TODO: logError("Could not give the child the go-ahead signal: %s", strerror(errno))
        || waitpid(childPid, &(result.containerExitStatus), __WALL) < 0 // TODO: logError("waitpid() failed: %s", strerror(errno)); 
    ) {
        kill(childPid, SIGKILL);
        close(syncPipeWrite);
        tinyjailDestroyCgroup(containerId);
        // The subroutines should have set an error message already
        result.containerStartedStatus = -1;
        return result;
    }
    // As we clean up, make sure to vacuum up any leftover child processes
    int tmp;
    while (wait(&tmp) > 0) {}
    close(syncPipeWrite);
    tinyjailDestroyCgroup(containerId);

    return result;
}