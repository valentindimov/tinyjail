#pragma once

#include "tinyjail.h"

/// @brief Sets up the container's user namespace.
/// @param childPid PID of the container process
/// @param uid UID of the container process
/// @param gid GID of the container process
/// @param containerParams Container parameters
/// @param result Result object passed to the library caller
/// @return 0 on success, -1 on failure
int setupContainerUserNamespace(
    int childPid, 
    int uid, 
    int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
);
