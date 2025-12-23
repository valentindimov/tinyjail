// SPDX-License-Identifier: MIT

// _GNU_SOURCE is needed for setns(), unshare(), clone(), and namespace-related flags for clone().
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include "network.h"
#include "utils.h"

static int createVethPair(int netlinkSocket, char* if1, char* if2) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link add dev %s type veth peer %s", if1, if2);
    return system(command);
}

static int setMasterOfInterface(int netlinkSocket, char* interface, char* master) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s master %s", interface, master);
    return system(command);
}

static int enableInterface(int netlinkSocket, char* interface) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s up", interface);
    return system(command);
}

static int moveInterfaceToNamespaceByFd(int netlinkSocket, const char* procfsPath, char* interface, int fd) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s netns %s/self/fd/%d", interface, procfsPath, fd);
    return system(command);
}

static int addAddressToInterface(int netlinkSocket, char* interface, char* address) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip addr add %s dev %s", address, interface);
    return system(command);
}

static int addDefaultRouteToInterface(int netlinkSocket, char* targetAddress, char* targetInterface) {
    // TODO: Do this without using the iproute2 tool (use rtnetlink directly?)
    ALLOC_LOCAL_FORMAT_STRING(command, "ip route add default via %s dev %s", targetAddress, targetInterface);
    return system(command);
}

static int configureNetwork(
    const char* procfsPath,
    int childPidFd,
    int myNetNsFd,
    const struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    // RTNETLINK socket created here and closed when exiting this function
    RAII_FD netlinkSocket = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (netlinkSocket <= 0) {
        netlinkSocket = -1;
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "RTNETLINK socket() failed: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_nl bindInfo = {
        .nl_family = AF_NETLINK,
        .nl_pad = 0,
        .nl_pid = getpid(),
        .nl_groups = 0
    };
    if (bind(netlinkSocket, (struct sockaddr*) &bindInfo, sizeof(bindInfo)) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "RTNETLINK bind() failed: %s", strerror(errno));
        return -1;
    }

    // Create the vEth pair -inside- the container, then move it outside of it by using the parent PID as the namespace PID.
    // This saves us from having to delete the interface to clean up - when the container dies, the interface is automatically cleaned up.
    ALLOC_LOCAL_FORMAT_STRING(vethNameInside, "i_%s", params->containerId);
    ALLOC_LOCAL_FORMAT_STRING(vethNameOutside, "o_%s", params->containerId);

    if (setns(childPidFd, CLONE_NEWNET) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "setns() to enter the container network namespace failed: %s", strerror(errno));
        return -1;
    }
    if (createVethPair(netlinkSocket, vethNameInside, vethNameOutside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to create vEth pair %s-%s.", vethNameOutside, vethNameInside);
        return -1;
    }
    if (moveInterfaceToNamespaceByFd(netlinkSocket, procfsPath, vethNameOutside, myNetNsFd) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to move interface %s to outside network namespace.", vethNameOutside);
        return -1;
    }
    if (enableInterface(netlinkSocket, vethNameInside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to enable inside interface %s.", vethNameInside);
        return -1;
    }
    if (params->networkIpAddr) {
        if (addAddressToInterface(netlinkSocket, vethNameInside, params->networkIpAddr) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add address %s to inside interace %s.", params->networkIpAddr, vethNameInside);
            return -1;
        }
    }
    if (params->networkDefaultRoute) {
        if (addDefaultRouteToInterface(netlinkSocket, params->networkDefaultRoute, vethNameInside) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add default route %s to inside interace %s.", params->networkDefaultRoute, vethNameInside);
            return -1;
        }
    }
    if (setns(myNetNsFd, CLONE_NEWNET) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "setns() to go back to the host network namespace failed: %s", strerror(errno));
        return -1;
    }

    if (params->networkPeerIpAddr) {
        if (addAddressToInterface(netlinkSocket, vethNameOutside, params->networkPeerIpAddr) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not add address %s to outside interace %s.", params->networkPeerIpAddr, vethNameOutside);
            return -1;
        }
    }
    if (params->networkBridgeName) {
        if (setMasterOfInterface(netlinkSocket, vethNameOutside, params->networkBridgeName) != 0) {
            snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not attach outside interace %s to bridge %s.", vethNameOutside, params->networkBridgeName);
            return -1;
        }
    }
    if (enableInterface(netlinkSocket, vethNameOutside) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Failed to enable outside interface %s.", vethNameOutside);
        return -1;
    }
    return 0;
}

static int setupContainerNetworkInner(
    const char* procfsPath,
    int childPid, 
    const struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    ALLOC_LOCAL_FORMAT_STRING(myNetNsPath, "%s/self/ns/net", procfsPath);
    // Get a handle on both the inside and outside network namespaces
    RAII_FD myNetNsFd = open(myNetNsPath, O_RDONLY);
    if (myNetNsFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not open network NS from procfs: %s", strerror(errno));
        return -1;
    }
    RAII_FD childPidFd = syscall(SYS_pidfd_open, childPid, 0);
    if (childPidFd < 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "pidfd_open() on child PID failed: %s", strerror(errno));
        return -1;
    }
    int retval = configureNetwork(params->containerDir, childPidFd, myNetNsFd, params, result);
    // Make sure we're in our own network namespace even if we failed
    setns(myNetNsFd, CLONE_NEWNET);
    return retval;
}

int setupContainerNetwork(
    int childPid, 
    const struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
) {
    // If we're using the host network namespace, skip network setup completely
    if (params->useHostNetwork) {
        return 0;
    }
    
    if (mount("proc", params->containerDir, "proc", 0, NULL) != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not mount temporary procfs: %s", strerror(errno));
        return -1;
    }
    int configureUsernsResult = setupContainerNetworkInner(params->containerDir, childPid, params, result);
    int umount2Result = umount2(params->containerDir, MNT_DETACH);
    int umount2Errno = errno;
    if (configureUsernsResult != 0) {
        return -1;
    }
    if (umount2Result != 0) {
        snprintf(result->errorInfo, ERROR_INFO_SIZE, "Could not umount temporary procfs mount: %s", strerror(umount2Errno));
        return -1;
    }
    return 0;
}
