// SPDX-License-Identifier: MIT

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

    /// @brief UID map in the format: "<container UID> <host UID> <mapping size>".
    /// Use newlines to separate entries (TODO: semicolons should be supported as well)
    /// The default UID map maps the owner UID of the container root dir to container UID 0.
    char* uidMap;
    /// @brief Container UID for the container process (default: 0)
    unsigned long uid;
    /// @brief GID map in the format: "<container GID> <host GID> <mapping size>".
    /// Use newlines to separate entries (TODO: semicolons should be supported as well)
    /// The default GID map maps the owner GID of the container root dir to container GID 0.
    char* gidMap;
    /// @brief Container GID for the container process (default: 0)
    unsigned long gid;

    /// @brief NULL-terminated list of "filename=value" strings that specify cgroup options like resource limits.
    char** cgroupOptions;

    /// @brief Set to nonzero if the container should use the host network namespace. All other network options are ignored.
    int useHostNetwork;
    /// @brief If networkBridgeName is not NULL, set the master of the container's vEth interface to the given bridge.
    char* networkBridgeName;
    /// @brief If networkIpAddr is not NULL, set the container's vEth interface IP address to this.
    char* networkIpAddr;
    /// @brief If networkPeerIpAddr is not NULL, set IP address of the other end of the vEth pair (the one in the host namespace) to it.
    char* networkPeerIpAddr;
    /// @brief If networkDefaultRoute is not NULL, set the default route of the container's vEth interface to the given destination.
    char* networkDefaultRoute;

    /// @brief Sets the hostname inside the container. If set to NULL, it's set to "tinyjail".
    char* hostname;
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
