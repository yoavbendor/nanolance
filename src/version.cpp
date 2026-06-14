// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/version.hpp"

#include "nanolance/version.h"

namespace nanolance {

const char* library_version() {
    return NANOLANCE_VERSION_STRING;
}

}  // namespace nanolance
