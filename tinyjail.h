#pragma once

struct tinyjailContainerParams {
    // Run parameters of the container process
    char* containerDir; // Path to root directory of container
    char** commandList; // NULL-terminated argv for the container init process
    char** environment; // NULL-terminated envp for the container init process
    char* workDir; // Working directory for the container. Set to NULL to leave it unspecified.
    // TODO list of directories to overlay the root directory on top of (workdir in /tmp/...)
    // sudo mount -t overlay overlay -olowerdir=./lower:./lower2,upperdir=./upper,workdir=./work ./upper

    // Cgroup resource limits for the container
    unsigned long cpuMaxPercent; // Integer 1-100, or 0 if no limit
    unsigned long cpuWeight; // Integer 100-10000, or 0 if no limit
    unsigned long memoryHigh; // Integer > 0 (bytes), or 0 if no limit
    unsigned long memoryMax; // Integer > 0 (bytes), or 0 if no limit
    unsigned long pidsMax; // Integer > 0 (number of pids), or 0 if no limit

    // Networking settings for the container
    char* networkBridgeName; // String or NULL if no network is needed for the container. Mandatory if networkIpAddr or networkDefaultRoute is specified.
    char* networkIpAddr; // String or NULL if no IP address is to be used for the container.
    char* networkDefaultRoute; // String or NULL if no default route is to be used for the container.
};

extern void (*tinyjailErrorLogFunc)(const char*);

__attribute__ ((visibility ("default"))) int tinyjailLaunchContainer(struct tinyjailContainerParams programArgs);
