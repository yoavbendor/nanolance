# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.arrow``: pylance's Arrow extension types -- bfloat16 and the image types -- and ``cast``.

bfloat16 values are stored as ``fixed_size_binary(2)`` under the extension name ``lance.bfloat16``
(little-endian bit patterns). Conversions from floats round to nearest even, going through float32
first, as pylance does. Numpy views of bfloat16 use ``ml_dtypes.bfloat16``.
"""

from __future__ import annotations

import decimal
import io
import json
import numbers
import typing
from pathlib import Path
from typing import Callable, Iterable, Optional, Union

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc

try:  # pandas is optional: without it there is no pandas extension dtype
    import pandas as pd
    from pandas.api.extensions import ExtensionArray as _PdExtensionArray
    from pandas.api.extensions import ExtensionDtype as _PdExtensionDtype
    from pandas.api.extensions import register_extension_dtype
except ImportError:  # pragma: no cover
    pd = None

__all__ = [
    "BFloat16",
    "BFloat16Array",
    "BFloat16Type",
    "PandasBFloat16Array",
    "bfloat16_array",
    "cast",
    "EncodedImageType",
    "FixedShapeImageTensorType",
    "ImageArray",
    "ImageScalar",
    "ImageURIArray",
    "ImageURIType",
]

_BFLOAT16_EXTENSION = "lance.bfloat16"
_NULL_VALUES_MSG = "Cannot convert null values to numpy"


# ---------------------------------------------------------------------------------------------
# bit-level conversions
# ---------------------------------------------------------------------------------------------


def _ml_bfloat16():
    import ml_dtypes  # pyright: ignore[reportMissingImports]

    return ml_dtypes.bfloat16


def _float32_to_bits(values, *, canonical_nan: bool = False) -> np.ndarray:
    """Round float32 values to bfloat16 bit patterns (uint16), nearest even.

    A NaN keeps its top 16 bits with the quiet bit set, as pylance's scalar and list constructors
    do; ``canonical_nan`` gives the sign and the quiet NaN 0x7fc0 instead, as pylance's ``cast`` does.
    """
    f = np.asarray(values, dtype=np.float32)
    raw = f.view(np.uint32)
    # uint64 so that adding the rounding bias cannot wrap for the NaN bit patterns
    rounded = ((raw.astype(np.uint64) + ((raw >> 16) & 1) + 0x7FFF) >> 16).astype(np.uint16)
    if canonical_nan:
        quiet_nan = (((raw >> 16) & 0x8000) | 0x7FC0).astype(np.uint16)
    else:
        quiet_nan = ((raw >> 16) | 0x0040).astype(np.uint16)
    return np.where(np.isnan(f), quiet_nan, rounded).astype(np.uint16)


def _bits_to_float32(bits) -> np.ndarray:
    return (np.asarray(bits, dtype=np.uint16).astype(np.uint32) << 16).view(np.float32)


def _float_scalar_to_bits(value) -> int:
    with np.errstate(over="ignore"):
        return int(_float32_to_bits(np.array([value], dtype=np.float32))[0])


def _as_real(value) -> float:
    if isinstance(value, BFloat16):
        return value.as_float()
    if isinstance(value, (numbers.Real, np.generic)):  # np.generic: e.g. an ml_dtypes bfloat16 scalar
        return float(value)
    raise TypeError(f"argument 'value': must be real number, not {type(value).__name__}")


def _rust_display(bits: int) -> str:
    """Format a bfloat16 bit pattern the way Rust's ``{}`` formats the f32 it widens to."""
    f = _bits_to_float32([bits])[0]
    if np.isnan(f):
        return "NaN"
    if np.isinf(f):
        return "inf" if f > 0 else "-inf"
    if f == 0:
        return "-0" if bits & 0x8000 else "0"
    # Shortest round-trip digits (numpy), then Rust's tie rule: an exact tie rounds away from zero,
    # where numpy rounds to even.
    sign = "-" if f < 0 else ""
    mantissa, _, exponent = np.format_float_scientific(abs(f), unique=True, trim="-").partition("e")
    digits = mantissa.replace(".", "")
    shift = int(exponent) - len(digits) + 1
    with decimal.localcontext() as ctx:
        ctx.prec = 400  # an f32 subnormal has up to ~150 exact decimal digits
        unit = decimal.Decimal(1).scaleb(shift)
        lower = decimal.Decimal(int(digits)).scaleb(shift)
        if decimal.Decimal(float(abs(f))) - lower == unit / 2:
            lower += unit
        return sign + format(lower, "f")


