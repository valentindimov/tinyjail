// SPDX-License-Identifier: MIT

// _GNU_SOURCE is needed for setns(), unshare(), clone(), and namespace-related flags for clone().
#define _GNU_SOURCE

#include <alloca.h>
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

#include "tinyjail.h"

/// @brief Allocates a locally-scoped (via alloca()) string using the provided format.
#define ALLOC_LOCAL_FORMAT_STRING(VARNAME, FORMAT, ...) \
    int len##VARNAME = snprintf("", 0, FORMAT, __VA_ARGS__); \
    char* VARNAME = alloca((len##VARNAME + 1) * sizeof(char)); \
    snprintf(VARNAME, len##VARNAME + 1, FORMAT, __VA_ARGS__);

/// @brief Closes a fire descriptor and sets it to -1 if it is nonnegative. If it is negative, does nothing.
/// @param fd Pointer to the file descriptor variable
static void closep(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/// @brief Splits an input string into two output strings at a delmiter character. Allocates no memory, but modifies the input string by setting delimiter bytes to NULL bytes.
/// @param input The input string. It will be modified by this function.
/// @param output_1 Output: The part of the input before the delimiter. Output undefined if the function fails.
/// @param output_2 Output: The part of the input after the delimiter. Output undefined if the function fails.
/// @param delim The delimiter character
/// @return 0 if the string could be split, -1 if the split was unsuccessful (e.g. because the delimiter character was not found)
static int splitString(char* input, char** output_1, char** output_2, char delim) {
    if (input == NULL) {
        return -1;
    }
    // Start from the beginning of the string and iterate until the string ends, or we find '='
    *output_1 = input;
    *output_2 = input;
    while (**output_2 != '\0' && **output_2 != delim) {
        *output_2 += 1;
    }
    if (**output_2 == delim) {
        // Found the delimiter, replace it with a NULL and set output_2 to the remainder of the string.
        **output_2 = '\0';
        *output_2 += 1;
        return 0;
    } else {
        // We did not find an the delimiter
        return -1;
    }
}

/// @brief Defines a special type of FD that gets automatically closed when it exits scope. Use closep() to close it earlier, that function is idempotent.
/// Unfortunately this is a non-standard extension, so it will only compile with gcc or clang. But it's so useful for simplifying error handling, it's worth losing that portability...
#define RAII_FD __attribute__((cleanup(closep))) int

/// @brief Checks if a given string represents a filename (not a path) which is not "." or ".."
/// @param filename The filename to check
/// @return 1 if the string represents a regular filename, and 0 if it does not.
/// Generally you can use this function to see if a user-supplied filename is safe to use.
/// The function will reject filenames which can cause path traversal.
static int stringIsRegularFilename(const char* filename) {
    // Disallow empty strings and the special filenames "." and ".."
    if (*filename == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        return 0;
    }
    // Reject any string containing slashes
    while(*filename) {
        if (*filename == '/') {
            return 0;
        }
        filename++;
    }
    return 1;
}

static int configureContainerCgroup(
    const char* cgroupfsMountPath,
    const char* containerId,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
) {
    // TODO remove this
    ALLOC_LOCAL_FORMAT_STRING(containerCgroupPath, "%s/%s", cgroupfsMountPath, containerId);
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

static int configureContainerUserNamespace(
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

static int createVethPair(char* if1, char* if2) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link add dev %s type veth peer %s", if1, if2);
    return system(command);
}

static int setMasterOfInterface(char* interface, char* master) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s master %s", interface, master);
    return system(command);
}

static int enableInterface(char* interface) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s up", interface);
    return system(command);
}

static int moveInterfaceToNamespaceByFd(char* interface, int fd) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s netns /proc/self/fd/%d", interface, fd);
    return system(command);
}

static int addAddressToInterface(char* interface, char* address) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip addr add %s dev %s", address, interface);
    return system(command);
}

static int addDefaultRouteToInterface(char* targetAddress, char* targetInterface) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip route add default via %s dev %s", targetAddress, targetInterface);
    return system(command);
}

