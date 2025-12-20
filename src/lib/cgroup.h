#pragma once

#include "tinyjail.h"

/// @brief Set up the container cgroup restrictions.
/// @param containerId ID of the container
/// @param childPid PID of the container process
/// @param uid UID of the container process
/// @param gid GID of the container process
/// @param containerParams Container options object
/// @param result Result object returned to the library caller
/// @return 0 on success, -1 on failure
int setupContainerCgroup(
    const char* containerId,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
);
