#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h> 
#include <sys/stat.h>

#include "child.h"

int containerChildLaunch(struct ContainerChildLauncherArgs *args) {
    // We won't need the writing end of the sync pipe (and in case the parent crashes, we want to avoid being stuck waiting on ourselves)
    close(args->syncPipeWrite);
    // Wait to get a message "OK" over the sync pipe. Only if we get that are we sure that our parent has initialized everything.
    char result[2];
    if (read(args->syncPipeRead, result, 2) != 2 || strncmp(result, "OK", 2) != 0) {
        // TODO log error: Waiting for the go-ahead signal from the launcher encountered an error
        return -1;
    }
    close(args->syncPipeRead);

    // Unshare the cgroup namespace here (after our parent has had the chance to move us to our cgroup)
    if (unshare(CLONE_NEWCGROUP) != 0) {
        // TODO log error: Child could not unshare cgroup namespace
        return -1;
    }

    // Set our UID and GID.
    if (setuid(0) != 0 || setgid(0) != 0) {
        // TODO log error: Child could not switch UID and GID
        return -1;
    }

    // Bind-mount the container dir to itself then pivot to it
    if (mount(args->containerDir, args->containerDir, "none", MS_BIND | MS_PRIVATE | MS_REC | MS_NOSUID, NULL) != 0) {
        // TODO log error: Could not bind-mount container root directory
        return -1;
    }
    if (chdir(args->containerDir) != 0) {
        // TODO log error: Child could chdir to container root dir 
        return -1;
    }

    // Pivot the filesystem root
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        // TODO: Child could pivot_root to container root dir
        return -1;
    }
    if (umount2(".", MNT_DETACH) != 0) {
        // TODO: Child could unmount old root from container root dir
        return -1;
    }

    // If a working directory was set, make sure to set that before execve-ing
    if (args->workDir != NULL && chdir(args->workDir) != 0) {
        // TODO: Child could chdir to chosen workdir for container
        return -1;
    }

    // All good, execute the target command.
    execve(args->commandList[0], (args->commandList + 1), args->environment);

    // If we got here, the execve() call failed. We already cleaned the temporary directory though, so just exit.
    // TODO log error: Child could not execve(). The container binary, or a library it requires, may be missing.
    return -1;
}