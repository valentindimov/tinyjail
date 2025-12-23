// SPDX-License-Identifier: MIT

// SPDX-License-Identifier: MIT

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
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tinyjail.h"
#include "launcher.h"
#include "utils.h"
#include <linux/limits.h>

struct tinyjailContainerResult tinyjailLaunchContainer(
    struct tinyjailContainerParams containerParams
) {
    struct tinyjailContainerResult result = {0};

#define RETURN_WITH_ERROR(...) result.containerStartedStatus = -1; snprintf(result.errorInfo, ERROR_INFO_SIZE, __VA_ARGS__); return result;

    // If the container ID is NULL, generate a random 12-character hex ID
    uint64_t randInt = 0;
    getrandom(&randInt, sizeof(randInt), 0);
    ALLOC_LOCAL_FORMAT_STRING(randomContainerId, "%lx", randInt & 0xffffffffffff);
    if (containerParams.containerId == NULL) {
        containerParams.containerId = randomContainerId;
    }

    // Resolve the container root path to an absolute one
    char resolvedRootPath[(PATH_MAX + 1) * sizeof(char)];
    memset(resolvedRootPath, 0, sizeof(resolvedRootPath));
    if (realpath(containerParams.containerDir, resolvedRootPath) == NULL) {
        RETURN_WITH_ERROR("Could not resolve path %s: %s", containerParams.containerDir, strerror(errno));
    }
    if (strcmp(resolvedRootPath, "/") == 0) {
        RETURN_WITH_ERROR("Container root dir cannot be /");
    }
    containerParams.containerDir = resolvedRootPath;

    // Determine the UID and GID for the container as the owner of the container directory
    struct stat containerDirStat;
    if (stat(containerParams.containerDir, &containerDirStat) != 0) {
        RETURN_WITH_ERROR("Could not stat %s: %s", containerParams.containerDir, strerror(errno));
    }
    if (containerParams.uid == -1) {
        containerParams.uid = containerDirStat.st_uid;
    }
    if (containerParams.gid == -1) {
        containerParams.gid = containerDirStat.st_gid;
    }

    // Set default hostname if not specified
    if (containerParams.hostname == NULL) {
        containerParams.hostname = "tinyjail";
    }

    // Validate container parameters
    if (containerParams.containerId && strlen(containerParams.containerId) > 12) {
        RETURN_WITH_ERROR("containerId can be at most 12 characters long.");
    }
    if (!containerParams.commandList) {
        RETURN_WITH_ERROR("containerParams missing required parameter: commandList.");
    }
    if (!containerParams.containerDir) {
        RETURN_WITH_ERROR("containerParams missing required parameter: containerDir.");
    }
    if (!containerParams.environment) {
        RETURN_WITH_ERROR("containerParams missing required parameter: environment.");
    }
    if (containerParams.networkBridgeName && containerParams.networkPeerIpAddr) {
        RETURN_WITH_ERROR("containerParams cannot have both networkBridgeName and networkPeerIPAddr set.");
    }

    // Since we'll pipe in the result of the container launch, set up the pipe first
    int resultPipe[2] = { -1, -1 };
    if (pipe(resultPipe) != 0) {
        RETURN_WITH_ERROR("pipe() failed: %s", strerror(errno));
    }
    RAII_FD resultPipeRead = resultPipe[0];
    RAII_FD resultPipeWrite = resultPipe[1];

    // Now run the container launch function in a subprocess.
    int launcherPid = fork();
    if (launcherPid < 0) {
        RETURN_WITH_ERROR("fork() failed: %s", strerror(errno));
    } else if (launcherPid == 0) {
        // Child process logic goes here
        closep(&resultPipeRead);
        launchContainer(&containerParams, &result);
        write(resultPipeWrite, &result, sizeof(result));
        closep(&resultPipeWrite);
        exit(0);
    } else {
        closep(&resultPipeWrite);
        size_t readResult = read(resultPipeRead, &result, sizeof(result));
        int readResultErrno = errno;
        closep(&resultPipeRead);
        int launcherExitCode;
        int launcherWaitpid = waitpid(launcherPid, &launcherExitCode, __WALL);
        int launcherWaitpidErrno = errno;
        if (readResult != sizeof(result)) {
            RETURN_WITH_ERROR("Could not read() result back from launcher: %s", strerror(readResultErrno));
        } else if (launcherWaitpid < 0) {
            RETURN_WITH_ERROR("Could not waitpid() on launcher: %s", strerror(launcherWaitpidErrno));
        } else {
            return result;
        }
    }

#undef RETURN_WITH_ERROR
}
