#pragma once

#include "tinyjail.h"

int tinyjailSetupContainerCgroup(char* containerId, int childPid, unsigned int uid, unsigned int gid, struct tinyjailContainerParams* containerParams);
int tinyjailDestroyCgroup(char* containerId);