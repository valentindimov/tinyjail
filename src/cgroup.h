#pragma once

#include "tinyjail.h"

int tinyjailSetupContainerCgroup(
    const char* containerCgroupPath,
    int childPid,
    unsigned int uid, 
    unsigned int gid, 
    const struct tinyjailContainerParams* containerParams,
    struct tinyjailContainerResult *result
);
int tinyjailDestroyCgroup(char* containerId);