def _bits_from_ml(arr: np.ndarray, label: str) -> np.ndarray:
    if arr.dtype != _ml_bfloat16():
        raise ValueError(f"Cannot convert non-bfloat16 values to {label}")
    return np.ascontiguousarray(arr).view(np.uint16)


def _storage_from_bits(bits: np.ndarray, mask: Optional[np.ndarray]) -> pa.Array:
    n = len(bits)
    data = pa.py_buffer(np.ascontiguousarray(bits, dtype="<u2").tobytes())
    null_count = 0 if mask is None else int(mask.sum())
    valid = None
    if null_count:
        valid = pa.py_buffer(np.packbits(~mask, bitorder="little").tobytes())
    return pa.Array.from_buffers(pa.binary(2), n, [valid, data], null_count=null_count)


def _bf16_array(bits: np.ndarray, mask: Optional[np.ndarray] = None) -> "BFloat16Array":
    return pa.ExtensionArray.from_storage(BFloat16Type(), _storage_from_bits(bits, mask))


def _values_to_bits(values: Iterable) -> tuple[np.ndarray, np.ndarray]:
    """Python values (None for null, reals, BFloat16) to (bit patterns, null mask)."""
    items = list(values)
    mask = np.array([v is None for v in items], dtype=bool)
    floats = np.array(
        [0.0 if v is None else (v.as_float() if isinstance(v, BFloat16) else _as_real(v)) for v in items],
        dtype=np.float64,
    )
    with np.errstate(over="ignore"):
        bits = _float32_to_bits(floats.astype(np.float32))
    return bits, mask


# ---------------------------------------------------------------------------------------------
# BFloat16 scalar
# ---------------------------------------------------------------------------------------------


class BFloat16:
    """A bfloat16 value. Built from any real number, rounded to nearest even."""

    __slots__ = ("_bits",)

    def __init__(self, value):
        self._bits = _float_scalar_to_bits(_as_real(value))

    @classmethod
    def _from_bits(cls, bits: int) -> "BFloat16":
        obj = cls.__new__(cls)
        obj._bits = int(bits) & 0xFFFF
        return obj

    @classmethod
    def from_bytes(cls, data: bytes) -> "BFloat16":
        """Build from 2 little-endian bytes."""
        if not isinstance(data, bytes):
            raise TypeError(
                f"argument 'bytes': {type(data).__name__!r} object is not an instance of 'bytes'"
            )
        if len(data) != 2:
            raise ValueError(f"BFloat16::from_bytes: expected 2 bytes, got {len(data)}")
        return cls._from_bits(int.from_bytes(data, "little"))

    def as_float(self) -> float:
        return float(_bits_to_float32([self._bits])[0])

    def __str__(self) -> str:
        return _rust_display(self._bits)

    def __repr__(self) -> str:
        return _rust_display(self._bits)

    def __eq__(self, other):
        if not isinstance(other, BFloat16):
            return NotImplemented
        return self.as_float() == other.as_float()

    def __lt__(self, other):
        if not isinstance(other, BFloat16):
            return NotImplemented
        return self.as_float() < other.as_float()

    def __le__(self, other):
        if not isinstance(other, BFloat16):
            return NotImplemented
        return self.as_float() <= other.as_float()

    def __gt__(self, other):
        if not isinstance(other, BFloat16):
            return NotImplemented
        return self.as_float() > other.as_float()

    def __ge__(self, other):
        if not isinstance(other, BFloat16):
            return NotImplemented
        return self.as_float() >= other.as_float()

    __hash__ = None  # type: ignore[assignment]  # unhashable, as in pylance


# ---------------------------------------------------------------------------------------------
# BFloat16Type / BFloat16Array / BFloat16Scalar
# ---------------------------------------------------------------------------------------------


class BFloat16Type(pa.ExtensionType):
    def __init__(self):
        pa.ExtensionType.__init__(self, pa.binary(2), _BFLOAT16_EXTENSION)

    def __arrow_ext_serialize__(self):
        return b""

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        return BFloat16Type()

    def __arrow_ext_class__(self):
        return BFloat16Array

    def __arrow_ext_scalar_class__(self):
        return BFloat16Scalar

    def to_pandas_dtype(self):
        return PandasBFloat16Type()

    def __reduce__(self):
        # Workaround to ensure pickle works in earlier versions of PyArrow
        return type(self).__arrow_ext_deserialize__, (
            self.storage_type,
            self.__arrow_ext_serialize__(),
        )


