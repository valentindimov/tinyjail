#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <alloca.h>

#include "tinyjail.h"
#include "utils.h"

void closep(int* fd) {
    if (*fd >= 0) {
        close(*fd);
    }
}

int stringIsRegularFilename(const char* filename) {
    // Disallow empty strings and the special filenames "." and ".."
    if (*filename == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
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