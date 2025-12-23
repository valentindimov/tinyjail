// SPDX-License-Identifier: MIT

// Needed for DT_DIR
#define _GNU_SOURCE

#include "cgroup.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

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

void deleteCgroupDir(
    const char* path
) {
    // When clearing cgroups, we should make sure to delete child cgroups first.
    // Effectively this means to recursively delete all subdirectories before this one.
    // We could use nftw() here but it traverses in the wrong order - root first, children after.
    DIR *openedDir = opendir(path);
    if (openedDir != NULL) {
        struct dirent *entry;
        while((entry = readdir(openedDir)) != NULL) {
            if (entry->d_type == DT_DIR) {
                ALLOC_LOCAL_FORMAT_STRING(subdirPath, "%s/%s", path, entry->d_name);
                deleteCgroupDir(subdirPath);
            }
        }
        closedir(openedDir);
    }
    rmdir(path);
}

void cleanContainerCgroup(
    const struct tinyjailContainerParams* containerParams
) {
    if (mount("none", containerParams->containerDir, "cgroup2", 0, NULL) == 0) {
        ALLOC_LOCAL_FORMAT_STRING(cgroupPath, "%s/%s", containerParams->containerDir, containerParams->containerId);
        deleteCgroupDir(cgroupPath);
        umount2(containerParams->containerDir, MNT_DETACH);
    }
}
