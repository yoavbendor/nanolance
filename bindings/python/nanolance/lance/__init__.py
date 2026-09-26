# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""A pylance-compatible API over nanolance.

Code written against pylance's ``lance`` module runs on nanolance by changing its import::

    import nanolance.lance as lance

    ds = lance.write_dataset(table, "data.lance")
    ds.to_table(columns=["a"], limit=10)

or, to leave the code as it is, by installing nanolance under the ``lance`` name for this process
(before anything imports ``lance``)::

    import nanolance.lance
    nanolance.lance.install_as_lance()
    import lance  # nanolance's

Only the part of pylance listed in docs/PYLANCE_COMPAT.md is implemented -- reading and writing
datasets, versions, fragments, row ids, files -- and anything outside it raises
``NotImplementedError`` naming the feature rather than doing something different. Installing does
not touch a real pylance on disk; it only changes what ``import lance`` means in this process.
"""

from __future__ import annotations

import importlib
import importlib.abc
import importlib.machinery
import importlib.util
import sys

import nanolance as _nanolance_pkg
from nanolance.lance._errors import NotSupportedError
from nanolance.lance.dataset import (
    LanceDataset,
    LanceScanner,
    ScannerBuilder,
    dataset,
    write_dataset,
)
from nanolance.lance.blob import BlobFile, blob_array, blob_field
from nanolance.lance.fragment import DataFile, DeletionFile, FragmentMetadata, LanceFragment

#: The pylance release whose API (and test suite) this module tracks.
PYLANCE_API_VERSION = "12.0.0"
__version__ = f"{PYLANCE_API_VERSION}+nanolance.{_nanolance_pkg.__version__}"

__all__ = [
    "BlobFile",
    "DataFile",
    "DeletionFile",
    "FragmentMetadata",
    "LanceDataset",
    "LanceFragment",
    "LanceScanner",
    "NotSupportedError",
    "ScannerBuilder",
    "__version__",
    "blob_array",
    "blob_field",
    "dataset",
    "install_as_lance",
    "uninstall_as_lance",
    "write_dataset",
]


def __getattr__(name: str):
    from nanolance.lance._stubs import placeholder

    if name.startswith("__"):
        raise AttributeError(name)
    # A submodule nanolance implements (lance.file, lance.fragment, ...) is the module itself.
    try:
        module = importlib.import_module(f"{__name__}.{name}")
    except ModuleNotFoundError as exc:
        if exc.name != f"{__name__}.{name}":
            raise
    else:
        globals()[name] = module
        return module
    value = placeholder(f"lance.{name}")
    globals()[name] = value
    return value


class _AliasFinder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    """Resolve ``lance`` and ``lance.<sub>`` to ``nanolance.lance`` and ``nanolance.lance.<sub>``."""

    def find_spec(self, fullname, path=None, target=None):
        if fullname != "lance" and not fullname.startswith("lance."):
            return None
        return importlib.machinery.ModuleSpec(fullname, self, is_package=True)

    def create_module(self, spec):
        name = spec.name
        real = "nanolance." + name
        try:
            return importlib.import_module(real)
        except ModuleNotFoundError as exc:
            if exc.name != real:
                raise
        from nanolance.lance._stubs import PYLANCE_MODULES, stub_module

        sub = name[len("lance."):]
        if sub in PYLANCE_MODULES or sub.split(".")[0] in PYLANCE_MODULES:
            return stub_module(real, name)
        raise ModuleNotFoundError(f"No module named {name!r} (not part of pylance's API)", name=name)

    def exec_module(self, module):
        pass


_finder = _AliasFinder()


def install_as_lance() -> None:
    """Make ``import lance`` (and ``lance.<submodule>``) load nanolance's implementation.

    Affects this process only. Call it before anything imports ``lance``: a module that already
    imported the real pylance keeps it.
    """
    if _finder not in sys.meta_path:
        sys.meta_path.insert(0, _finder)
    for name in [n for n in sys.modules if n == "lance" or n.startswith("lance.")]:
        if not getattr(sys.modules[name], "__name__", "").startswith("nanolance."):
            del sys.modules[name]
    # Register the modules already loaded under their ``lance`` names. Loading one through the finder
    # instead would also bind it as an attribute of the package -- ``lance.dataset`` would become the
    # module rather than the function, as pylance's own import order avoids.
    for name, module in list(sys.modules.items()):
        if name == "nanolance.lance" or name.startswith("nanolance.lance."):
            sys.modules.setdefault(name[len("nanolance."):], module)


def uninstall_as_lance() -> None:
    """Undo install_as_lance: ``import lance`` finds whatever it found before."""
    if _finder in sys.meta_path:
        sys.meta_path.remove(_finder)
    for name in [n for n in sys.modules if n == "lance" or n.startswith("lance.")]:
        if getattr(sys.modules[name], "__name__", "").startswith("nanolance."):
            del sys.modules[name]
