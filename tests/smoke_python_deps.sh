# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
#
# Shared preflight for the ctest smoke scripts, which all drive a Python interpreter to build their
# input or to cross-check the result with stock Lance.
#
# WHY. The documented quick start is `cmake -S . -B build && cmake --build build && ctest`. It says
# nothing about pip, because the LIBRARY needs nothing from pip -- but eight smoke tests did, and
# they failed rather than skipped when pyarrow was absent. A new user following the README on a
# clean machine got eight red tests for a dependency the docs never asked them to install. The
# quickstart CI job caught this the first time it was able to run.
#
# ctest already has the right mechanism (`SKIP_RETURN_CODE 77`, used by the golden-file tests), so
# this just makes every smoke script report a missing optional dependency the same way. A missing
# module is a SKIP; anything else is still a failure.
#
# Usage, after $python_bin is set:
#     . "$(dirname "$0")/smoke_python_deps.sh"
#     require_python_modules "$python_bin" pyarrow          # or: pyarrow lance

require_python_modules() {
    local python_bin="$1"
    shift
    local missing
    if ! missing="$("$python_bin" - "$@" <<'PY'
import importlib.util
import sys

print(" ".join(name for name in sys.argv[1:] if importlib.util.find_spec(name) is None))
PY
    )"; then
        echo "smoke skipped: '$python_bin' could not be run" >&2
        exit 77
    fi
    if [ -n "$missing" ]; then
        echo "smoke skipped: '$python_bin' is missing module(s):$missing" >&2
        echo "  install them with: $python_bin -m pip install$missing" >&2
        exit 77
    fi
}
