#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "utils.h"
#include "userns.h"

int tinyjailSetupContainerUserNamespace(
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