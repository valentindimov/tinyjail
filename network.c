#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/syscall.h>

#include "utils.h"
#include "network.h"

// I would love to do all of this without invoking system(), but doing so means talking to the kernel directly via rtnetlink.
// Unfortunately rtnetlink is, to say it in the nicest way I can, severely underdocumented. The documentation alone is insufficient even for basic operations,
// and for basically all of the operations below, you'd have to dig through kernel code or other open source programs' code to figure out what you need to do.
// The fact that Rust has an easy-to-use library for that same purpose proves that Rust is superior in every way: https://docs.rs/rtnetlink/latest/rtnetlink/

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
        result->errorInfo = "Could not switch network namespace (setns() failed).";
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
        result->errorInfo = "Failed to get FD of my own network namespace. Is /proc mounted?";
    } else {
        int childPidFd = syscall(SYS_pidfd_open, childPid, 0);
        if (myNetNsFd < 0) {
            result->errorInfo = "Could not get a handle on child process (pidfd_open() failed).";
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