class BFloat16Scalar(pa.ExtensionScalar):
    def as_py(self, **kwargs):
        if self.value is None:
            return None
        return BFloat16._from_bits(int.from_bytes(self.value.as_py(), "little"))

    def __eq__(self, other):
        if isinstance(other, pa.Scalar):
            return super().__eq__(other)
        mine = self.as_py()
        if mine is None or other is None:
            return mine is None and other is None
        if isinstance(other, BFloat16):
            return mine == other
        try:  # a real number is compared exactly, as pylance does: 1.1 is not 1.1015625
            value = _as_real(other)
        except TypeError:
            return NotImplemented
        return mine.as_float() == value

    __hash__ = pa.ExtensionScalar.__hash__


class BFloat16Array(pa.ExtensionArray):
    """An Arrow array of bfloat16 values."""

    def __str__(self) -> str:
        items = ["None" if v is None else str(v) for v in self.to_pylist()]
        return "[\n" + ",\n".join("  " + s for s in items) + "\n]"

    def __repr__(self) -> str:
        return "<lance.arrow.%s object at 0x%016x>\n%s" % (type(self).__name__, id(self), str(self))

    @classmethod
    def from_numpy(cls, arr: np.ndarray) -> "BFloat16Array":
        """From a numpy array of ``ml_dtypes.bfloat16``. NaN stays a value, not a null."""
        return _bf16_array(_bits_from_ml(np.asarray(arr), "BFloat16Array"))

    @classmethod
    def from_pandas(cls, obj) -> "BFloat16Array":
        """From a pandas Series or array; NaN and None become nulls."""
        values = pa.array(obj, from_pandas=True)
        if isinstance(values.type, BFloat16Type):
            return values
        return cast(values, BFloat16Type())

    def to_numpy(self, zero_copy_only: bool = True, writable: bool = False) -> np.ndarray:
        """As a numpy array of ``ml_dtypes.bfloat16``; raises if there are nulls."""
        if self.null_count:
            raise ValueError(_NULL_VALUES_MSG)
        return self._bits().view(_ml_bfloat16())

    def to_pandas(self, **kwargs):
        if pd is None:  # pragma: no cover
            raise ImportError("pandas is required for to_pandas()")
        bits, mask = self._bits(), self._null_mask()
        return pd.Series(PandasBFloat16Array(bits, mask), name="bfloat16")

    def __array__(self, dtype=None, copy=None):
        arr = self.to_numpy()
        return arr if dtype is None else arr.astype(dtype)

    def _bits(self) -> np.ndarray:
        storage = self.storage
        if len(storage) == 0:
            return np.zeros(0, dtype=np.uint16)
        raw = np.frombuffer(storage.buffers()[1], dtype="<u2", count=len(storage) + storage.offset)
        return raw[storage.offset :].copy()

    def _null_mask(self) -> np.ndarray:
        return np.asarray(self.storage.is_null().to_numpy(zero_copy_only=False), dtype=bool)


def bfloat16_array(values) -> BFloat16Array:
    """Build a bfloat16 array from Python reals (None for null), numpy arrays or BFloat16 values."""
    if isinstance(values, np.ndarray) and values.dtype != object:
        bits = _float32_to_bits(values.astype(np.float32))
        return _bf16_array(bits)
    bits, mask = _values_to_bits(values)
    return _bf16_array(bits, mask if mask.any() else None)


