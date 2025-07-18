#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <alloca.h>

#include "tinyjail.h"

int parseArgs(char** argv, struct tinyjailContainerParams *parsedArgs, char** envStringsBuffer) {
    if (*argv == NULL) {
        return -1;
    }

    parsedArgs->environment = envStringsBuffer;

    char** argparse = argv + 1;
    while (*argparse != NULL) {
        char* command = *(argparse++);
        if (*argparse == NULL) {
            return -1;
        }

        if (strcmp(command, "--") == 0) {
            parsedArgs->commandList = argparse;
            break;
        } else if (strcmp(command, "--root") == 0) {
            parsedArgs->containerDir = *(argparse++);
        } else if (strcmp(command, "--env") == 0) {
            *(envStringsBuffer++) = *(argparse++);
        }  else if (strcmp(command, "--workdir") == 0) {
            parsedArgs->workDir = *(argparse++);
        } else if (strcmp(command, "--cpu-max-percent") == 0) {
            parsedArgs->cpuMaxPercent = strtoul(*(argparse++), NULL, 0);
            if (parsedArgs->cpuMaxPercent > 100) {
                return -1;
            }
        } else if (strcmp(command, "--cpu-weight") == 0) {
            parsedArgs->cpuWeight = strtoul(*(argparse++), NULL, 0);
            if (parsedArgs->cpuWeight < 100 || parsedArgs->cpuWeight > 10000) {
                return -1;
            }
        } else if (strcmp(command, "--memory-high") == 0) {
            parsedArgs->memoryHigh = strtoul(*(argparse++), NULL, 0);
        } else if (strcmp(command, "--memory-max") == 0) {
            parsedArgs->memoryMax = strtoul(*(argparse++), NULL, 0);
        } else if (strcmp(command, "--pids-max") == 0) {
            parsedArgs->pidsMax = strtoul(*(argparse++), NULL, 0);
        } else if (strcmp(command, "--network-bridge") == 0) {
            parsedArgs->networkBridgeName = *(argparse++);
        } else if (strcmp(command, "--ip-address") == 0) {
            parsedArgs->networkIpAddr = *(argparse++);
        } else if (strcmp(command, "--default-route") == 0) {
            parsedArgs->networkDefaultRoute = *(argparse++);
        } else {
            printf("Unknown: %s.\n", command);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (getuid() != 0) { 
        printf("Please run this program as root.\n"); 
        return -1;
    }

    // We can have at most argc env pointers specified, so just allocate space for that many.
    // We will definitely allocate too much space here, but it shouldn't really be a problem.
    char** envStringsBuf = alloca((argc + 1) * sizeof(char*));
    memset(envStringsBuf, 0, (argc + 1) * sizeof(char*));

    struct tinyjailContainerParams programArgs = {0};
    if (parseArgs(argv, &programArgs, envStringsBuf) != 0) {
        printf("Usage: ./jail --root <root directory> [--env <key>=<value>]* [<resource limit args>] [<networking args>] -- <command>\n");
        printf("Resource limit args: --cpu-max-percent, --cpu-weight, --memory-high, --memory-max, --pids-max\n");
        printf("Networking args: --network-bridge (mandatory if any of the others is specified), --ip-address, --default-route\n");
        return -1;
    }

    return tinyjailLaunchContainer(programArgs);
}