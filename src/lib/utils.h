// SPDX-License-Identifier: MIT

#pragma once

#include <alloca.h>

/// @brief Allocates a locally-scoped (via alloca()) string using the provided format.
#define ALLOC_LOCAL_FORMAT_STRING(VARNAME, FORMAT, ...) \
    int len##VARNAME = snprintf("", 0, FORMAT, __VA_ARGS__); \
    char* VARNAME = alloca((len##VARNAME + 1) * sizeof(char)); \
    snprintf(VARNAME, len##VARNAME + 1, FORMAT, __VA_ARGS__);

/// @brief Closes a fire descriptor and sets it to -1 if it is nonnegative. If it is negative, does nothing.
/// @param fd Pointer to the file descriptor variable
void closep(int* fd);

/// @brief Splits an input string into two output strings at a delmiter character. Allocates no memory, but modifies the input string by setting delimiter bytes to NULL bytes.
/// @param input The input string. It will be modified by this function.
/// @param output_1 Output: The part of the input before the delimiter. Output undefined if the function fails.
/// @param output_2 Output: The part of the input after the delimiter. Output undefined if the function fails.
/// @param delim The delimiter character
/// @return 0 if the string could be split, -1 if the split was unsuccessful (e.g. because the delimiter character was not found)
int splitString(char* input, char** output_1, char** output_2, char delim);

/// @brief Defines a special type of FD that gets automatically closed when it exits scope. Use closep() to close it earlier, that function is idempotent.
/// Unfortunately this is a non-standard extension, so it will only compile with gcc or clang. But it's so useful for simplifying error handling, it's worth losing that portability...
#define RAII_FD __attribute__((cleanup(closep))) int

/// @brief Checks if a given string represents a filename (not a path) which is not "." or ".."
/// @param filename The filename to check
/// @return 1 if the string represents a regular filename, and 0 if it does not.
/// Generally you can use this function to see if a user-supplied filename is safe to use.
/// The function will reject filenames which can cause path traversal.
int stringIsRegularFilename(const char* filename);