if pd is not None:
    from pandas._libs.lib import no_default as _no_default

    class PandasBFloat16Type(_PdExtensionDtype):
        """The pandas dtype ``lance.bfloat16``."""

        name = _BFLOAT16_EXTENSION
        type = BFloat16
        kind = "f"
        na_value = None

        @classmethod
        def construct_array_type(cls):
            return PandasBFloat16Array

        def __str__(self) -> str:
            return self.name

        def __eq__(self, other) -> bool:
            return isinstance(other, PandasBFloat16Type) or other == self.name

        def __hash__(self) -> int:
            return hash(self.name)

        def __reduce__(self):
            return PandasBFloat16Type, ()

    class PandasBFloat16Array(_PdExtensionArray):
        """A pandas extension array of bfloat16 values: uint16 bit patterns plus a null mask."""

        def __init__(self, bits, mask=None, copy: bool = False):
            self._bits_arr = np.array(bits, dtype=np.uint16, copy=copy)
            self._mask = np.zeros(len(self._bits_arr), dtype=bool) if mask is None else np.asarray(mask, dtype=bool)

        @property
        def dtype(self):
            return PandasBFloat16Type()

        def __len__(self) -> int:
            return len(self._bits_arr)

        @property
        def nbytes(self) -> int:
            return self._bits_arr.nbytes + self._mask.nbytes

        def _item(self, i):
            if self._mask[i]:
                return None
            return BFloat16._from_bits(int(self._bits_arr[i]))

        def __getitem__(self, key):
            if isinstance(key, numbers.Integral):
                return self._item(int(key))
            key = pd.api.indexers.check_array_indexer(self, key)
            return PandasBFloat16Array(self._bits_arr[key], self._mask[key], copy=True)

        def __setitem__(self, key, value):
            raise TypeError("PandasBFloat16Array is immutable")

        def __iter__(self):
            for i in range(len(self)):
                yield self._item(i)

        def isna(self) -> np.ndarray:
            return self._mask.copy()

        def copy(self):
            return PandasBFloat16Array(self._bits_arr, self._mask, copy=True)

        def take(self, indices, allow_fill: bool = False, fill_value=None):
            from pandas.api.extensions import take as pd_take

            bits = pd_take(self._bits_arr, indices, allow_fill=allow_fill, fill_value=0)
            mask = pd_take(self._mask, indices, allow_fill=allow_fill, fill_value=True)
            if allow_fill and fill_value is not None:
                fill_bits, fill_mask = _values_to_bits([fill_value])
                fill = np.asarray(indices) == -1
                bits = np.where(fill, fill_bits[0], bits)
                mask = np.where(fill, fill_mask[0], mask)
            return PandasBFloat16Array(bits, mask)

        @classmethod
        def _concat_same_type(cls, to_concat):
            return PandasBFloat16Array(
                np.concatenate([a._bits_arr for a in to_concat]),
                np.concatenate([a._mask for a in to_concat]),
            )

        @classmethod
        def _from_sequence(cls, scalars, *, dtype=None, copy: bool = False):
            if isinstance(scalars, PandasBFloat16Array):
                return scalars.copy() if copy else scalars
            values = [None if (isinstance(v, float) and np.isnan(v)) else v for v in scalars]
            bits, mask = _values_to_bits(values)
            return cls(bits, mask)

        @classmethod
        def _from_factorized(cls, values, original):
            return cls(np.where(values < 0, 0, values).astype(np.uint16), values < 0)

        def _values_for_factorize(self):
            codes = np.where(self._mask, -1, self._bits_arr.astype(np.int64))
            return codes, -1

        def _values_for_argsort(self) -> np.ndarray:
            floats = _bits_to_float32(self._bits_arr).astype(np.float64)
            return np.where(self._mask, np.nan, floats)

        def to_numpy(self, dtype=None, copy: bool = False, na_value=None):
            if na_value is not None and na_value is not _no_default:
                floats = np.where(self._mask, np.nan, _bits_to_float32(self._bits_arr).astype(np.float64))
                return floats.astype(dtype) if dtype is not None else floats
            if self._mask.any():
                raise ValueError(_NULL_VALUES_MSG)
            arr = self._bits_arr.view(_ml_bfloat16())
            return arr if dtype is None else arr.astype(dtype)

        def __array__(self, dtype=None, copy=None):
            return np.array([self._item(i) for i in range(len(self))], dtype=object) if dtype in (None, object) else self.to_numpy(dtype=dtype)

        def astype(self, dtype, copy: bool = True):
            if isinstance(dtype, PandasBFloat16Type) or (isinstance(dtype, str) and dtype == _BFLOAT16_EXTENSION):
                return self.copy()
            if dtype is object or dtype == "object":
                return np.array(list(self), dtype=object)
            if isinstance(dtype, np.dtype) and dtype.kind == "f":
                return self.to_numpy(dtype=dtype, na_value=np.nan)
            return super().astype(dtype, copy=copy)

        def __arrow_array__(self, type=None):
            arr = _bf16_array(self._bits_arr, self._mask if self._mask.any() else None)
            return arr if type is None else arr.cast(type)

        def equals(self, other) -> bool:
            return (
                isinstance(other, PandasBFloat16Array)
                and len(self) == len(other)
                and np.array_equal(self._mask, other._mask)
                and np.array_equal(self._bits_arr[~self._mask], other._bits_arr[~other._mask])
            )

        def __repr__(self) -> str:
            return "PandasBFloat16Array(%s)" % ", ".join(
                "None" if v is None else str(v) for v in self
            )

        def _formatter(self, boxed: bool = False):
            return lambda v: "None" if v is None else str(v)

    register_extension_dtype(PandasBFloat16Type)

    def _pandas_from_numpy(cls, arr):
        return cls(_bits_from_ml(np.asarray(arr), "PandasBFloat16Array"))

    PandasBFloat16Array.from_numpy = classmethod(_pandas_from_numpy)  # type: ignore[attr-defined]

