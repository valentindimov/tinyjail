#pragma once

#include <stdio.h>
#include <alloca.h>
#include <sys/random.h>

#define ALLOC_LOCAL_FORMAT_STRING(VARNAME, FORMAT, ...) \
    int sz##VARNAME = snprintf("", 0, FORMAT, __VA_ARGS__); \
    char* VARNAME = alloca((sz##VARNAME + 1)*sizeof(char)); \
    snprintf(VARNAME, sz##VARNAME + 1, FORMAT, __VA_ARGS__);

/// @brief Writes a string to a file. The string will be created in memory first then written with a single write() call.
/// @param dirfd FD to the directory where the file is located.
/// @param filePath Path to the file relative to dirfd.
/// @param fileData String to write to the file.
/// @return 0 on success, -1 on failure.
int tinyjailWriteFileAt(int dirfd, const char* filePath, const char* format, ...);