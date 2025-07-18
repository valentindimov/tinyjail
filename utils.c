#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <alloca.h>

#include "tinyjail.h"
#include "utils.h"

int stringIsRegularFilename(const char* filename) {
    // Disallow the special filenames "." and ".."
    if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
        return 0;
    }
    // Reject any string containing slashes
    while(*filename) {
        if (*filename == '/') {
            return 0;
        }
        filename++;
    }
    return 1;
}

int tinyjailWriteFileAt(int dirfd, const char* filePath, const char* format, ...) {
    // Format the file data into an in-memory string
    va_list argptr;
    va_start(argptr, format);
    int szData = vsnprintf("", 0, format, argptr);
    va_end(argptr);
    char* fileData = (char*) alloca((szData + 1) * sizeof(char));
    va_start(argptr, format);
    vsnprintf(fileData, szData + 1, format, argptr);
    va_end(argptr);

    // Write the formatted data into the file
    int fd = openat(dirfd, filePath, O_WRONLY);
    if (fd < 0) {
        return -1; 
    }
    int nbytes = strlen(fileData);
    while (nbytes > 0) {
        int numWrittenBytes = write(fd, fileData, nbytes);
        if (numWrittenBytes < 0) {
            close(fd);
            return -1;
        } else {
            nbytes -= numWrittenBytes;
            fileData += numWrittenBytes;
        }
    }
    close(fd);

    // done
    return 0;
}