else:  # pragma: no cover

    class PandasBFloat16Type:  # type: ignore[no-redef]
        pass

    class PandasBFloat16Array:  # type: ignore[no-redef]
        pass


pa.register_extension_type(BFloat16Type())


# ---------------------------------------------------------------------------------------------
# cast
# ---------------------------------------------------------------------------------------------


def _is_bf16_target(target_type) -> bool:
    return isinstance(target_type, BFloat16Type) or (
        isinstance(target_type, str) and target_type in ("bfloat16", "bf16")
    )


def cast(arr: pa.Array, target_type: Union[pa.DataType, str], *args, **kwargs) -> pa.Array:
    """Cast an array, extending :func:`pyarrow.compute.cast` for the bfloat16 types.

    Supported: floating arrays <-> bfloat16 arrays (``"bfloat16"``, ``"bf16"`` or
    :class:`BFloat16Type` as target), and fixed-size lists of floats or bfloat16 with the same
    list size. Anything else goes to pyarrow.
    """
    if isinstance(arr.type, BFloat16Type):
        if not pa.types.is_floating(target_type):
            raise ValueError(
                "Only support casting bfloat16 array to floating array," + f"got: {target_type}"
            )
        bits = arr._bits()
        f32 = pa.array(_bits_to_float32(bits), type=pa.float32(), mask=arr._null_mask())
        return f32.cast(target_type)
    if _is_bf16_target(target_type):
        if not pa.types.is_floating(arr.type):
            raise ValueError(
                "Only support casting floating array to bfloat16 array," + f"got: {arr.type}"
            )
        f32 = arr.cast(pa.float32())
        mask = np.asarray(f32.is_null().to_numpy(zero_copy_only=False), dtype=bool)
        values = f32.fill_null(0.0).to_numpy(zero_copy_only=False)
        return _bf16_array(_float32_to_bits(values, canonical_nan=True), mask if mask.any() else None)
    if pa.types.is_fixed_size_list(arr.type) and pa.types.is_fixed_size_list(target_type):
        if arr.type.list_size != target_type.list_size:
            raise ValueError(
                "Only support casting fixed size list to fixed size list "
                f"with the same size, got: {arr.type} to {target_type}"
            )
        size = arr.type.list_size
        flat = arr.values.slice(arr.offset * size, len(arr) * size)
        values = cast(flat, target_type.value_type)
        return pa.FixedSizeListArray.from_arrays(
            values=values, list_size=size, mask=arr.is_null() if arr.null_count else None
        )
    return pc.cast(arr, target_type, *args, **kwargs)


# ---------------------------------------------------------------------------------------------
# image types
# ---------------------------------------------------------------------------------------------


def _is_pyarrow_string_type(t: pa.DataType) -> bool:
    return pa.types.is_string(t) or pa.types.is_large_string(t)


def _is_pyarrow_binary_type(t: pa.DataType) -> bool:
    return pa.types.is_binary(t) or pa.types.is_large_binary(t)


class ImageURIType(pa.ExtensionType):
    def __init__(self, storage_type: pa.DataType = pa.string()):
        if not _is_pyarrow_string_type(storage_type):
            raise ValueError("storage_type must be a string type")
        pa.ExtensionType.__init__(self, storage_type, "lance.arrow.image_uri")

    def __arrow_ext_serialize__(self):
        return b""

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        return ImageURIType(storage_type)

    def __arrow_ext_class__(self):
        return ImageURIArray

    def __arrow_ext_scalar_class__(self):
        return ImageURIScalar

    def __reduce__(self):
        return type(self).__arrow_ext_deserialize__, (self.storage_type, self.__arrow_ext_serialize__())


