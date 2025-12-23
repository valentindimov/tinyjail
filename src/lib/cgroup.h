// SPDX-License-Identifier: MIT

#pragma once

#include "tinyjail.h"

/// @brief Set up the container cgroup restrictions.
/// @param childPid PID of the container process
/// @param containerParams Container options object
/// @param result Result object returned to the library caller
/// @return 0 on success, -1 on failure
int setupContainerCgroup(
    int childPid,
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
);

/// @brief Attempts to clean the container cgroup after the container has exited.
/// @param containerParams Container options object
void cleanContainerCgroup(
    const struct tinyjailContainerParams* containerParams
);