static int configureNetwork(
    int childPidFd,
    int myNetNsFd,
    const char* containerId,
    const struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    // Create the vEth pair -inside- the container, then move it outside of it by using the parent PID as the namespace PID.
    // This saves us from having to delete the interface to clean up - when the container dies, the interface is automatically cleaned up.
    ALLOC_LOCAL_FORMAT_STRING(vethNameInside, "i_%s", containerId);
    ALLOC_LOCAL_FORMAT_STRING(vethNameOutside, "o_%s", containerId);

    if (setns(childPidFd, CLONE_NEWNET) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "setns() to enter the container network namespace failed: %s", strerror(errno));
        return -1;
    }
    if (createVethPair(vethNameInside, vethNameOutside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to create vEth pair %s-%s.", vethNameOutside, vethNameInside);
        return -1;
    }
    if (moveInterfaceToNamespaceByFd(vethNameOutside, myNetNsFd) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to move interface %s to outside network namespace.", vethNameOutside);
        return -1;
    }
    if (enableInterface(vethNameInside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to enable inside interface %s.", vethNameInside);
        return -1;
    }
    if (params->networkIpAddr) {
        if (addAddressToInterface(vethNameInside, params->networkIpAddr) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add address %s to inside interace %s.", params->networkIpAddr, vethNameInside);
            return -1;
        }
    }
    if (params->networkDefaultRoute) {
        if (addDefaultRouteToInterface(params->networkDefaultRoute, vethNameInside) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add default route %s to inside interace %s.", params->networkDefaultRoute, vethNameInside);
            return -1;
        }
    }
    if (setns(myNetNsFd, CLONE_NEWNET) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "setns() to go back to the host network namespace failed: %s", strerror(errno));
        return -1;
    }

    if (params->networkPeerIpAddr) {
        if (addAddressToInterface(vethNameOutside, params->networkPeerIpAddr) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add address %s to outside interace %s.", params->networkPeerIpAddr, vethNameOutside);
            return -1;
        }
    }
    if (params->networkBridgeName) {
        if (setMasterOfInterface(vethNameOutside, params->networkBridgeName) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not attach outside interace %s to bridge %s.", vethNameOutside, params->networkBridgeName);
            return -1;
        }
    }
    if (enableInterface(vethNameOutside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to enable outside interface %s.", vethNameOutside);
        return -1;
    }
    return 0;
}

static int setupContainerNetwork(
    int childPid, 
    char* containerId, 
    const struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    // Get a handle on both the inside and outside network namespaces
    RAII_FD myNetNsFd = open("/proc/self/ns/net", O_RDONLY);
    if (myNetNsFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open /proc/self/ns/net: %s", strerror(errno));
        return -1;
    }
    RAII_FD childPidFd = syscall(SYS_pidfd_open, childPid, 0);
    if (childPidFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "pidfd_open() on child PID failed: %s", strerror(errno));
        return -1;
    }
    int retval = configureNetwork(childPidFd, myNetNsFd, containerId, params, result);
    // Make sure we're in our own network namespace even if we failed
    setns(myNetNsFd, CLONE_NEWNET);
    return retval;
}

struct ContainerInitArgs {
    char* containerDir;
    char** commandList;
    char** environment;
    char* workDir;
    // Pipe used by the parent to signal to the child that its namespaces are initialized and it may execve() now.
    int syncPipeWrite;
    int syncPipeRead;
    // Pipe used by the child to send error messages to the parent.
    int errorPipeWrite;
    int errorPipeRead;
};