class EncodedImageType(pa.ExtensionType):
    def __init__(self, storage_type: pa.DataType = pa.binary()):
        if not _is_pyarrow_binary_type(storage_type):
            raise ValueError("storage_type must be a binary type")
        pa.ExtensionType.__init__(self, storage_type, "lance.arrow.encoded_image")

    def __arrow_ext_serialize__(self):
        return b""

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        return EncodedImageType(storage_type)

    def __arrow_ext_class__(self):
        return EncodedImageArray

    def __arrow_ext_scalar_class__(self):
        return EncodedImageScalar

    def __reduce__(self):
        return type(self).__arrow_ext_deserialize__, (self.storage_type, self.__arrow_ext_serialize__())


class FixedShapeImageTensorType(pa.ExtensionType):
    def __init__(self, arrow_type: pa.DataType, shape):
        self.shape = shape
        self.arrow_type = arrow_type
        assert len(shape) > 0
        length = 1
        for dim in shape:
            length *= dim
        pa.ExtensionType.__init__(self, pa.list_(arrow_type, length), "lance.arrow.fixed_shape_image_tensor")

    def __arrow_ext_serialize__(self):
        return json.dumps({"shape": self.shape}).encode()

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        deserialized = json.loads(serialized.decode())
        return FixedShapeImageTensorType(storage_type.value_type, deserialized["shape"])

    def __arrow_ext_class__(self):
        return FixedShapeImageTensorArray

    def __arrow_ext_scalar_class__(self):
        return FixedShapeImageTensorScalar

    def __reduce__(self):
        return type(self).__arrow_ext_deserialize__, (self.storage_type, self.__arrow_ext_serialize__())


pa.register_extension_type(ImageURIType())
pa.register_extension_type(EncodedImageType())
pa.register_extension_type(FixedShapeImageTensorType(pa.uint8(), (0,)))


def _pil_available() -> bool:
    try:
        import PIL  # noqa: F401  # pyright: ignore[reportMissingImports]
    except ImportError:
        return False
    return True


class ImageArray(pa.ExtensionArray):
    def __repr__(self):
        return "<lance.arrow.%s object at 0x%016x>\n%s" % (
            type(self).__name__,
            id(self),
            repr(self.to_pylist()),
        )

    @classmethod
    def from_array(cls, images):
        """Build the matching image array (ImageURIArray, EncodedImageArray or
        FixedShapeImageTensorArray) from a string, binary or fixed-shape tensor array, or an
        iterable of URIs."""
        if isinstance(images, (pa.StringArray, pa.LargeStringArray)):
            return pa.ExtensionArray.from_storage(ImageURIType(images.type), images)
        elif isinstance(images, (pa.BinaryArray, pa.LargeBinaryArray)):
            return pa.ExtensionArray.from_storage(EncodedImageType(images.type), images)
        elif isinstance(images, pa.FixedShapeTensorArray):
            typ = FixedShapeImageTensorType(images.type.value_type, images.type.shape)
            return pa.ExtensionArray.from_storage(typ, images.storage)
        elif isinstance(images, (ImageURIArray, EncodedImageArray, FixedShapeImageTensorArray)):
            return images
        elif isinstance(images, (list, tuple, Iterable)):
            return pa.ExtensionArray.from_storage(ImageURIType(), pa.array(images, type=pa.string()))
        else:
            raise TypeError("Cannot build a ImageArray from {}".format(type(images)))


class ImageURIArray(ImageArray):
    """Array of image URIs: local files, or remote files on the web, S3 or GCS if reachable."""

    @classmethod
    def from_uris(cls, uris: Union[pa.StringArray, pa.LargeStringArray, Iterable[Union[str, Path]]]):
        """Build from an array or iterable of URIs (such as a list of str or Path)."""
        storage: pa.Array
        if isinstance(uris, (pa.StringArray, pa.LargeStringArray)):
            storage = uris
        elif isinstance(uris, Iterable):
            storage = pa.array((str(uri) for uri in uris), type=pa.string())
        else:
            raise TypeError("Cannot build a ImageURIArray from {}".format(type(uris)))
        return cls.from_storage(ImageURIType(storage.type), storage)

    def read_uris(self, storage_type=pa.binary()) -> "EncodedImageArray":
        """Read the image bytes behind each URI into an EncodedImageArray.

        Use ``storage_type=pa.large_binary()`` for more than 2GiB of data.
        """
        from urllib.error import URLError
        from urllib.parse import urlparse
        from urllib.request import Request, urlopen

        from pyarrow import fs

        def download(url):
            try:
                return urlopen(Request(url)).read()
            except URLError as e:
                if hasattr(e, "reason"):
                    print("Failed to reach the server: ", e.reason)
                elif hasattr(e, "code"):
                    print("The server could not fulfill the request. Error code: ", getattr(e, "code"))
                return None

        images = []
        for uri in self.storage:
            parsed_uri = urlparse(uri.as_py())
            if parsed_uri.scheme in ("http", "https"):
                images.append(download(uri.as_py()))
            else:
                filesystem, path = fs.FileSystem.from_uri(uri.as_py())
                with filesystem.open_input_stream(path) as f:
                    images.append(f.read())
        return EncodedImageArray.from_storage(EncodedImageType(storage_type), pa.array(images, type=storage_type))


