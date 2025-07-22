#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/syscall.h> 
#include <sys/stat.h>

#include "child.h"
#include "utils.h"

int containerChildLaunch(struct ContainerChildLauncherArgs *args) {
    // We won't need the writing end of the sync pipe (and in case the parent crashes, we want to avoid being stuck waiting on ourselves)
    close(args->syncPipeWrite);
    close(args->errorPipeRead);
    
    // Wait to get a message "OK" over the sync pipe. Only if we get that are we sure that our parent has initialized everything.
    char result[2];
    if (read(args->syncPipeRead, result, 2) != 2 || strncmp(result, "OK", 2) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not read() on sync pipe: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }
    close(args->syncPipeRead);

    // Unshare the cgroup namespace here (after our parent has had the chance to move us to our cgroup)
    if (unshare(CLONE_NEWCGROUP) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Unsharing cgroup namespace in child failed: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Set our UID and GID.
    if (setuid(0) != 0 || setgid(0) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not switch UID or GID: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Bind-mount the container dir to itself then pivot to it
    if (mount(args->containerDir, args->containerDir, "none", MS_BIND | MS_PRIVATE | MS_REC | MS_NOSUID, NULL) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not bind-mount container roor dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }
    if (chdir(args->containerDir) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not chdir to container roor dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Pivot the filesystem root
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not pivot_root to container roor dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }
    if (umount2(".", MNT_DETACH) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not unmount old root dir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // If a working directory was set, make sure to set that before execve-ing
    if (args->workDir != NULL && chdir(args->workDir) != 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "Child could not chdir to chosen workdir: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // Make sure that if we successfully execve(), the errorPipeWrite is closed
    if (fcntl(args->errorPipeWrite, F_SETFD, FD_CLOEXEC) < 0) {
        ALLOC_LOCAL_FORMAT_STRING(error, "fcntl() on error pipe failed: %s", strerror(errno))
        write(args->errorPipeWrite, error, strlen(error));
        return -1;
    }

    // All good, execute the target command.
    execve(args->commandList[0], (args->commandList + 1), args->environment);

    // If we got here, the execve() call failed.
    ALLOC_LOCAL_FORMAT_STRING(error, "execve() failed: %s", strerror(errno))
    write(args->errorPipeWrite, error, strlen(error));
    return -1;
}