#pragma once

/// @brief Encapsulates all parameters used to run a container process.
struct tinyjailContainerParams {
    /// @brief Path to the root directory of the container. Should be writeable.
    char* containerDir;
    /// @brief argv (NULL-terminated array of command args) for the container init process.
    char** commandList;
    /// @brief envp (NULL-terminated list of KEY=VALUE strings) for the container init process
    char** environment;
    /// @brief Working directory for the container init process. If set to NULL, it will be "/"
    char* workDir;
    /// @brief Colon-separated list of read-only directories to overlay the container FS over. Set to NULL for no overlaying.
    // TODO implement this
    char* lowerDirs; // sudo mount -t overlay overlay -olowerdir=./lower:./lower2,upperdir=./upper,workdir=./work ./upper

    /// @brief Maximum CPU Percentage that the container can use, 1-100. 0 means no limit.
    unsigned long cpuMaxPercent;
    /// @brief Proportional weight of CPU usage allocated to the container (100-10000). 0 means none.
    unsigned long cpuWeight;
    /// @brief Soft maximum of the memory the container may use (in bytes). O means no limit.
    unsigned long memoryHigh;
    /// @brief Hard maximum of the memory the container may use (in bytes). 0 means no limit.
    unsigned long memoryMax;
    /// @brief Maximum number of tasks (PIDs) allowed inside the container. 0 means no limit.
    unsigned long pidsMax;

    /// @brief If networkBridgeName is not NULL, create a vEth interface in the container and connect it to this bridge.
    char* networkBridgeName;
    /// @brief If networkIpAddr is not NULL, set the container's vEth interface IP address to this. Requires networkBridgeName to be specified.
    char* networkIpAddr;
    /// @brief If networkDefaultRoute is not NULL, set the default route of the container's vEth interface to the given destination. Requires networkBridgeName to be specified.
    char* networkDefaultRoute;
};

extern void (*tinyjailErrorLogFunc)(const char*);

__attribute__ ((visibility ("default"))) int tinyjailLaunchContainer(struct tinyjailContainerParams programArgs);
