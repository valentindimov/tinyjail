#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <alloca.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "utils.h"
#include "logging.h"
#include "cgroup.h"

static int configureCgroup(
    int cgroupPathFd,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams
) {
    // Set up delegation
    if (fchownat(cgroupPathFd, ".", uid, gid, 0) != 0) { tinyjailLogError("Failed to change ownership on cgroup: %s\n", strerror(errno)); return -1; }
    if (fchownat(cgroupPathFd, "cgroup.procs", uid, gid, 0) != 0) { tinyjailLogError("Failed to change ownership on cgroup.procs: %s\n", strerror(errno)); return -1; }
    if (fchownat(cgroupPathFd, "cgroup.subtree_control", uid, gid, 0) != 0) { tinyjailLogError("Failed to change ownership on cgroup.subtree_control: %s\n", strerror(errno)); return -1; }
    if (fchownat(cgroupPathFd, "cgroup.threads", uid, gid, 0) != 0) { tinyjailLogError("Failed to change ownership on cgroup.threads: %s\n", strerror(errno)); return -1; }

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
                tinyjailLogError("Invalid cgroups option: %s", filename);
                return -1;
            }
            if (tinyjailWriteFileAt(cgroupPathFd, filename, "%s", contents) != 0) {
                tinyjailLogError("Could not write %s to %s: %s", filename, contents, strerror(errno));
                return -1;
            }
        } else {
            // We did not find an '=' sign, the string was malformed
            tinyjailLogError("Malformed cgroup option: %s", *curOptPtr);
            return 1;
        }
    }

    // Move the child process to the cgroup
    if (tinyjailWriteFileAt(cgroupPathFd, "cgroup.procs", "%d", childPid) != 0) { tinyjailLogError("Failed to add process to cgroup cgroup: %s\n", strerror(errno)); return -1; }

    return 0;
}

int tinyjailSetupContainerCgroup(char* containerId, int childPid, unsigned int uid, unsigned int gid, struct tinyjailContainerParams* containerParams) {
    ALLOC_LOCAL_FORMAT_STRING(containerCgroupPath, "/sys/fs/cgroup/container_%s", containerId);
    if (mkdir(containerCgroupPath, 0770) != 0) { tinyjailLogError("Failed to create cgroup %s: %s\n", containerCgroupPath, strerror(errno)); } else {
        int cgroupPathFd = open(containerCgroupPath, 0);
        if (cgroupPathFd < 0) { tinyjailLogError("Failed to open cgroup %s: %s\n", containerCgroupPath, strerror(errno)); } else {
            if (configureCgroup(cgroupPathFd, childPid, uid, gid, containerParams) == 0) {
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