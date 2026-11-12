// SPDX-License-Identifier: Apache-2.0
// setenv / unsetenv and getpid for the tests that use them. POSIX has them (getpid in <unistd.h>); the Windows CRT
// has _putenv_s instead, where an empty value removes the variable, and _getpid in <process.h>.
#pragma once

#include <cstdlib>

#ifdef _WIN32
#include <process.h>

inline int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
inline int getpid() { return _getpid(); }
#else
#include <unistd.h>
#endif