/// @brief Runs the initial part of the container init process. Runs in a separate process.
/// @param args Arguments for container initialization
/// @return Nothing if it gets to execve()-ing the container entrypoint, otherwise returns -1 on failure.
static int runContainerInit(struct ContainerInitArgs *args) {
#define RETURN_WITH_ERROR(...) { ALLOC_LOCAL_FORMAT_STRING(error, __VA_ARGS__); write(args->errorPipeWrite, error, strlen(error)); return -1; }

    // We won't need the writing end of the sync pipe (and in case the parent crashes, we want to avoid being stuck waiting on ourselves)
    close(args->syncPipeWrite);
    close(args->errorPipeRead);
    
    // Wait to get a message "OK" over the sync pipe. Only if we get that are we sure that our parent has initialized everything.
    char result[2];
    if (read(args->syncPipeRead, result, 2) != 2 || strncmp(result, "OK", 2) != 0) {
        RETURN_WITH_ERROR("Child could not read() on sync pipe: %s", strerror(errno));
    }
    close(args->syncPipeRead);

    // Set the container init process to be a subreaper, since most init processes expect it.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        RETURN_WITH_ERROR("Could not set container init as subreaper: %s", strerror(errno));
    }

    // Unshare the cgroup namespace here (after our parent has had the chance to move us to our cgroup)
    if (unshare(CLONE_NEWCGROUP) != 0) {
        RETURN_WITH_ERROR("Unsharing cgroup namespace in child failed: %s", strerror(errno));
    }

    // Set our UID and GID.
    if (setuid(0) != 0 || setgid(0) != 0) {
        RETURN_WITH_ERROR("Child could not switch UID or GID: %s", strerror(errno));
    }

    // Pivot to the filesystem root
    if (mount(args->containerDir, args->containerDir, "none", MS_BIND | MS_PRIVATE | MS_REC | MS_NOSUID, NULL) != 0) {
        RETURN_WITH_ERROR("Could not bind-mount container roor dir: %s", strerror(errno));
    }
    if (chdir(args->containerDir) != 0) {
        RETURN_WITH_ERROR("Child could not chdir to container roor dir: %s", strerror(errno));
    }
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        RETURN_WITH_ERROR("Child could not pivot_root to container roor dir: %s", strerror(errno));
    }
    if (umount2(".", MNT_DETACH) != 0) {
        RETURN_WITH_ERROR("Child could not unmount old root dir: %s", strerror(errno));
    }

    // If a working directory was set, make sure to set that before execve-ing
    if (args->workDir != NULL && chdir(args->workDir) != 0) {
        RETURN_WITH_ERROR("Child could not chdir to chosen workdir: %s", strerror(errno));
    }

    // Make sure that if we successfully execve(), the errorPipeWrite is closed
    if (fcntl(args->errorPipeWrite, F_SETFD, FD_CLOEXEC) < 0) {
        RETURN_WITH_ERROR("fcntl() on error pipe failed: %s", strerror(errno));
    }

    // All good, execute the target command.
    execve(args->commandList[0], (args->commandList + 1), args->environment);

    // If we got here, the execve() call failed.
    RETURN_WITH_ERROR("execve() failed: %s", strerror(errno));

#undef RETURN_WITH_ERROR
}

