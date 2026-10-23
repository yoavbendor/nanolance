# SPDX-License-Identifier: Apache-2.0
"""Writes Lindera 3 dictionary directories and user dictionaries for the lindera tokenizer tests, in the
formats lindera-dictionary 3.0.7 reads: metadata.json, char_def.bin and unk.bin (rkyv 0.8 archives),
matrix.mtx, dict.da (a daachorse 2.1 bytewise automaton), dict.vals / dict.wordsidx / dict.words, and a
user dictionary .bin (an rkyv archive of a prefix dictionary). Both engines read what this writes."""

import json
import struct
from collections import deque

# ---------------------------------------------------------------------------------------------------
# daachorse 2.1: DoubleArrayAhoCorasick<u32>::serialize (states, outputs, match kind, num_states).


def daachorse(patterns):
    """`patterns`: [(bytes, u32 value)], distinct and non-empty."""
    children = [{}]  # trie node -> {byte: node}
    term = [None]
    depth = [0]
    for key, value in patterns:
        node = 0
        for c in key:
            nxt = children[node].get(c)
            if nxt is None:
                nxt = len(children)
                children.append({})
                term.append(None)
                depth.append(depth[node] + 1)
                children[node][c] = nxt
            node = nxt
        term[node] = value
    n = len(children)
    fail = [0] * n
    order = []
    queue = deque([0])
    while queue:
        node = queue.popleft()
        order.append(node)
        for c, child in sorted(children[node].items()):
            if node != 0:
                f = fail[node]
                while f != 0 and c not in children[f]:
                    f = fail[f]
                fail[child] = children[f].get(c, 0) if f != 0 or c in children[0] else 0
                if fail[child] == child:
                    fail[child] = 0
            queue.append(child)
    # Outputs: a terminal's own pattern, then (by parent) the outputs of its fail state.
    outputs = []
    out_pos = [0] * n  # 1-based, 0 = none
    for node in order:
        if node == 0:
            continue
        inherited = out_pos[fail[node]]
        if term[node] is not None:
            outputs.append((term[node], depth[node], inherited))
            out_pos[node] = len(outputs)
        else:
            out_pos[node] = inherited
    # Double array: XOR bases, unique, children never at index 0 (root) or 1 (dead).
    size = 256
    used_index = {0, 1}
    used_base = set()
    index_of = {0: 0}
    base_of = {}
    stack = [0]
    while stack:
        node = stack.pop()
        labels = sorted(children[node])
        if not labels:
            continue
        base = 1
        while base in used_base or any((base ^ c) in used_index for c in labels):
            base += 1
        used_base.add(base)
        base_of[node] = base
        for c in labels:
            idx = base ^ c
            used_index.add(idx)
            index_of[children[node][c]] = idx
            stack.append(children[node][c])
    top = max(used_index | {b ^ 255 for b in used_base}) + 1
    size = ((top + 255) // 256) * 256
    states = [[0, 0, 0, 0] for _ in range(size)]  # base, fail, output_pos, check
    label_of = {}
    for node in range(n):
        for c, child in children[node].items():
            label_of[child] = c
    for node in range(n):
        i = index_of[node]
        states[i][0] = base_of.get(node, 0)
        states[i][1] = index_of[fail[node]] if node != 0 else 1
        states[i][2] = out_pos[node]
        states[i][3] = label_of.get(node, 0)
    for i in range(size):
        if i in used_index and i not in (0, 1):
            continue
        # A vacant slot (and the root and dead slots): a check no used base reaches it with.
        for c in range(256):
            if (i ^ c) not in used_base:
                states[i][3] = c
                break
    out = bytearray(struct.pack("<I", size))
    for base, f, opos, check in states:
        out += struct.pack("<III", base, f, (opos << 8) | check)
    out += struct.pack("<I", len(outputs))
    for value, length, parent in outputs:
        out += struct.pack("<III", value, length, parent)
    out += bytes([0])  # MatchKind::Standard
    out += struct.pack("<I", n + 1)
    return bytes(out)


# ---------------------------------------------------------------------------------------------------
# rkyv 0.8: objects are written before what points at them; a Vec is (offset from the field, length).


class Rkyv:
    def __init__(self):
        self.buf = bytearray()

    def align(self, a):
        while len(self.buf) % a:
            self.buf.append(0)

    def write(self, data, align):
        self.align(align)
        at = len(self.buf)
        self.buf += data
        return at

    @staticmethod
    def vec(field_at, target, length):
        return struct.pack("<iI", target - field_at, length)

    def string_target(self, s):
        b = s.encode()
        return self.write(b, 1) if len(b) > 8 else None

    @staticmethod
    def string(field_at, s, target):
        b = s.encode()
        if len(b) <= 8:
            return b + b"\xff" * (8 - len(b))
        n = len(b)
        return struct.pack("<Ii", 0x80 | (n & 0x3F) | ((n >> 6) << 8), target - field_at)


def write_vec_of_u32_vecs(r, rows):
    targets = [r.write(b"".join(struct.pack("<I", v) for v in row), 4) for row in rows]
    r.align(4)
    at = len(r.buf)
    r.buf += b"".join(Rkyv.vec(at + 8 * i, t, len(row)) for i, (t, row) in enumerate(zip(targets, rows)))
    return at


def char_def_bin(categories, mapping):
    """`categories`: [(name, invoke, group, length)]; `mapping`: sorted [(boundary, [category ids])]."""
    r = Rkyv()
    defs = r.write(b"".join(struct.pack("<BBxxI", int(inv), int(grp), ln) for _, inv, grp, ln in categories), 4)
    name_targets = [r.string_target(name) for name, *_ in categories]
    r.align(4)
    names = len(r.buf)
    r.buf += b"".join(Rkyv.string(names + 8 * i, c[0], t) for i, (c, t) in enumerate(zip(categories, name_targets)))
    bounds = r.write(b"".join(struct.pack("<I", b) for b, _ in mapping), 4)
    values = write_vec_of_u32_vecs(r, [ids for _, ids in mapping])
    r.align(4)
    root = len(r.buf)
    r.buf += Rkyv.vec(root, defs, len(categories)) + Rkyv.vec(root + 8, names, len(categories))
    r.buf += Rkyv.vec(root + 16, bounds, len(mapping)) + Rkyv.vec(root + 24, values, len(mapping))
    return bytes(r.buf)


def unk_bin(refs, costs):
    """`refs`: per category, unknown word ids; `costs`: [(cost, left_id, right_id)]."""
    r = Rkyv()
    refs_at = write_vec_of_u32_vecs(r, refs)
    costs_at = r.write(b"".join(struct.pack("<IBBxxhHHxx", i, 0, 2, c, lft, rgt) for i, (c, lft, rgt) in enumerate(costs)), 4)
    words = b""
    idx = []
    for i in range(len(costs)):
        detail = "\0".join(["UNK", str(i)]).encode()
        idx.append(len(words))
        words += struct.pack("<I", len(detail)) + detail
    idx_at = r.write(b"".join(struct.pack("<I", v) for v in idx), 4)
    words_at = r.write(words, 1)
    r.align(4)
    root = len(r.buf)
    r.buf += Rkyv.vec(root, refs_at, len(refs)) + Rkyv.vec(root + 8, costs_at, len(costs))
    r.buf += Rkyv.vec(root + 16, idx_at, len(idx)) + Rkyv.vec(root + 24, words_at, len(words))
    return bytes(r.buf)


def prefix_files(words, system):
    """`words`: [(surface, cost, left, right)] -> (dict.da, dict.vals, dict.wordsidx, dict.words), entries
    grouped by surface in byte order as Lindera's builders write them."""
    groups = {}
    for row_id, (surface, cost, left, right) in enumerate(sorted(words, key=lambda w: w[0].encode())):
        groups.setdefault(surface.encode(), []).append((row_id, cost, left, right))
    vals = b""
    keys = []
    offset = 0
    for key in sorted(groups):
        entries = groups[key]
        keys.append((key, (offset << (8 if system else 5)) | len(entries)))
        offset += len(entries)
        for row_id, cost, left, right in entries:
            vals += struct.pack("<IhHH", row_id, cost, left, right)
    widx = b""
    wdata = b""
    for row_id in range(len(words)):
        detail = "\0".join(["名詞", "一般", str(row_id)]).encode()
        widx += struct.pack("<I", len(wdata))
        wdata += struct.pack("<I", len(detail)) + detail
    return daachorse(keys), vals, widx, wdata


def user_bin(words):
    da, vals, widx, wdata = prefix_files(words, system=False)
    r = Rkyv()
    parts = [r.write(p, 1) for p in (da, vals, widx, wdata)]
    r.align(4)
    root = len(r.buf)
    for i, (at, p) in enumerate(zip(parts, (da, vals, widx, wdata))):
        r.buf += Rkyv.vec(root + 8 * i, at, len(p))
    r.buf += b"\x00\x00\x00\x00"  # is_system = false, padding
    return bytes(r.buf)


IPADIC_METADATA = {
    "name": "test", "encoding": "UTF-8", "default_word_cost": -10000, "default_left_context_id": 5,
    "default_right_context_id": 5, "default_field_value": "*", "flexible_csv": False,
    "skip_invalid_cost_or_id": False, "normalize_details": False,
    "dictionary_schema": {"fields": ["surface", "left_context_id", "right_context_id", "cost", "major_pos",
                                     "pos_detail_1", "pos_detail_2", "pos_detail_3", "conjugation_type",
                                     "conjugation_form", "base_form", "reading", "pronunciation"]},
    "user_dictionary_schema": {"fields": ["surface", "reading", "pronunciation"]},
}

# IPADIC's char.def categories: (name, invoke, group, length), and the ranges that use them.
CATEGORIES = [("DEFAULT", 0, 1, 0), ("SPACE", 0, 1, 0), ("KANJI", 0, 0, 2), ("SYMBOL", 1, 1, 0),
              ("NUMERIC", 1, 1, 0), ("ALPHA", 1, 1, 0), ("HIRAGANA", 0, 1, 2), ("KATAKANA", 1, 1, 2),
              ("KANJINUMERIC", 1, 1, 0), ("GREEK", 1, 1, 0), ("CYRILLIC", 1, 1, 0)]
RANGES = [(0x0, [0]), (0x9, [1]), (0xE, [0]), (0x20, [1]), (0x21, [3]), (0x30, [4]), (0x3A, [3]), (0x41, [5]),
          (0x5B, [3]), (0x61, [5]), (0x7B, [3]), (0x7F, [0]), (0x391, [9]), (0x3CA, [0]), (0x400, [10]),
          (0x530, [0]), (0x3000, [1]), (0x3001, [3]), (0x3007, [3, 8]), (0x3008, [3]), (0x3041, [6]),
          (0x30A0, [7]), (0x30FB, [3]), (0x30FC, [7, 6]), (0x30FD, [7]), (0x3100, [0]), (0x3400, [2]),
          (0x4E00, [2, 8]), (0x4E01, [2]), (0x4E03, [2, 8]), (0x4E04, [2]), (0x9FB0, [0]), (0xFF10, [4]),
          (0xFF1A, [0])]


def write_dictionary(path, words, n_ids, costs, unknown, metadata=None):
    """A system dictionary directory. `words`: [(surface, cost, left, right)]; `costs[f][b]`: the
    connection cost from a right id `f` to a left id `b`; `unknown`: per category [(cost, left, right)]."""
    path.mkdir(parents=True, exist_ok=True)
    (path / "metadata.json").write_text(json.dumps(metadata or IPADIC_METADATA), encoding="utf-8")
    (path / "char_def.bin").write_bytes(char_def_bin(CATEGORIES, RANGES))
    refs, flat = [], []
    for entries in unknown:
        refs.append(list(range(len(flat), len(flat) + len(entries))))
        flat += entries
    (path / "unk.bin").write_bytes(unk_bin(refs, flat))
    matrix = struct.pack("<hhh", -1, n_ids, n_ids)
    matrix += b"".join(struct.pack("<h", costs[f][b]) for b in range(n_ids) for f in range(n_ids))
    (path / "matrix.mtx").write_bytes(matrix)
    da, vals, widx, wdata = prefix_files(words, system=True)
    (path / "dict.da").write_bytes(da)
    (path / "dict.vals").write_bytes(vals)
    (path / "dict.wordsidx").write_bytes(widx)
    (path / "dict.words").write_bytes(wdata)
