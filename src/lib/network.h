#pragma once

#include "tinyjail.h"

/// @brief Sets up the network of the container.
/// @param childPid PID of the container process
/// @param containerId ID of the container
/// @param params Container parameters
/// @param result Result object passed back to the library caller
/// @return 
int setupContainerNetwork(
    int childPid, 
    char* containerId, 
    const struct tinyjailContainerParams *params,
    struct tinyjailContainerResult *result
);
