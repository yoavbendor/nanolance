# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""pytest plugin: run a test suite written for lancedb against nanolance.

    python -m pytest -p nanolance.lancedb._pytest_plugin <tests>

Loaded before any test module is imported, so ``import lancedb`` and ``import lance`` in them are nanolance's.
Processes the tests spawn (multiprocessing "spawn") start without it: put a ``sitecustomize.py`` calling
``nanolance.lancedb.install_as_lancedb()`` on their PYTHONPATH.
"""

import nanolance.lancedb

nanolance.lancedb.install_as_lancedb()
