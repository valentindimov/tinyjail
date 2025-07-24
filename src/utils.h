#pragma once

#include <stdio.h>
#include <alloca.h>
#include <sys/random.h>

/// @brief Sets VARNAME to a formatted string given the format, and lenVARNAME to the number of characters (not including the terminating NULL) in the string.
#define ALLOC_LOCAL_FORMAT_STRING(VARNAME, FORMAT, ...) \
    int len##VARNAME = snprintf("", 0, FORMAT, __VA_ARGS__); \
    char* VARNAME = alloca((len##VARNAME + 1) * sizeof(char)); \
    snprintf(VARNAME, len##VARNAME + 1, FORMAT, __VA_ARGS__);

void closep(int* fd);
#define RAII_FD __attribute__((cleanup(closep))) int

/// @brief Checks if a given string represents a filename (not a path) which is not "." or ".."
/// @param filename The filename to check
/// @return 1 if the string represents a regular filename, and 0 if it does not.
/// Generally you can use this function to see if a user-supplied filename is safe to use.
/// The function will reject filenames which can cause path traversal.
int stringIsRegularFilename(const char* filename);