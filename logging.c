#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>

#include "tinyjail.h"
#include "logging.h"

static void defaultErrorLogFunc(const char* message) {
    fprintf(stderr, "%s\n", message);
}
void (*tinyjailErrorLogFunc)(const char*) = defaultErrorLogFunc;

void tinyjailLogError(const char* format, ...) {
    // Format the error message into an in-memory string
    va_list argptr;
    va_start(argptr, format);
    int szData = vsnprintf("", 0, format, argptr);
    va_end(argptr);
    char* errorMessage = (char*) alloca((szData + 1) * sizeof(char));
    va_start(argptr, format);
    vsnprintf(errorMessage, szData + 1, format, argptr);
    va_end(argptr);
    // Now call the error logging function
    tinyjailErrorLogFunc(errorMessage);
}