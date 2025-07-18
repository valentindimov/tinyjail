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

    char** currentArg = argv + 1;
    while (*currentArg != NULL) {
        char* command = *(currentArg++);
        if (*currentArg == NULL) {
            return -1;
        }

        if (strcmp(command, "--") == 0) {
            parsedArgs->commandList = currentArg;
            break;
        } else if (strcmp(command, "--root") == 0) {
            parsedArgs->containerDir = *(currentArg++);
        } else if (strcmp(command, "--env") == 0) {
            *(envStringsBuffer++) = *(currentArg++);
        } else if (strcmp(command, "--workdir") == 0) {
            parsedArgs->workDir = *(currentArg++);
        } else if (strcmp(command, "--lowerdirs") == 0) {
            parsedArgs->lowerDirs = *(currentArg++);
        } else if (strcmp(command, "--cpu-max-percent") == 0) {
            parsedArgs->cpuMaxPercent = strtoul(*currentArg, NULL, 0);
            if (parsedArgs->cpuMaxPercent > 100) {
                printf("Invalid --cpu-max-percent: %s", *currentArg);
                return -1;
            }
            currentArg++;
        } else if (strcmp(command, "--cpu-weight") == 0) {
            parsedArgs->cpuWeight = strtoul(*currentArg, NULL, 0);
            if (parsedArgs->cpuWeight < 100 || parsedArgs->cpuWeight > 10000) {
                printf("Invalid --cpu-weight: %s", *currentArg);
                return -1;
            }
            currentArg++;
        } else if (strcmp(command, "--memory-high") == 0) {
            parsedArgs->memoryHigh = strtoul(*(currentArg++), NULL, 0);
        } else if (strcmp(command, "--memory-max") == 0) {
            parsedArgs->memoryMax = strtoul(*(currentArg++), NULL, 0);
        } else if (strcmp(command, "--pids-max") == 0) {
            parsedArgs->pidsMax = strtoul(*(currentArg++), NULL, 0);
        } else if (strcmp(command, "--network-bridge") == 0) {
            parsedArgs->networkBridgeName = *(currentArg++);
        } else if (strcmp(command, "--ip-address") == 0) {
            parsedArgs->networkIpAddr = *(currentArg++);
        } else if (strcmp(command, "--default-route") == 0) {
            parsedArgs->networkDefaultRoute = *(currentArg++);
        } else {
            printf("Unknown argument: %s.\n", command);
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