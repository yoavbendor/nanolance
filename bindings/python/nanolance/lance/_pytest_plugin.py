# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""pytest plugin: run a test suite written for pylance against nanolance.

    python -m pytest -p nanolance.lance._pytest_plugin <pylance's tests>

Loaded before any test module is imported, so ``import lance`` in them is nanolance's.
"""

import nanolance.lance

nanolance.lance.install_as_lance()
