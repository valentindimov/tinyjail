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
#include <sys/wait.h>
#include <unistd.h>

#include "userns.h"
#include "utils.h"

static int configureContainerUserNamespace(
    const char* procfsPath,
    int childPid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    ALLOC_LOCAL_FORMAT_STRING(procfsProcPath, "%s/%d", procfsPath, childPid);

    RAII_FD procFd = open(procfsProcPath, 0);
    if (procFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open child process's procfs: %s.", strerror(errno));
        return -1;
    }
    ALLOC_LOCAL_FORMAT_STRING(uidMapContents, "0 %u 1\n", containerParams->uid);
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
    ALLOC_LOCAL_FORMAT_STRING(gidMapContents, "0 %u 1\n", containerParams->gid);
    RAII_FD gidMapFd = openat(procFd, "gid_map", O_WRONLY);
    if (gidMapFd < 0 || write(gidMapFd, gidMapContents, lengidMapContents) < lengidMapContents) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not set gid_map for child process: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int setupContainerUserNamespace(
    int childPid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    if (mount("proc", containerParams->containerDir, "proc", 0, NULL) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not mount temporary procfs: %s", strerror(errno));
        return -1;
    }
    int configureUsernsResult = configureContainerUserNamespace(containerParams->containerDir, childPid, containerParams, result);
    int umount2Result = umount2(containerParams->containerDir, MNT_DETACH);
    int umount2Errno = errno;
    if (configureUsernsResult != 0) {
        return -1;
    }
    if (umount2Result != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not umount temporary procfs mount: %s", strerror(umount2Errno));
        return -1;
    }
    return 0;
}