static int finishConfiguringAndAwaitContainerProcess(
    const struct tinyjailContainerParams *containerParams,
    struct tinyjailContainerResult *result,
    char* containerId,
    int childPid,
    int uid,
    int gid,
    int syncPipeWrite,
    int errorPipeRead
) {
    if (mount("none", containerParams->containerDir, "cgroup2", 0, NULL) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not mount cgroupfs: %s", strerror(errno));
        return -1;
    }
    if (configureContainerCgroup(containerParams->containerDir, containerId, childPid, uid, gid, containerParams, result) != 0) {
        return -1;
    }
    if (umount2(containerParams->containerDir, MNT_DETACH) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not umount temporary cgroupfs mount: %s", strerror(errno));
        return -1;
    }
    if (configureContainerUserNamespace(childPid, uid, gid, result) != 0) {
        return -1;
    }
    if (setupContainerNetwork(childPid, containerId, containerParams, result) != 0) {
        return -1;
    }
    if (write(syncPipeWrite, "OK", 2) != 2) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not give the child the go-ahead signal: %s", strerror(errno));
        return -1;
    }
    if (read(errorPipeRead, result->errorInfo, ERROR_INFO_SIZE - 1) > 0) {
        return -1;
    }
    if (waitpid(childPid, &(result->containerExitStatus), __WALL) < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "waitpid() failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/// @brief Runs the container launcher logic, in a separate subprocess.
/// @param containerParams Input arg: the parameters for launching the container
/// @param result Output arg: the result of the launch is written here
static void runContainerLauncher(const struct tinyjailContainerParams *containerParams, struct tinyjailContainerResult *result) {
#define RETURN_WITH_ERROR(...) result->containerStartedStatus = -1; snprintf(result->errorInfo, ERROR_INFO_SIZE, __VA_ARGS__); return;

    if (getuid() != 0) {
        RETURN_WITH_ERROR("tinyjail requires root permissions to run.");
    }

    // The container launcher already sets up the mounts for the container, so it runs in its own mount namespace.
    if (unshare(CLONE_NEWNS) != 0) {
        RETURN_WITH_ERROR("Unsharing cgroup namespace in child failed: %s", strerror(errno));
    }
    // Transition shared mounts to private mounts to prevent anything we do with mounts from propagating outside of the countainer
    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        RETURN_WITH_ERROR("Could not set all mounts to private: %s", strerror(errno));
    }

    // Validate container parameters
    if (containerParams->containerId && strlen(containerParams->containerId) > 12) {
        RETURN_WITH_ERROR("containerId can be at most 12 characters long.");
    }
    if (!containerParams->commandList) {
        RETURN_WITH_ERROR("containerParams missing required parameter: commandList.");
    }
    if (!containerParams->containerDir) {
        RETURN_WITH_ERROR("containerParams missing required parameter: containerDir.");
    }
    if (!containerParams->environment) {
        RETURN_WITH_ERROR("containerParams missing required parameter: environment.");
    }
    if (containerParams->networkBridgeName && containerParams->networkPeerIpAddr) {
        RETURN_WITH_ERROR("containerParams cannot have both networkBridgeName and networkPeerIPAddr set.");
    }
    char resolvedRootPath[(PATH_MAX + 1) * sizeof(char)];
    memset(resolvedRootPath, 0, sizeof(resolvedRootPath));
    if (realpath(containerParams->containerDir, resolvedRootPath) == NULL) {
        RETURN_WITH_ERROR("Could not resolve path %s: %s", containerParams->containerDir, strerror(errno));
    }
    if (strcmp(resolvedRootPath, "/") == 0) {
        RETURN_WITH_ERROR("Container root dir cannot be /");
    }

    // Determine the UID and GID for the container as the owner of the container directory
    struct stat containerDirStat;
    if (stat(containerParams->containerDir, &containerDirStat) != 0) {
        RETURN_WITH_ERROR("Could not stat %s: %s", containerParams->containerDir, strerror(errno));
    }
    unsigned int uid = containerDirStat.st_uid;
    unsigned int gid = containerDirStat.st_gid;
    
    // Set up the sync pipe for signalling the child process to begin execution, and one for passing error messages back
    int syncPipe[2] = { -1, -1 };
    int errorPipe[2] = { -1, -1 };
    int pipeSuccess = (pipe(syncPipe) == 0 && pipe(errorPipe) == 0);
    RAII_FD syncPipeRead = syncPipe[0];
    RAII_FD syncPipeWrite = syncPipe[1];
    RAII_FD errorPipeRead = errorPipe[0];
    RAII_FD errorPipeWrite = errorPipe[1];
    if (!pipeSuccess) {
        RETURN_WITH_ERROR("pipe() failed: %s", strerror(errno));
    }

    // Set tinyjail itself as a subreaper, so that if the container init dies, we are the ones vacuuming up leftover children.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        RETURN_WITH_ERROR("Could not set container init as subreaper: %s", strerror(errno));
    }

    // Set up the container's mounts
    // TODO

    // Start the child process and close the read end of the sync pipe (it is for the child process only)
    // Do not unshare the cgroup namespace just yet - the subprocess will do this, after we have moved it to the right cgroup. 
    struct ContainerInitArgs args = {
        .containerDir = containerParams->containerDir,
        .commandList = containerParams->commandList,
        .environment = containerParams->environment,
        .workDir = containerParams->workDir,
        .syncPipeRead = syncPipeRead,
        .syncPipeWrite = syncPipeWrite,
        .errorPipeRead = errorPipeRead,
        .errorPipeWrite = errorPipeWrite
    };
    int cloneFlags = (CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWUSER | CLONE_NEWTIME | CLONE_NEWNET | SIGCHLD);
    // The stack memory of the child is a local 4K buffer allocated in this function. 
    // This should be enough, but in either case, the child has its own memory map so even if it overruns the buffer, it shouldn't cause problems for us.
    int childPid = clone((int (*)(void *)) runContainerInit, (void*) (((char*) alloca(4096)) + 4095), cloneFlags, (void*) &args);
    if (childPid < 0) {
        RETURN_WITH_ERROR("clone() failed: %s", strerror(errno));
    }
    closep(&syncPipeRead); // closep() is idempotent because it also sets the FD variable to -1
    closep(&errorPipeWrite); // closep() is idempotent because it also sets the FD variable to -1

    // The container ID should be less than 12 characters long since it's used to generate names for the network interfaces (which are capped at 15 characters)
    ALLOC_LOCAL_FORMAT_STRING(containerId, "tj_%d", childPid & 0xffffffff); // childPid being a 32-bit integer, the ID will be at most 12 characters long.
    if (containerParams->containerId != NULL) {
        containerId = containerParams->containerId;
    }

    // Create a cgroup for the child process. Because we run in our own mount namespace here, we don't need to worry about cleaning up temporary mounts on failure
    {
        if (mount("none", containerParams->containerDir, "cgroup2", 0, NULL) != 0) {
            RETURN_WITH_ERROR("Could not mount cgroupfs: %s", strerror(errno));
        }
        ALLOC_LOCAL_FORMAT_STRING(cgroupPath, "%s/%s", containerParams->containerDir, containerId);
        if (mkdir(cgroupPath, 0770) != 0) {
            RETURN_WITH_ERROR("Could not create cgroup: %s.", strerror(errno));
        }
        if (umount2(containerParams->containerDir, MNT_DETACH) != 0) {
            RETURN_WITH_ERROR("Could not umount temporary cgroupfs mount: %s", strerror(errno));
        }
    }
    // There is only one return point from this point on, so we're sure we will delete the container cgroup before returning.

    if (finishConfiguringAndAwaitContainerProcess(containerParams, result, containerId, childPid, uid, gid, syncPipeWrite, errorPipeRead) != 0) {
        kill(childPid, SIGKILL);
        // The subroutines should have set an error message already
        result->containerStartedStatus = -1;
    }

    // Success. Now attempt final cleanup...
    // Make sure to vacuum up any leftover child processes
    int tmp;
    while (wait(&tmp) > 0) {}
    // Make sure to remove the cgroup
    // TODO: This might not work if the container cgroup has child cgroups. Maybe we should have a more sophisticated recursive delete procedure here.
    if (mount("none", containerParams->containerDir, "cgroup2", 0, NULL) == 0) {
        ALLOC_LOCAL_FORMAT_STRING(cgroupPath, "%s/%s", containerParams->containerDir, containerId);
        rmdir(cgroupPath);
        umount2(containerParams->containerDir, MNT_DETACH);
    }

    return;

