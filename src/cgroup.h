#pragma once

#include "tinyjail.h"

int tinyjailSetupContainerCgroup(
    char* containerId, 
    int childPid, 
    unsigned int uid, 
    unsigned int gid, 
    struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult* result
);
int tinyjailDestroyCgroup(char* containerId);