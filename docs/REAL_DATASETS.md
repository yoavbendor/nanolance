# Testing on real data

Two sets of real data test nanolance. Neither is stored in this repository, and nothing here
redistributes it. Each dataset keeps the license its source gives it.

| | what it tests | tool |
|---|---|---|
| **Lance datasets people published** on the Hugging Face Hub: embeddings, images, audio, video, text, LanceDB tables | reading files other writers made, checked against pylance | `tools/real_lance_check.py` |
| **COCO 2017 val and Speech Commands**, raw downloads | writing training data with nanolance and with pylance, then reading it back crosswise, timed | `tools/bench_multimodal.py`, `test_real_datasets.py` |

## Setup on a dev host

```sh
git clone https://github.com/yoavbendor/nanolance && cd nanolance
python -m venv .venv && . .venv/bin/activate
pip install pylance==12.0.0 pyarrow numpy pytest   # pylance is the reference the checks compare against
pip install -e bindings/python                       # builds nanolance (CMake, a C++17 compiler)
```

## 1. Lance datasets from the Hugging Face Hub

The Hub lists every dataset in the Lance format, 110 when this was written:
<https://huggingface.co/datasets?format=format:lance>. They range from a few MB to 7.5 TB, and the
large ones are split into data files of 1 to 70 GB. Nanolance reads single data files, so one or
two files are enough to test a dataset that is too big to download.

```sh
python tools/real_lance_check.py list                 # every Lance dataset on the Hub, smallest first
python tools/real_lance_check.py list --search clip   # ... matching a word
python tools/real_lance_check.py suggested            # a picked set, and what each one holds

# Download (resumable; HF_TOKEN for gated datasets) and check:
python tools/real_lance_check.py fetch lance-format/mnist-lance lhoestq/wiki-dpr-lance-example --dest ~/lance-hub
python tools/real_lance_check.py fetch lance-format/librispeech-clean-lance --only data/dev_clean.lance --dest ~/lance-hub
python tools/real_lance_check.py fetch lance-format/fineweb-edu --max-gb 2 --dest ~/lance-hub   # 1-2 of its 2,913 files
python tools/real_lance_check.py check ~/lance-hub --json hub.json

# Or the whole suggested set, fetched and checked in one go: about 4 GB at the default --max-gb 1.
# A table whose smallest data file is larger is skipped, and the output says how large it is.
python tools/real_lance_check.py run --dest ~/lance-hub
python tools/real_lance_check.py run --dest ~/lance-hub --max-gb 20   # adds laion-1m and openvid files: tens of GB
```

`fetch` downloads a table's manifests, then its smallest data files that fit in `--max-gb`. Indexes
are fetched only with `--indices`, since nanolance does not read them. A complete table is checked
as a dataset. A partial one is checked file by file.

Every table is read by both engines through the same pylance API, and the results are compared:

- **dataset:** schema, `count_rows`, the first `--rows` rows (or every fragment, with `--full`),
  `take` of `--takes` random rows, and `take_blobs` on blob columns.
- **file:** `LanceFileReader.read_range` and `take_rows` on each data file.

Each check reports one of these outcomes:

- `pass`: equal to pylance.
- `type`: equal values, another Arrow type.
- `refused`: nanolance raised an error that names what it does not read.
- `mismatch`: different rows from pylance. This is a wrong result; please report it with the dataset name.
- `pylance`: pylance itself failed.

Timings are the faster of two runs, with files in the page cache. The command exits non-zero on any
mismatch.

### Results (2026-09-28, pylance 12.0.0, this repository at the commit adding this page)

18 of 24 tables pass and none mismatch. All 6 refusals are Lance file format 2.0. Times are
nanolance / pylance, in ms:

