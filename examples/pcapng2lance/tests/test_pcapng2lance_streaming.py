#!/usr/bin/env python3
"""Windowed streaming must produce IDENTICAL output to the whole-file path.

Converts the same capture with the default (huge) window and with tiny windows that force constant
refills, mid-block straddles, the grow path (blocks bigger than the window), and many per-chunk commits
(multiple fragments). Asserts every column + external payload reference matches the default run, and that
the small-window run really did split into multiple fragments. Combined with the realfile oracle test
(which proves the default path correct), this proves the streaming path correct.

argv: <exe> <capture>.  CTest 77 = skipped (pylance missing)."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: test_pcapng2lance_streaming.py <exe> <capture>", file=sys.stderr)
        return 2
    exe, capture = argv[1], argv[2]
    try:
        import lance
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"streaming interop skipped: {exc}", file=sys.stderr)
        return 77

    def convert(out: Path, extra: list[str]) -> None:
        r = subprocess.run([exe, *extra, capture, str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"convert {extra} failed: {r.stderr}")

    def snapshot(path: Path):
        ds = lance.dataset(str(path))
        t = ds.to_table()
        payload = t.column("payload_ref").combine_chunks()
        cols = {name: t.column(name).to_pylist()
                for name in ("packet_id", "ts_raw", "caplen", "origlen", "link_type", "ts_resol", "epb_flags")}
        cols["_pos"] = payload.field("position").to_pylist()
        cols["_size"] = payload.field("size").to_pylist()
        cols["_uri"] = payload.field("blob_uri").to_pylist()
        return ds, t.num_rows, cols

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        whole = tmp_path / "whole.lance"
        small = tmp_path / "small.lance"      # many clean chunks (blocks fit)
        tiny = tmp_path / "tiny.lance"        # forces the grow path (blocks > window)
        convert(whole, [])
        convert(small, ["--window-bytes", "4096"])
        convert(tiny, ["--window-bytes", "64"])

        ds_whole, n_whole, c_whole = snapshot(whole)
        ds_small, n_small, c_small = snapshot(small)
        _ds_tiny, n_tiny, c_tiny = snapshot(tiny)

        assert n_small == n_whole == n_tiny, f"row counts differ: {n_whole}/{n_small}/{n_tiny}"
        assert c_small == c_whole, "small-window output differs from whole-file output"
        assert c_tiny == c_whole, "tiny-window (grow path) output differs from whole-file output"
        # packet_id must be a contiguous global sequence across fragments.
        assert c_whole["packet_id"] == list(range(n_whole)), "packet_id not contiguous"

        # The small-window run must actually have chunked into multiple fragments (bounded writer memory).
        whole_frags = len(ds_whole.get_fragments())
        small_frags = len(ds_small.get_fragments())
        assert whole_frags == 1, f"whole-file run should be 1 fragment, got {whole_frags}"
        assert small_frags > 1, f"small-window run should be multiple fragments, got {small_frags}"

    print(f"pcapng2lance streaming ok ({n_whole} rows identical across whole/4096/64-byte windows; "
          f"{small_frags} fragments when chunked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
