// SPDX-License-Identifier: MIT

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <alloca.h>
#include <sys/random.h>
#include <stdint.h>

#include "tinyjail.h"
#include "launcher.h"
#include "utils.h"

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

    // Validate the container parameters
    // TODO: move this from launcher.c

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
