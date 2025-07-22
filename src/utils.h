#pragma once

#include <stdio.h>
#include <alloca.h>
#include <sys/random.h>

#define ALLOC_LOCAL_FORMAT_STRING(VARNAME, FORMAT, ...) \
    int sz##VARNAME = snprintf("", 0, FORMAT, __VA_ARGS__); \
    char* VARNAME = alloca((sz##VARNAME + 1)*sizeof(char)); \
    snprintf(VARNAME, sz##VARNAME + 1, FORMAT, __VA_ARGS__);

void closep(int* fd);
#define RAII_FD __attribute__((cleanup(closep))) int

/// @brief Writes a string to a file. The string will be created in memory first then written with a single write() call.
/// @param dirfd FD to the directory where the file is located.
/// @param filePath Path to the file relative to dirfd.
/// @param fileData String to write to the file.
/// @return 0 on success, -1 on failure.
int tinyjailWriteFileAt(int dirfd, const char* filePath, const char* format, ...);

/// @brief Checks if a given string represents a filename (not a path) which is not "." or ".."
/// @param filename The filename to check
/// @return 1 if the string represents a regular filename, and 0 if it does not.
/// Generally you can use this function to see if a user-supplied filename is safe to use.
/// The function will reject filenames which can cause path traversal.
int stringIsRegularFilename(const char* filename);