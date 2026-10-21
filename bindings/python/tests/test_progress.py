# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Write and index progress callbacks, checked against pylance's (lance.progress) where it is the reference."""

import json

from nanolance.lance.fragment import FragmentMetadata
from nanolance.lance.progress import FileSystemFragmentWriteProgress
from support import require_pylance


def test_file_system_write_progress_matches_pylance(tmp_path):
    lance = require_pylance()
    fragment = FragmentMetadata(id=3, files=[], physical_rows=0)
    seen = {}
    for name, tracker in (("nanolance", FileSystemFragmentWriteProgress),
                          ("pylance", lance.progress.FileSystemFragmentWriteProgress)):
        base = tmp_path / name
        progress = tracker(str(base), metadata={"test_key": "test_value"})
        progress.begin(fragment)
        in_progress = json.loads((base / "fragment_3.in_progress").read_text())
        begun = sorted(p.name for p in base.iterdir())
        progress.complete(fragment)
        seen[name] = (begun, sorted(p.name for p in base.iterdir()), in_progress,
                      json.loads((base / "fragment_3.json").read_text()))
    assert seen["nanolance"] == seen["pylance"]
    assert seen["nanolance"][0] == ["fragment_3.in_progress", "fragment_3.json"]
    assert seen["nanolance"][1] == ["fragment_3.json"]
    assert seen["nanolance"][2] == {"fragment_id": 3, "metadata": {"test_key": "test_value"}}


def _recorder_class():
    """A FragmentWriteProgress (pylance's base class, which both engines accept) that records its calls."""
    lance = require_pylance()

    class Recorder(lance.progress.FragmentWriteProgress):
        def __init__(self):
            self.calls = []

        def begin(self, fragment, **kwargs):
            self.calls.append(("begin", fragment.id, len(fragment.files), fragment.physical_rows))

        def complete(self, fragment, **kwargs):
            self.calls.append(("complete", fragment.id, len(fragment.files), fragment.physical_rows))

    return Recorder


class _Recorder:
    """Placeholder replaced by _recorder_class(), kept for the readers of the tests below."""

    def __init__(self):
        self.calls = []

    def begin(self, fragment, **kwargs):
        self.calls.append(("begin", fragment.id, len(fragment.files), fragment.physical_rows))

    def complete(self, fragment, **kwargs):
        self.calls.append(("complete", fragment.id, len(fragment.files), fragment.physical_rows))


def _rows(n, start=0):
    import pyarrow as pa

    return pa.table({"a": list(range(start, start + n))})


def test_write_dataset_progress_matches_pylance(tmp_path):
    lance = require_pylance()
    import nanolance.lance as nl

    seen = {}
    for name, engine in (("nanolance", nl), ("pylance", lance)):
        recorder = _recorder_class()()
        engine.write_dataset(_rows(10), tmp_path / name, max_rows_per_file=5, progress=recorder)
        seen[name] = recorder.calls
    # Each file: begin (id 0, rows not yet known), then complete with its rows. nanolance's begin
    # lists the file it will write (pylance's begin lists none), so the file count is left out there.
    assert [c for c in seen["nanolance"] if c[0] == "complete"] == [c for c in seen["pylance"] if c[0] == "complete"]
    assert [c[:2] + c[3:] for c in seen["nanolance"] if c[0] == "begin"] == \
        [c[:2] + c[3:] for c in seen["pylance"] if c[0] == "begin"]
    assert [c for c in seen["nanolance"] if c[0] == "complete"] == [("complete", 0, 1, 5), ("complete", 0, 1, 5)]


def test_write_dataset_append_progress_begins_as_pylance(tmp_path):
    lance = require_pylance()
    import nanolance.lance as nl

    seen = {}
    for name, engine in (("nanolance", nl), ("pylance", lance)):
        engine.write_dataset(_rows(3), tmp_path / name)
        recorder = _recorder_class()()
        engine.write_dataset(_rows(3, 3), tmp_path / name, mode="append", progress=recorder)
        seen[name] = recorder.calls
    # pylance calls begin for an append and no complete; nanolance's complete follows the file's write.
    assert [c[:2] for c in seen["nanolance"] if c[0] == "begin"] == [c[:2] for c in seen["pylance"] if c[0] == "begin"]
    assert [c for c in seen["pylance"] if c[0] == "complete"] == []


