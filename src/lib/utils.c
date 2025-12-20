#include <unistd.h>
#include <string.h>

#include "utils.h"

void closep(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int splitString(char* input, char** output_1, char** output_2, char delim) {
    if (input == NULL) {
        return -1;
    }
    // Start from the beginning of the string and iterate until the string ends, or we find '='
    *output_1 = input;
    *output_2 = input;
    while (**output_2 != '\0' && **output_2 != delim) {
        *output_2 += 1;
    }
    if (**output_2 == delim) {
        // Found the delimiter, replace it with a NULL and set output_2 to the remainder of the string.
        **output_2 = '\0';
        *output_2 += 1;
        return 0;
    } else {
        // We did not find an the delimiter
        return -1;
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
