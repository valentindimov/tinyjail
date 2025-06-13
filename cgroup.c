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

    // Set up resource limits
    if (containerParams->cpuMaxPercent > 0) {
        if (tinyjailWriteFileAt(cgroupPathFd, "cpu.max", "%lu000 100000", containerParams->cpuMaxPercent) != 0) { tinyjailLogError("Failed to set up cpu.max in cgroup: %s\n", strerror(errno)); return -1; }
    }
    if (containerParams->cpuWeight > 0) {
        if (tinyjailWriteFileAt(cgroupPathFd, "cpu.weight", "%lu", containerParams->cpuWeight) != 0) { tinyjailLogError("Failed to set up cpu.weight in cgroup: %s\n", strerror(errno)); return -1; }
    }
    if (containerParams->memoryHigh > 0) {
        if (tinyjailWriteFileAt(cgroupPathFd, "memory.high", "%lu", containerParams->memoryHigh) != 0) { tinyjailLogError("Failed to set up memory.high in cgroup: %s\n", strerror(errno)); return -1; }
    }
    if (containerParams->memoryMax > 0) {
        if (tinyjailWriteFileAt(cgroupPathFd, "memory.max", "%lu", containerParams->memoryMax) != 0) { tinyjailLogError("Failed to set up memory.max in cgroup: %s\n", strerror(errno)); return -1; }
    }
    if (containerParams->pidsMax > 0) {
        if (tinyjailWriteFileAt(cgroupPathFd, "pids.max", "%lu", containerParams->pidsMax) != 0) { tinyjailLogError("Failed to set up pids.max in cgroup: %s\n", strerror(errno)); return -1; }
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