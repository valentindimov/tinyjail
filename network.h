#pragma once

#include "tinyjail.h"

int tinyjailSetupContainerNetwork(
    int childPid, 
    char* containerId, 
    struct tinyjailContainerParams *params, 
    struct tinyjailContainerResult *result
);