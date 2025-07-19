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

static int enterNetworkNamespace(int namespaceFd, struct tinyjailContainerResult *result) {
    if (setns(namespaceFd, CLONE_NEWNET) != 0) {
        snprintf(
            result->errorInfo,
            ERROR_INFO_SIZE,
            "setns() for network namespace failed: %s",
            strerror(errno)
        );
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
    ALLOC_LOCAL_FORMAT_STRING(vethNameInside, "i_%s", containerId);
    ALLOC_LOCAL_FORMAT_STRING(vethNameOutside, "o_%s", containerId);

    // Create a veth pair for the container
    int myNetNsFd = open("/proc/self/ns/net", O_RDONLY);
    if (myNetNsFd < 0) {
        snprintf(
            result->errorInfo,
            ERROR_INFO_SIZE,
            "Could not open /proc/self/ns/net: %s",
            strerror(errno)
        );
    } else {
        int childPidFd = syscall(SYS_pidfd_open, childPid, 0);
        if (myNetNsFd < 0) {
            snprintf(
                result->errorInfo,
                ERROR_INFO_SIZE,
                "pidfd_open() on child PID failed: %s",
                strerror(errno)
            );
        } else {
            // Create the vEth pair -inside- the container, then move it outside of it by using the parent PID as the namespace PID.
            // This saves us from having to delete the interface to clean up - when the container dies, the interface is automatically cleaned up.
            if (
                enterNetworkNamespace(childPidFd, result) == 0
                && createVethPair(vethNameInside, vethNameOutside) == 0
                && moveInterfaceToNamespaceByFd(vethNameOutside, myNetNsFd) == 0
                && enableInterface(vethNameInside) == 0
                && (!params->networkIpAddr || addAddressToInterface(vethNameInside, params->networkIpAddr) == 0)
                && (!params->networkDefaultRoute || addDefaultRoute(params->networkDefaultRoute, vethNameInside) == 0)
                && enterNetworkNamespace(myNetNsFd, result) == 0
                && (!params->networkPeerIpAddr || addAddressToInterface(vethNameOutside, params->networkPeerIpAddr) == 0)
                && (!params->networkBridgeName || setMasterOfInterface(vethNameOutside, params->networkBridgeName) == 0)
                && enableInterface(vethNameOutside) == 0
            ) {
                // SUCCESS CASE CLEANUP
                close(childPidFd);
                close(myNetNsFd);
                return 0;
            }
            // Make sure we're in our own network namespace!
            setns(myNetNsFd, CLONE_NEWNET);
            // FAILURE CASE CLEANUP
            close(childPidFd);
        }
        close(myNetNsFd);
    }
    return -1;
}