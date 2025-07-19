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

    int procFd = open(procfsProcPath, 0);
    if (procFd < 0) {
        snprintf(
            result->errorInfo, 
            ERROR_INFO_SIZE, 
            "Could not open child process's /proc: %s. Is /proc mounted?",
            strerror(errno)
        );
        return -1;
    }
    if (tinyjailWriteFileAt(procFd, "uid_map", "0 %u 1\n", uid) != 0) {
        snprintf(
            result->errorInfo, 
            ERROR_INFO_SIZE, 
            "Could not set uid_map for child process: %s",
            strerror(errno)
        );
        close(procFd);
        return -1;
    }
    if (tinyjailWriteFileAt(procFd, "setgroups", "deny") != 0) {
        snprintf(
            result->errorInfo, 
            ERROR_INFO_SIZE, 
            "Could not set setgroups deny for child process: %s",
            strerror(errno)
        );
        close(procFd);
        return -1;
    }
    if (tinyjailWriteFileAt(procFd, "gid_map", "0 %u 1\n", gid) != 0) {
        snprintf(
            result->errorInfo, 
            ERROR_INFO_SIZE, 
            "Could not set gid_map for child process: %s",
            strerror(errno)
        );
        close(procFd);
        return -1;
    }
    close(procFd);
    return 0;
}