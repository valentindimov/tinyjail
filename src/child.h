#pragma once

struct ContainerChildLauncherArgs {
    char* containerDir;
    char** commandList;
    char** environment;
    char* workDir;
    // Pipe used by the parent to signal to the child that its namespaces are initialized and it may execve() now.
    int syncPipeWrite;
    int syncPipeRead;
    // Pipe used by the child to send error messages to the parent.
    int errorPipeWrite;
    int errorPipeRead;
};

int containerChildLaunch(struct ContainerChildLauncherArgs *args);