| dataset | what is in it | first rows | take 256 random rows |
|---|---|---|---|
| lance-format/mnist-lance train | PNG bytes, labels, 512-d embeddings | 2.8 / 7.8 | 1.0 / 8.0 |
| lance-format/eurosat-lance train | satellite images, 512-d embeddings | 6.1 / 21.1 | 1.3 / 8.0 |
| lance-format/handwriting-ocr train | images, transcriptions | 6.2 / 77.5 | 1.6 / 9.9 |
| lance-format/librispeech-clean-lance dev_clean | audio, transcripts, 384-d embeddings | 224 / 1,345 | 34.7 / 54.0 |
| lance-format/squad-v2-lance train | text, `list<string>` answers, 384-d embeddings | 5.0 / 8.5 | **63.8 / 13.8** |
| lance-format/ms-marco-v2.1-lance validation | passages as `list<string>`, 384-d embeddings | 45.5 / 45.1 | **2,835 / 21.2** |
| davanstrien/emb-test-wiki-lance | 241,787 passages, 384-d embeddings | 7.2 / 18.8 | 1.5 / 8.0 |
| prrao87/tea-hypervectors | images, `large_list<large_string>` | 25.6 / 154.5 | 107 / 163 |
| lancedb/magical_kingdom, Jacob235/fred-vector-index, Litian2002/robotics-papers-vecdb, lhoestq/wiki-dpr-lance-example, carpelan/sbl-lance | LanceDB tables with embeddings | refused: format 2.0 | |
| lance-format/fineweb-edu (its smallest data file) | text, 384-d embeddings | refused: format 2.0 | |

What these runs found:

- **Fixed:** `take` on an embedding column read the whole page. MNIST's 512-d vectors sit in one
  123 MB FullZip page, and 256 rows took 1.4 s (pylance: 8 ms). A fixed-width FullZip row sits at a
  fixed stride, so `take` now reads just those rows: 1.0 ms and 0.5 MB.
  (`test_work_guards.py::test_take_of_embeddings_reads_just_those_rows`)
- **Fixed:** a LanceDB table would not open ("failed to decode manifest protobuf"). A data file
  can list field id -2, for a column the schema dropped, and protobuf spells a negative `int32` as a
  10-byte varint. The manifest decoder refused it.
- **Open:** Lance file format 2.0 (footer `0.3`). LanceDB wrote it by default for a long time, so
  most LanceDB tables on the Hub are 2.0. nanolance reads 2.1 and 2.2 (see `ROADMAP.md`).
- **Open:** `take` on `list<string>` columns in FullZip pages decodes the whole page. MS MARCO's
  `passage_text` reads 193 MB for 256 rows. Scans are at parity. See `ROADMAP.md`.

## 2. COCO 2017 and Speech Commands, written and read by both engines

`tools/bench_multimodal.py`'s header lists the downloads. Put them in one directory:

```sh
mkdir -p ~/datasets && cd ~/datasets
curl -LO https://s3.amazonaws.com/fast-ai-coco/val2017.zip                        # 815 MB, 5,000 JPEGs
curl -LO https://s3.amazonaws.com/fast-ai-coco/annotations_trainval2017.zip       # 241 MB
unzip -q annotations_trainval2017.zip 'annotations/*_val2017.json'
mkdir speech && curl -L https://storage.googleapis.com/download.tensorflow.org/data/speech_commands_test_set_v0.02.tar.gz \
  | tar -xz -C speech                                                              # 4,890 one-second clips
```

`val2017.zip` stays zipped; the script reads the JPEGs from it. Then, from the repository:

```sh
NANOLANCE_DATASETS=~/datasets python -m pytest bindings/python/tests/test_real_datasets.py -v   # 1,000 rows
NANOLANCE_DATASETS=~/datasets NANOLANCE_DATASETS_ROWS=0 python -m pytest bindings/python/tests/test_real_datasets.py
python tools/bench_multimodal.py --data ~/datasets --runs 3 --out /tmp/multimodal.json   # timed; see BENCHMARKS.md
```

The test writes each dataset with nanolance and with pylance. It then checks every read in both
directions, a shuffled epoch by `take`, and the work nanolance does, at 1 and 4 threads. The
benchmark times writing, a scan, a metadata-only read and a shuffled epoch, with Parquet alongside.

## Reporting what you find

A `mismatch` is a wrong result, and the most important thing to report. A `refused` names what is
missing. Either way, the dataset name and the line from `check` are enough to reproduce it.
`nlance-pagelayout <table or data file>` (built with the C++ tools) prints each column's page
layout, which usually says why.
