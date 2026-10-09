# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.dependencies``: optional third-party modules, imported on first use.

``pandas`` here is the module, or -- when it is not installed -- a stand-in that raises a helpful
``ModuleNotFoundError`` when used; ``_PANDAS_AVAILABLE`` says which. ``_check_for_pandas(obj)`` asks
whether an object is a pandas one without importing pandas to find out.
"""

from __future__ import annotations

import re
import sys
from functools import lru_cache
from importlib import import_module
from importlib.util import find_spec
from types import ModuleType
from typing import Any, Tuple

__all__ = ["numpy", "pandas", "polars", "torch", "datasets"]


class _LazyModule(ModuleType):
    """A module imported when first touched, or a stand-in for one that is not installed."""

    __lazy__ = True
    _prefixes = {"numpy": "np.", "pandas": "pd.", "polars": "pl.", "torch": "torch."}

    def __init__(self, module_name: str, *, module_available: bool) -> None:
        self._module_available = module_available
        self._module_name = module_name
        super().__init__(module_name)

    def __getattr__(self, attr: str) -> Any:
        if attr == "__wrapped__":
            raise AttributeError(f"{self._module_name!r} object has no attribute {attr!r}")
        if self._module_available:
            module = import_module(self._module_name)
            globals()[self._module_name] = module
            self.__dict__.update(module.__dict__)
            return getattr(module, attr)
        if attr == "__name__":
            return self._module_name
        if re.match(r"^__\w+__$", attr) and attr != "__version__":
            return None
        prefix = self._prefixes.get(self._module_name, "")
        raise ModuleNotFoundError(f"{prefix}{attr} requires {self._module_name!r} module to be installed") from None


def _lazy_import(module_name: str) -> Tuple[ModuleType, bool]:
    if module_name in sys.modules:
        return sys.modules[module_name], True
    try:
        spec = find_spec(module_name)
        available = not (spec is None or spec.loader is None)
    except ModuleNotFoundError:
        available = False
    return _LazyModule(module_name, module_available=available), available


numpy, _NUMPY_AVAILABLE = _lazy_import("numpy")
pandas, _PANDAS_AVAILABLE = _lazy_import("pandas")
polars, _POLARS_AVAILABLE = _lazy_import("polars")
torch, _TORCH_AVAILABLE = _lazy_import("torch")
datasets, _HUGGING_FACE_AVAILABLE = _lazy_import("datasets")
_, _PYDANTIC_AVAILABLE = _lazy_import("pydantic")


@lru_cache(maxsize=None)
def _might_be(cls: type, type_: str) -> bool:
    try:
        return any(f"{type_}." in str(o) for o in cls.mro())
    except TypeError:
        return False


def _check_for_numpy(obj: Any, *, check_type: bool = True) -> bool:
    return _NUMPY_AVAILABLE and _might_be(type(obj) if check_type else obj, "numpy")


def _check_for_pandas(obj: Any, *, check_type: bool = True) -> bool:
    return _PANDAS_AVAILABLE and _might_be(type(obj) if check_type else obj, "pandas")


def _check_for_polars(obj: Any, *, check_type: bool = True) -> bool:
    return _POLARS_AVAILABLE and _might_be(type(obj) if check_type else obj, "polars")


def _check_for_torch(obj: Any, *, check_type: bool = True) -> bool:
    return _TORCH_AVAILABLE and _might_be(type(obj) if check_type else obj, "torch")


def _check_for_hugging_face(obj: Any, *, check_type: bool = True) -> bool:
    return _HUGGING_FACE_AVAILABLE and _might_be(type(obj) if check_type else obj, "datasets")
