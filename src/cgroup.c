#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <alloca.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "utils.h"
#include "cgroup.h"

int tinyjailSetupContainerCgroup(
    const char* containerCgroupPath,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    RAII_FD cgroupPathFd = open(containerCgroupPath, 0);
    if (cgroupPathFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open cgroup %s: %s.", containerCgroupPath, strerror(errno));
        return -1;
    }
    // Set up delegation
    if (fchownat(cgroupPathFd, ".", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.procs", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.procs: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.subtree_control", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.subtree_control: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.threads", uid, gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.threads: %s", strerror(errno));
        return -1; 
    }

    // Apply cgroup configuration options
    for (char** curOptPtr = containerParams->cgroupOptions; *curOptPtr != NULL; curOptPtr++) {
        // Make a copy of the option and make sure it's null-terminated
        // Later on we'll replace the first "=" in this copy with a NULL.
        // The first part (before the NULL) will be the filename in the cgroup folder
        // The second part (after the NULL) will be the contents to write there
        unsigned long szOptionStr = strlen(*curOptPtr) + 1;
        char* filename = alloca(szOptionStr);
        memset(filename, 0, szOptionStr);
        strncpy(filename, *curOptPtr, szOptionStr - 1);

        // Start from the beginning of the string and iterate until the string ends, or we find '='
        char* contents = filename;
        while (*contents != '\0' && *contents != '=') {
            contents++;
        }
        if (*contents == '=') {
            // Found '=', replace it with a NULL and use the remainder of the string as contents.
            *contents = '\0';
            contents++;
            // Make sure we only try writing to files in the cgroup directory
            if (!stringIsRegularFilename(filename)) {
                snprintf(result->errorInfo, ERROR_INFO_SIZE, "Invalid cgroup option name: %s", filename);
                return -1;
            }
            RAII_FD cgroupOptionFd = openat(cgroupPathFd, filename, O_WRONLY);
            size_t lencontents = strlen(contents);
            if (cgroupOptionFd < 0 || write(cgroupOptionFd, contents, lencontents) < lencontents) {
                snprintf(result->errorInfo,ERROR_INFO_SIZE,"Failed to apply cgroup option %s: %s", filename, strerror(errno));
                return -1;
            }
        } else {
            // We did not find an '=' sign, the string was malformed
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Cgroup option %s is missing a value (missing '=')", filename);
            return 1;
        }
    }

    // Move the child process to the cgroup
    ALLOC_LOCAL_FORMAT_STRING(childPidStr, "%d", childPid);
    RAII_FD cgroupChildPidFd = openat(cgroupPathFd, "cgroup.procs", O_WRONLY);
    if (cgroupChildPidFd < 0 || write(cgroupChildPidFd, childPidStr, lenchildPidStr) < lenchildPidStr) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not move container process to cgroup: %s", strerror(errno));
        return -1;
    }

    return 0;
}