// SPDX-License-Identifier: MIT

#pragma once

/// @brief Encapsulates all parameters used to run a container process.
struct tinyjailContainerParams {
    /// @brief Optional explicit ID for the container. If left at NULL, a random ID is generated.
    char* containerId;

    /// @brief Path to the root directory of the container. Should be writeable.
    char* containerDir;
    /// @brief argv (NULL-terminated array of command args) for the container init process.
    char** commandList;
    /// @brief envp (NULL-terminated list of KEY=VALUE strings) for the container init process
    char** environment;

    /// @brief Working directory for the container init process. If set to NULL, it will be "/"
    char* workDir;

    /// @brief Host UID for the container to run as. If -1 is specified, the owner of the container root directory is used.
    unsigned int uid;
    /// @brief Host GID for the container to run as. If -1 is specified, the owner of the container root directory is used.
    unsigned int gid;

    /// @brief NULL-terminated list of "filename=value" strings that specify cgroup options like resource limits.
    char** cgroupOptions;

    /// @brief If networkBridgeName is not NULL, set the master of the container's vEth interface to the given bridge.
    char* networkBridgeName;
    /// @brief If networkIpAddr is not NULL, set the container's vEth interface IP address to this.
    char* networkIpAddr;
    /// @brief If networkPeerIpAddr is not NULL, set IP address of the other end of the vEth pair (the one in the host namespace) to it.
    char* networkPeerIpAddr;
    /// @brief If networkDefaultRoute is not NULL, set the default route of the container's vEth interface to the given destination.
    char* networkDefaultRoute;
};

// Try to keep this struct at 256 B
#define ERROR_INFO_SIZE (240)
struct tinyjailContainerResult {
    /// @brief Set to 0 if the container was started successfully, and nonzero otherwise
    int containerStartedStatus;
    /// @brief If the container started successfully, this stores its exit status (as written by waitpid()).
    int containerExitStatus;
    /// @brief Shost human-readable string with a more detailed error description, if available.
    char errorInfo[ERROR_INFO_SIZE];
};

__attribute__ ((visibility ("default"))) struct tinyjailContainerResult tinyjailLaunchContainer(
    struct tinyjailContainerParams programArgs
);
