#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <alloca.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "utils.h"
#include "cgroup.h"

static int configureCgroup(
    int cgroupPathFd,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    // Set up delegation
    if (fchownat(cgroupPathFd, ".", uid, gid, 0) != 0) {
        result->errorInfo = "Failed to change the owner of the container cgroup";
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.procs", uid, gid, 0) != 0) { 
        result->errorInfo = "Failed to change the owner of the container cgroup.procs";
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.subtree_control", uid, gid, 0) != 0) {
        result->errorInfo = "Failed to change the owner of the container cgroup.subtree_control";
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.threads", uid, gid, 0) != 0) {
        result->errorInfo = "Failed to change the owner of the container cgroup.threads";
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
            if (!stringIsRegularFilename(filename)) {
                // Make sure we only try writing to files in the cgroup directory
                result->errorInfo = "You specified an invalid cgroups option name.";
                return -1;
            }
            if (tinyjailWriteFileAt(cgroupPathFd, filename, "%s", contents) != 0) {
                result->errorInfo = "Could not apply one of the cgroups options. Make sure that the option exists and belongs to a controller which is enabled in /sys/fs/cgroup/cgroup.subtree_control";
                return -1;
            }
        } else {
            // We did not find an '=' sign, the string was malformed
            result->errorInfo = "You specified a cgroups option without a value.";
            return 1;
        }
    }

    // Move the child process to the cgroup
    if (tinyjailWriteFileAt(cgroupPathFd, "cgroup.procs", "%d", childPid) != 0) { 
        result->errorInfo = "Could not move the container process to the container cgroup.";
        return -1; 
    }

    return 0;
}

int tinyjailSetupContainerCgroup(
    char* containerId, 
    int childPid, 
    unsigned int uid, 
    unsigned int gid, 
    struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult* result
) {
    ALLOC_LOCAL_FORMAT_STRING(containerCgroupPath, "/sys/fs/cgroup/container_%s", containerId);
    if (mkdir(containerCgroupPath, 0770) != 0) {
        result->errorInfo = "Could not create cgroup for the container. Make sure /sys/fs/cgroup is mounted.";
    } else {
        int cgroupPathFd = open(containerCgroupPath, 0);
        if (cgroupPathFd < 0) {
            result->errorInfo = "Created a cgroup, but could not open it. You might not have permission for it";
        } else {
            if (configureCgroup(cgroupPathFd, childPid, uid, gid, containerParams, result) == 0) {
                // SUCCESS CASE CLEANUP
                close(cgroupPathFd);
                return 0;
            }
            // FAILURE CASE CLEANUP
            close(cgroupPathFd);
        }
        rmdir(containerCgroupPath);
    }
    return -1;
}

int tinyjailDestroyCgroup(char* containerId) {
    ALLOC_LOCAL_FORMAT_STRING(containerCgroupPath, "/sys/fs/cgroup/container_%s", containerId);
    if (rmdir(containerCgroupPath) == 0) { 
        return 0;
    }
    return 1;
}