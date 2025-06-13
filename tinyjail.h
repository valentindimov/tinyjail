#pragma once

struct tinyjailContainerParams {
    char* containerDir; // System path
    char** commandList; // NULL-terminated argv
    char** environment; // NULL-terminated envp

    unsigned long cpuMaxPercent; // Integer 1-100, or 0 if no limit
    unsigned long cpuWeight; // Integer 100-10000, or 0 if no limit
    unsigned long memoryHigh; // Integer > 0 (bytes), or 0 if no limit
    unsigned long memoryMax; // Integer > 0 (bytes), or 0 if no limit
    unsigned long pidsMax; // Integer > 0 (number of pids), or 0 if no limit

    char* networkBridgeName; // String or NULL if no network is needed for the container. Mandatory if networkIpAddr or networkDefaultRoute is specified.
    char* networkIpAddr; // String or NULL if no IP address is to be used for the container.
    char* networkDefaultRoute; // String or NULL if no default route is to be used for the container.
};

extern void (*tinyjailErrorLogFunc)(const char*);

__attribute__ ((visibility ("default"))) int tinyjailLaunchContainer(struct tinyjailContainerParams programArgs);
