// SPDX-License-Identifier: Apache-2.0
// setenv / unsetenv for the tests that set environment variables. POSIX has them; the Windows CRT has
// _putenv_s instead, where an empty value removes the variable.
#pragma once

#include <cstdlib>

#ifdef _WIN32
inline int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif
