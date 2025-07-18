#pragma once

#include "tinyjail.h"

int tinyjailSetupContainerUserNamespace(int childPid, int uid, int gid, struct tinyjailContainerResult *result);