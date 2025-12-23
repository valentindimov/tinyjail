// SPDX-License-Identifier: MIT

#pragma once

#include "tinyjail.h"

/// @brief Runs the container launcher logic, in a separate subprocess.
/// @param containerParams Input arg: the parameters for launching the container
/// @param result Output arg: the result of the launch is written here
void launchContainer(
    const struct tinyjailContainerParams *containerParams, 
    struct tinyjailContainerResult *result
);
