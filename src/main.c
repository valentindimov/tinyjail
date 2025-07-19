#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <alloca.h>

#include "tinyjail.h"

int parseArgs(char** argv, 
              struct tinyjailContainerParams *parsedArgs, 
              char** envStringsBuffer, 
              char** cgroupOptionsBuffer) {
    if (*argv == NULL) {
        return -1;
    }

    parsedArgs->environment = envStringsBuffer;
    parsedArgs->cgroupOptions = cgroupOptionsBuffer;

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
        } else if (strcmp(command, "--cgroup") == 0) {
            *(cgroupOptionsBuffer++) = *(currentArg++);
        } else if (strcmp(command, "--workdir") == 0) {
            parsedArgs->workDir = *(currentArg++);
        } else if (strcmp(command, "--lowerdirs") == 0) {
            parsedArgs->lowerDirs = *(currentArg++);
        } else if (strcmp(command, "--network-bridge") == 0) {
            parsedArgs->networkBridgeName = *(currentArg++);
        } else if (strcmp(command, "--ip-address") == 0) {
            parsedArgs->networkIpAddr = *(currentArg++);
        }  else if (strcmp(command, "--peer-ip-address") == 0) {
            parsedArgs->networkPeerIpAddr = *(currentArg++);
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

    // Same goes for the list of strings for cgroup options
    char** cgroupOptionsBuf = alloca((argc + 1) * sizeof(char*));
    memset(cgroupOptionsBuf, 0, (argc + 1) * sizeof(char*));

    struct tinyjailContainerParams programArgs = {0};
    if (parseArgs(argv, &programArgs, envStringsBuf, cgroupOptionsBuf) != 0) {
        printf(
            "Usage: ./jail --root <root directory> "
            "[--env <key>=<value>]* "
            "[--workdir <directory>] "
            "[--lowerdirs <dir1>:<dir2>:...] "
            "[--cgroup <option>=<value>] "
            "[--network-bridge <device name>] "
            "[--ip-address <address>] "
            "[--peer-ip-address <address>] "
            "[--default-route <address>] "
            "-- <command>\n");
        return -1;
    }

    struct tinyjailContainerResult result = tinyjailLaunchContainer(programArgs);
    if (result.containerStartedStatus != 0) {
        fprintf(
            stderr, 
            "Error when starting container: %s\n", 
            result.errorInfo[0] == '\0' ? "(no error info)" : result.errorInfo
        );
        return -1;
    } else if (WIFEXITED(result.containerExitStatus)) {
        return WEXITSTATUS(result.containerExitStatus);
    } else if (WIFSIGNALED(result.containerExitStatus)) {
        fprintf(stderr, "Container killed by signal %d\n", WTERMSIG(result.containerExitStatus));
        return -1;
    } else {
        fprintf(stderr, "Container exit info: %x", result.containerExitStatus);
        return -1;
    }
}