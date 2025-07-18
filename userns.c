#include <unistd.h>
#include <fcntl.h>

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
        result->errorInfo = "Could not open child process's /proc subdir. Is /proc mounted?";
        return -1;
    }
    if (tinyjailWriteFileAt(procFd, "uid_map", "0 %u 1\n", uid) != 0) {
        result->errorInfo = "Could not set uid_map for child process.";
        close(procFd);
        return -1;
    }
    if (tinyjailWriteFileAt(procFd, "setgroups", "deny") != 0) {
        result->errorInfo = "Could not set setgroups deny for child process.";
        close(procFd);
        return -1;
    }
    if (tinyjailWriteFileAt(procFd, "gid_map", "0 %u 1\n", gid) != 0) {
        result->errorInfo = "Could not set gid_map for child process.";
        close(procFd);
        return -1;
    }
    close(procFd);
    return 0;
}