class EncodedImageArray(ImageArray):
    """Array of encoded images (any format; the format is not stored, only the bytes)."""

    def __repr__(self):
        decoder = None
        if _pil_available():

            def decoder(images):
                from PIL import Image  # pyright: ignore[reportMissingImports]

                return Image.open(io.BytesIO(images[0].as_py()))

        return "<lance.arrow.%s object at 0x%016x>%s\n%s" % (
            type(self).__name__,
            id(self),
            "\n[" + repr(decoder(self)) + ", ..]" if decoder else "",
            repr(self.to_pylist()[0][:30]),
        )

    def to_tensor(self, decoder: Optional[Callable[[Union[pa.BinaryArray, pa.LargeBinaryArray]], np.ndarray]] = None):
        """Decode the images into a FixedShapeImageTensorArray. Without ``decoder`` this needs Pillow;
        all images must have the same shape."""
        if not hasattr(pa, "FixedShapeTensorType"):
            raise NotImplementedError("This function requires PyArrow >= 12.0.0")
        if not decoder:
            if not _pil_available():
                raise ValueError("No image decoder available. Please install pillow or pass a decoder argument.")

            def decoder(images) -> "np.ndarray":
                from PIL import Image  # pyright: ignore[reportMissingImports]

                return np.stack([np.asarray(Image.open(io.BytesIO(img))) for img in images.to_pylist()])

        image_array = decoder(self.storage)
        if isinstance(image_array, pa.FixedShapeTensorType):
            tensor = typing.cast("pa.Array", image_array)
            shape = tensor.shape
            arrow_type = tensor.storage_type
            tensor_array = tensor
        else:
            shape = image_array.shape[1:]
            arrow_type = pa.from_numpy_dtype(image_array.dtype)
            tensor_array = pa.FixedShapeTensorArray.from_numpy_ndarray(image_array)
        return pa.ExtensionArray.from_storage(FixedShapeImageTensorType(arrow_type, shape), tensor_array.storage)


class FixedShapeImageTensorArray(ImageArray):
    """Array of fixed-shape tensors holding image pixels."""

    def to_numpy(self):
        """As a numpy array of shape (n, *image shape)."""
        ext_type = pa.fixed_shape_tensor(self.storage.type.value_type, self.type.shape)
        tensor_array = pa.ExtensionArray.from_storage(ext_type, self.storage)
        return tensor_array.to_numpy_ndarray()

    def to_encoded(self, encoder=None, storage_type=pa.binary()) -> "EncodedImageArray":
        """Encode each image (PNG with Pillow by default) into an EncodedImageArray. Pass
        ``encoder`` (numpy array in, sequence of bytes out) to use another format."""

        def pillow_encoder(x):
            from PIL import Image  # pyright: ignore[reportMissingImports]

            encoded_images = []
            for y in x:
                with io.BytesIO() as buf:
                    Image.fromarray(y).save(buf, format="PNG")
                    encoded_images.append(buf.getvalue())
            return pa.array(encoded_images, type=storage_type)

        if not encoder:
            if not _pil_available():
                raise ValueError("No image encoder available. Please install pillow or pass an encoder argument.")
            encoder = pillow_encoder
        return EncodedImageArray.from_storage(EncodedImageType(storage_type), encoder(self.to_numpy()))


class ImageScalar(pa.ExtensionScalar):
    def as_py(self, **kwargs):
        return self.value.as_py(**kwargs)


class ImageURIScalar(ImageScalar):
    pass


class EncodedImageScalar(ImageScalar):
    pass


class FixedShapeImageTensorScalar(ImageScalar):
    pass
