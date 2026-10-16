# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""lance.tokenize: the analyzer of an INVERTED index, run on a query string (pylance's FtsToken list)."""

from __future__ import annotations

import unicodedata

from nanolance import _nanolance
from nanolance.lance.dataset import LanceDataset, _lance_errors


class FtsToken:
    """One token of ``lance.tokenize``: its text and its position (pylance's FtsToken)."""

    __slots__ = ("text", "position")

    def __init__(self, text: str, position: int):
        self.text = text
        self.position = position

    def __repr__(self) -> str:
        return f'FtsToken(text={_debug_str(self.text)}, position={self.position})'


def _debug_str(text: str) -> str:
    """`text` as Rust's Debug formats a string, which is how pylance's repr shows it."""
    out = ['"']
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch in "\n\r\t\0":
            out.append({"\n": "\\n", "\r": "\\r", "\t": "\\t", "\0": "\\0"}[ch])
        elif ord(ch) < 0x20 or ord(ch) == 0x7F or unicodedata.category(ch) in ("Mn", "Me") or ch in "\u200c\u200d":
            out.append("\\u{%x}" % ord(ch))  # control, and the grapheme extenders (combining marks, ZWJ, ZWNJ)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def tokenize(query, *, analyzer=None, base_tokenizer=None, language=None, max_token_length=Ellipsis,
             lower_case=None, stem=None, remove_stop_words=None, custom_stop_words=None, ascii_folding=None,
             min_ngram_length=None, max_ngram_length=None, prefix_only=None, split_identifiers=None,
             split_on_numerics=None, preserve_original=None, index_operators=None) -> list[FtsToken]:
    """The tokens of `query` under the analyzer settings of an INVERTED index (the same options and
    errors as ``create_scalar_index(index_type="INVERTED")``). An omitted `max_token_length` is 40;
    None disables the limit. Positions are relative to the first token kept."""
    if not isinstance(query, str):
        kind = "None" if query is None else type(query).__name__
        raise TypeError(f"argument 'query': '{kind}' object is not an instance of 'str'")
    options = {
        "analyzer": analyzer, "base_tokenizer": base_tokenizer, "language": language,
        "lower_case": lower_case, "stem": stem, "remove_stop_words": remove_stop_words,
        "custom_stop_words": custom_stop_words, "ascii_folding": ascii_folding,
        "min_ngram_length": min_ngram_length, "max_ngram_length": max_ngram_length,
        "prefix_only": prefix_only, "split_identifiers": split_identifiers,
        "split_on_numerics": split_on_numerics, "preserve_original": preserve_original,
        "index_operators": index_operators,
    }
    if max_token_length is not Ellipsis:
        options["max_token_length"] = max_token_length
    params = LanceDataset._inverted_params(options)
    with _lance_errors():
        pairs = _nanolance._fts_tokenize(params, query)
    first = pairs[0][1] if pairs else 0
    return [FtsToken(text, position - first) for text, position in pairs]
