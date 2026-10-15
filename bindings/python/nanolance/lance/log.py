# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.log``: the Python logger for pylance-compatible code, and the level Lance's own log uses.

``LANCE_LOG`` sets the level of :data:`LOGGER` (the first entry without ``=``; ``trace`` means DEBUG,
empty means INFO). The same variable switches on Lance's native log, which nanolance writes from
:mod:`nanolance.lance._native_log`; see that module for the line format.
"""

from __future__ import annotations

import logging
import os
from typing import Optional

#: The environment variable holding Lance's log filter, as in pylance.
ENV_NAME_PYLANCE_LOGGING_LEVEL = "LANCE_LOG"

_DEFAULT_FORMAT = "%(asctime)s %(name)s [%(levelname)s] %(filename)s:%(lineno)d %(funcName)s : %(message)s"


def get_python_log_level(rust_log_level: str) -> str:
    """Python has no TRACE level: Lance's ``trace`` is logged as DEBUG."""
    if rust_log_level.lower() == "trace":
        return "DEBUG"
    return rust_log_level


def get_log_level() -> str:
    """The Python level for ``LANCE_LOG``: its first bare level, upper-cased; INFO when unset or empty."""
    value = os.environ.get(ENV_NAME_PYLANCE_LOGGING_LEVEL, "INFO").upper()
    if value == "":
        return "INFO"
    bare = [entry for entry in value.split(",") if "=" not in entry]
    if bare:
        return get_python_log_level(bare[0])
    return "INFO"


LOGGER = logging.getLogger("pylance")
LOGGER.setLevel(get_log_level())


def set_logger(
    file_path: Optional[str] = "pylance.log",
    name: str = "pylance",
    level: int = logging.INFO,
    format_string: Optional[str] = None,
    log_handler: Optional[logging.Handler] = None,
) -> None:
    """Point :data:`LOGGER` at logger ``name`` with one handler: ``log_handler``, or a file handler on
    ``file_path``. Every record is formatted with ``format_string`` (the default shows time, logger,
    level, file, line and function). Rebinds the module's ``LOGGER``."""
    global LOGGER
    if not format_string:
        format_string = _DEFAULT_FORMAT
    LOGGER = logging.getLogger(name)
    LOGGER.setLevel(level)
    handler = log_handler if log_handler is not None else logging.FileHandler(file_path)
    handler.setLevel(level)
    handler.setFormatter(logging.Formatter(format_string))
    LOGGER.addHandler(handler)
