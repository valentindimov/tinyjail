#include <unistd.h>
#include <fcntl.h>

#include "utils.h"
#include "logging.h"
#include "userns.h"

int tinyjailSetupContainerUserNamespace(int childPid, int uid, int gid) {
    ALLOC_LOCAL_FORMAT_STRING(procfsProcPath, "/proc/%d", childPid);

    int procFd = open(procfsProcPath, 0);
    if (procFd >= 0) {
        if (
            tinyjailWriteFileAt(procFd, "uid_map", "0 %u 1\n", uid) == 0 
            && tinyjailWriteFileAt(procFd, "setgroups", "deny") == 0 
            && tinyjailWriteFileAt(procFd, "gid_map", "0 %u 1\n", gid) == 0
        ) {
            // SUCCESS CASE CLEANUP
            close(procFd);
            return 0;
        }
        // FAILURE CASE CLEANUP
        close(procFd);
    }
    return -1;
}