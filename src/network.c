#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

#include "utils.h"
#include "network.h"

// TODO: Challenge accepted (and postponed until I have time to learn how to use rtnetlink), do all of this without invoking the iproute2 tool

static int createVethPair(char* if1, char* if2) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link add dev %s type veth peer %s", if1, if2);
    return system(command);
}

static int setMasterOfInterface(char* interface, char* master) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s master %s", interface, master);
    return system(command);
}

static int enableInterface(char* interface) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s up", interface);
    return system(command);
}

static int moveInterfaceToNamespaceByFd(char* interface, int fd) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip link set %s netns /proc/self/fd/%d", interface, fd);
    return system(command);
}

static int addAddressToInterface(char* interface, char* address) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip addr add %s dev %s", address, interface);
    return system(command);
}

static int addDefaultRoute(char* targetAddress, char* targetInterface) {
    ALLOC_LOCAL_FORMAT_STRING(command, "ip route add default via %s dev %s", targetAddress, targetInterface);
    return system(command);
}

static int configureNetwork(
    int childPidFd,
    int myNetNsFd,
    const char* containerId,
    struct tinyjailContainerParams *params,
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
        if (addDefaultRoute(params->networkDefaultRoute, vethNameInside) != 0) {
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

int tinyjailSetupContainerNetwork(
    int childPid, 
    char* containerId, 
    struct tinyjailContainerParams *params,
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