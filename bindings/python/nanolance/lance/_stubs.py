# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Placeholders for the parts of pylance's API that nanolance does not implement.

``from lance.util import validate_vector_index`` succeeds, so a module that imports many names but
uses a few keeps working; using the placeholder raises ``NotImplementedError`` naming it.
"""

from __future__ import annotations

import sys
import types

from nanolance.lance._errors import unsupported


class _PlaceholderMeta(type):
    def __call__(cls, *args, **kwargs):
        if getattr(cls, "_nl_placeholder", None) is cls:
            raise unsupported(cls._nl_name)
        return super().__call__(*args, **kwargs)

    def __getattr__(cls, attr):
        if attr.startswith("__"):
            raise AttributeError(attr)
        raise unsupported(f"{cls._nl_name}.{attr}")


def placeholder(qualname: str) -> type:
    cls = _PlaceholderMeta(qualname.rsplit(".", 1)[-1], (object,), {"_nl_name": qualname, "__module__": qualname.rsplit(".", 1)[0]})
    cls._nl_placeholder = cls
    return cls


class StubModule(types.ModuleType):
    """A module whose every public name is a placeholder."""

    def __init__(self, name: str, real_name: str):
        super().__init__(name)
        self._nl_real = real_name
        self.__path__ = []  # a package, so its submodules import too

    def __getattr__(self, attr):
        if attr.startswith("__"):
            raise AttributeError(attr)
        value = placeholder(f"{self._nl_real}.{attr}")
        setattr(self, attr, value)
        return value


# pylance's modules (v12) that nanolance has no implementation of. Importing one gives a StubModule.
PYLANCE_MODULES = {
    "_arrow", "_arrow.bf16", "_datagen", "_dataset", "_dataset.cache", "_dataset.sharded_batch_iterator",
    "arrow", "bitmap", "blob", "commit", "debug", "dependencies", "download", "hf", "indices",
    "indices.builder", "indices.ivf", "indices.pq", "io", "lance", "log", "mem_wal", "namespace", "optimize",
    "otel", "query", "sampler", "schema", "torch", "torch.async_dataset", "torch.bench_utils", "torch.data",
    "torch.dist", "torch.distance", "torch.kmeans", "tracing", "types", "udf", "util", "vector",
}


def stub_module(fullname: str, real_name: str) -> StubModule:
    module = sys.modules.get(fullname)
    if module is None:
        module = StubModule(fullname, real_name)
        sys.modules[fullname] = module
    return module