#undef RETURN_WITH_ERROR
}

struct tinyjailContainerResult tinyjailLaunchContainer(const struct tinyjailContainerParams containerParams) {
    struct tinyjailContainerResult result = {0};

#define RETURN_WITH_ERROR(...) result.containerStartedStatus = -1; snprintf(result.errorInfo, ERROR_INFO_SIZE, __VA_ARGS__); return result;

    // Since we'll pipe in the result of the container launch, set up the pipe first
    int resultPipe[2] = { -1, -1 };
    if (pipe(resultPipe) != 0) {
        RETURN_WITH_ERROR("pipe() failed: %s", strerror(errno));
    }
    RAII_FD resultPipeRead = resultPipe[0];
    RAII_FD resultPipeWrite = resultPipe[1];

    // Now run the container launch function in a subprocess.
    int launcherPid = fork();
    if (launcherPid < 0) {
        RETURN_WITH_ERROR("fork() failed: %s", strerror(errno));
    } else if (launcherPid == 0) {
        // Child process logic goes here
        closep(&resultPipeRead);
        runContainerLauncher(&containerParams, &result);
        write(resultPipeWrite, &result, sizeof(result));
        closep(&resultPipeWrite);
        exit(0);
    } else {
        closep(&resultPipeWrite);
        size_t readResult = read(resultPipeRead, &result, sizeof(result));
        int readResultErrno = errno;
        closep(&resultPipeRead);
        int launcherExitCode;
        int launcherWaitpid = waitpid(launcherPid, &launcherExitCode, __WALL);
        int launcherWaitpidErrno = errno;
        if (readResult != sizeof(result)) {
            RETURN_WITH_ERROR("Could not read() result back from launcher: %s", strerror(readResultErrno));
        } else if (launcherWaitpid < 0) {
            RETURN_WITH_ERROR("Could not waitpid() on launcher: %s", strerror(launcherWaitpidErrno));
        } else {
            return result;
        }
    }

#undef RETURN_WITH_ERROR
}