#include "cgroup.h"
#include "utils.h"

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

static int configureContainerCgroup(
    const char* cgroupfsMountPath,
    int childPid,
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    ALLOC_LOCAL_FORMAT_STRING(containerCgroupPath, "%s/%s", cgroupfsMountPath, containerParams->containerId);
    RAII_FD cgroupPathFd = open(containerCgroupPath, 0);
    if (cgroupPathFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open cgroup %s: %s.", containerCgroupPath, strerror(errno));
        return -1;
    }
    // Set up delegation
    if (fchownat(cgroupPathFd, ".", containerParams->uid, containerParams->gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.procs", containerParams->uid, containerParams->gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.procs: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.subtree_control", containerParams->uid, containerParams->gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.subtree_control: %s", strerror(errno));
        return -1; 
    }
    if (fchownat(cgroupPathFd, "cgroup.threads", containerParams->uid, containerParams->gid, 0) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not delegate container cgroup.threads: %s", strerror(errno));
        return -1; 
    }

    // Apply cgroup configuration options
    for (char** curOptPtr = containerParams->cgroupOptions; *curOptPtr != NULL; curOptPtr++) {
        // Make a copy of the option and make sure it's null-terminated
        // Later on we'll replace the first "=" in this copy with a NULL.
        // The first part (before the NULL) will be the filename in the cgroup folder
        // The second part (after the NULL) will be the contents to write there
        ALLOC_LOCAL_FORMAT_STRING(curOptCopy, "%s", *curOptPtr);
        char* filename;
        char* contents;
        if (splitString(curOptCopy, &filename, &contents, '=') != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Malformed cgroup option: %s (missing =?)", filename);
            return -1;
        }
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

int setupContainerCgroup(
    int childPid,
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    if (mount("none", containerParams->containerDir, "cgroup2", 0, NULL) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not mount cgroupfs: %s", strerror(errno));
        return -1;
    }
    // Make sure we unmount the cgroup2 mount, otherwise the cgroup cleanup won't work
    int configureCgroupResult = configureContainerCgroup(containerParams->containerDir, childPid, containerParams, result);
    int umount2Result = umount2(containerParams->containerDir, MNT_DETACH);
    int umount2Errno = errno;
    if (configureCgroupResult != 0) {
        return -1;
    }
    if (umount2Result != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not umount temporary cgroupfs mount: %s", strerror(umount2Errno));
        return -1;
    }

    return 0;
}