def test_fragment_create_failing_write_has_begun(tmp_path):
    """A LanceFragment.create whose reader fails after one batch: begin (with its id) was called, complete was not."""
    import pyarrow as pa

    import nanolance.lance as nl
    lance = require_pylance()

    def failing_reader():
        batch = pa.record_batch([pa.array(range(100))], names=["a"])
        yield batch
        raise RuntimeError("the reader failed")

    seen = {}
    for name, engine in (("nanolance", nl), ("pylance", lance)):
        recorder = _recorder_class()()
        reader = pa.RecordBatchReader.from_batches(pa.schema([pa.field("a", pa.int64())]), failing_reader())
        try:
            engine.LanceFragment.create(tmp_path / name, reader, fragment_id=7, progress=recorder)
        except Exception:
            pass  # the write fails; what matters is what the progress saw before it
        seen[name] = recorder.calls
    assert seen["nanolance"] == seen["pylance"] == [("begin", 7, 1, None)]


class _Events:
    """An index progress callback that records (event, stage, completed, total, unit) as IndexProgress has them."""

    def __init__(self):
        self.events = []

    def __call__(self, event):
        self.events.append((event.event, event.stage, event.completed, event.total, event.unit))


def test_inverted_index_progress_matches_pylance(tmp_path):
    lance = require_pylance()
    import nanolance.lance as nl
    import pyarrow as pa

    rows = [{"text": f"word{i % 7} common text {i}"} for i in range(75)]
    seen = {}
    for name, engine in (("nanolance", nl), ("pylance", lance)):
        uri = tmp_path / name
        engine.write_dataset(pa.Table.from_pylist(rows), uri)
        engine.write_dataset(pa.Table.from_pylist(rows), uri, mode="append")
        ds = engine.dataset(uri)
        events = _Events()
        ds.create_scalar_index(column="text", index_type="INVERTED", remove_stop_words=False,
                               progress_callback=events)
        seen[name] = events.events

    # pylance tokenizes fragments side by side, so the order of its tokenize_docs progress events (and the
    # count its "complete" carries) varies from run to run; its own test checks the stage order and that
    # the largest count is the row count. Compared here: the stages' start / complete order, every other
    # stage's events exactly, and the tokenize_docs counts as a multiset.
    def stages(events):
        return [(e, s) for e, s, *_ in events if e != "progress"]

    def others(events):
        return [ev for ev in events if ev[1] != "tokenize_docs"]

    def tokenized(events):
        return sorted(c for e, s, c, *_ in events if s == "tokenize_docs" and e == "progress")

    assert stages(seen["nanolance"]) == stages(seen["pylance"])
    assert others(seen["nanolance"]) == others(seen["pylance"])
    assert tokenized(seen["nanolance"]) == tokenized(seen["pylance"]) == [75, 150]
    assert stages(seen["nanolance"])[:2] == [("start", "load_data"), ("complete", "load_data")]


def test_ivf_pq_progress_stages_match_pylance(tmp_path):
    lance = require_pylance()
    import nanolance.lance as nl
    import numpy as np
    import pyarrow as pa

    vectors = np.random.default_rng(7).standard_normal((1500, 128)).astype(np.float32)
    table = pa.table({"id": np.arange(1500),
                      "vector": pa.FixedSizeListArray.from_arrays(pa.array(vectors.ravel()), 128)})
    seen = {}
    for name, engine in (("nanolance", nl), ("pylance", lance)):
        ds = engine.write_dataset(table, tmp_path / name)
        events = _Events()
        ds.create_index(column="vector", index_type="IVF_PQ", num_partitions=4, num_sub_vectors=4,
                        progress_callback=events)
        # The k-means iterations are not counted: their number may differ (the stages are compared).
        seen[name] = [(e, s, c, t) for e, s, c, t, _ in events.events if s != "train_ivf" or e != "progress"]
    assert [(e, s) for e, s, *_ in seen["nanolance"]] == [(e, s) for e, s, *_ in seen["pylance"]]
    for stage in ("shuffle", "merge_partitions"):  # the last count each reports
        last = {name: [c for e, s, c, t in events if s == stage and e == "progress"][-1]
                for name, events in seen.items()}
        assert last["nanolance"] == last["pylance"], stage
