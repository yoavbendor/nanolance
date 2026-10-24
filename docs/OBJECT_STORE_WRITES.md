# Design: writes to object stores (ROADMAP Wave 3, item 8)

Status: W1-W4 done (2026-10-20/21); every write works on `s3://` except `migrate_manifest_paths_v2`
(local only, 3.2). `cleanup_old_versions` dates objects by the listing's LastModified (nanos3reader
`ObjectInfo::last_modified_ns`); an object the store does not date is treated as just written (kept).
Auto cleanup on commit runs on object stores too (2026-10-22, as Lance's). W5: races and the S3 checker are in CI; uploading while encoding is left (section 7).

## 1. Goal

Every write nanolance does on a local dataset -- create, append, overwrite, delete, update, merge_insert,
schema changes, compaction, index builds, tags, branches, cleanup -- also works on `s3://` (and, through
the same client, S3-compatible stores: MinIO, R2, GCS's XML API), with Lance 12's commit semantics: two
writers never both publish version N, and a writer that loses gets the conflict rules of
`docs/tasks/CONFLICTS.md` (rebase, retry or refuse). pylance and nanolance write the same S3 dataset
concurrently and each reads the other's commits.

Reads from S3 are done (Wave 2 item 5): `io::ByteSource` over nanos3reader, listings, credentials and
endpoints from pylance's `storage_options`.

## 2. What Lance 12 does (measured on moto, pylance 12.0.0; source: lance-table-12.0.0 `io/commit.rs`)

- Handler: `s3://`, `gs://`, `az://` use `ConditionalPutCommitHandler`: the manifest is written with a
  create-only PUT (`If-None-Match: *`); a 412 means another writer took the version. `s3+ddb://` (an
  external manifest store in DynamoDB) is a separate handler -- **out of scope** here (section 8).
- Request sequence of one append (moto log): `PUT data/<name>.lance`, `GET ?list-type=2&prefix=_versions/`
  (latest version), `PUT _transactions/<read_version>-<uuid>.txn`, `PUT _versions/<v2 name>.manifest`.
  A delete adds `GET` of the data files it scans and `PUT _deletions/<frag>-<version>-<random>.arrow`.
- No `_versions/latest_version_hint.json` on S3 (pylance writes it only on local disks). The latest
  version is found by one LIST of `_versions/`: V2 names (`u64::MAX - version`, 20 digits) sort newest
  first.
- File names are random (data files, deletion files, transaction uuids), so every object but the manifest
  is written once by exactly one writer: only the manifest PUT can conflict.

## 3. Design

### 3.1 Write side: `io::ByteSink`

All bytes nanolance writes go through six places (`std::ofstream` / `fopen`): the Lance file writer
(`src/file/data_file_writer.cpp`, also used by every index file via `src/index/index_files.cpp`), deletion files
(`src/table/deletion_vector.cpp`), manifests (`src/table/manifest_writer.cpp`), transaction files
(`src/table/transaction_file.cpp`), Blob v2 sidecars (`src/file/blob_v2_external.cpp`), and refs JSON
(`src/table/dataset_refs.cpp`). Add the write-side twin of `io::ByteSource`:

```cpp
namespace nano_lance::io {
class ByteSink {                       // one object being written, streamed
public:
    virtual ~ByteSink() = default;
    virtual bool write(const void* data, std::size_t size, std::string& error) = 0;
    virtual bool close(std::string& error) = 0;   // the object exists after this, not before
};
std::unique_ptr<ByteSink> open_sink(const std::string& uri, std::string& error);
bool put_if_absent(const std::string& uri, const std::vector<std::uint8_t>& bytes,
                   bool& already_exists, std::string& error);   // the manifest publish
bool remove_object(const std::string& uri, std::string& error);     // cleanup, failed commits
bool make_dirs(const std::string& uri, std::string& error);         // no-op on object stores
}
```

- Local paths: `open_sink` is today's `std::ofstream` (same bytes, same names); `put_if_absent` is
  today's temp file + `link` (`manifest_writer.cpp`).
- S3: `open_sink` writes to a local temporary file and uploads it on `close` with nanos3reader's
  `put_file` (streamed, multipart from 64 MiB, retries, abort on failure). Streaming multipart
  straight from memory is a later optimisation (section 7); the temp file keeps memory bounded and
  reuses tested code. `put_if_absent` is `S3Client::put` with `if_none_match` (412 -> `already_exists`).
- A sink that is destroyed without `close` uploads nothing (an aborted write leaves no object).
- Readers registered for `s3://` are unchanged. Writers that read existing files during a write (a
  delete reading the old deletion file, compaction reading data, index builds reading columns) must read
  through `ByteSource`, never `std::filesystem` (audit in W2).

### 3.2 Commit protocol (`commit_next_version` on an object store)

1. Write every new object (data, deletion, index, sidecar, transaction file) through sinks. All names are
   random; nothing is visible to readers yet.
2. Publish: `put_if_absent(_versions/<name of read_version + 1>)`.
3. `already_exists` -> the version is taken. **First** read that manifest and compare its
   `transaction_file` with ours (each commit's uuid is unique): equal means our own PUT succeeded on an
   earlier attempt whose response was lost (a timeout after the server committed, then a retried PUT
   saw 412). That is success, not a conflict. Lance 12 does not make this check, so a lost response
   there can turn into a second, rebased copy of the same append. nanolance must not do that.
4. Otherwise it is a real conflict: the C1-C3 path (classify every intervening transaction, rebase /
   retry / refuse), then publish again at the new version. The rebased manifest references the same
   uploaded objects; a merged deletion file (C3) is a new object.
5. A commit that fails or is refused leaves its uploaded objects behind, unreferenced. Remove them on a
   best-effort basis (as `remove_transaction_file` does today); whatever remains is found by
   `cleanup_old_versions`, which already deletes unreferenced files (on S3 through LIST + DELETE, W4).

Latest-version discovery for the read version: one LIST of `_versions/` (as pylance). V1-named datasets
on S3 (`{version}.manifest`) work as they do locally (both engines parse every name in the listing; only
Lance's newest-first shortcut needs V2 names); `migrate_manifest_paths_v2` stays local-only, since a rename
on an object store is a copy and a delete, not atomic.

### 3.3 Safety on stores without conditional PUT

AWS S3 (since 2024-08), MinIO (2024+), R2 and GCS honour `If-None-Match: *`. A store that ignores the
header turns step 2 into an overwrite, and two writers can both "win". Lance 12 trusts the store.
nanolance probes it instead, once per bucket and process, before its first publish: a probe object next to
the manifests (`_versions/.nanolance-conditional-put-probe-<random>`) is created, created again (which must
answer 412) and removed. A store that accepts the second PUT is refused for every commit ("does not honour
conditional writes"). Three requests per process and bucket; nothing per commit.

A read-back of the published manifest is **not** a guard: measured in W1 (two Python processes racing on
a store that ignores the header), a later writer's overwrite lands after the earlier writer has read its
own manifest back, and 6 of 12 appends reported done were lost. The read-back is kept only for step 3
(a 412 that may be our own lost reply).

## 4. Tests

- **moto** (CI already runs it for nanos3reader and the O-phase reads; it honours `If-None-Match` and
  `If-Match`, checked by nanos3reader's `put_checks`).
- **Fault injection** in nanos3reader (a test-only hook, `NANOS3READER_FAULTS`): fail the n-th PUT / part /
  complete to a key matching a pattern; return a timeout after the server committed (the lost-response
  case of step 3); ignore `If-None-Match` (the unsafe store of 3.3). nanolance's object-store tests set it.
- **Races**: two processes (nanolance x nanolance, nanolance x pylance) appending, deleting, compacting and
  building indexes on one S3 dataset; afterwards the row multiset equals the serial result, every version
  opens in both engines, no version is skipped or doubled.
- **Interop**: pylance opens every dataset nanolance writes on moto and the reverse (the existing
  `tests/test_object_store*.py` grow write cases).

## 5. Phases

| Phase | Scope | Who |
|---|---|---|
| W1 | `ByteSink` + `put_if_absent`; create / append / overwrite on `s3://` from C++, Python (`storage_options`) and lance-c; the commit protocol of 3.2 including the lost-response check; the guard of 3.3; nanos3reader fault hook; moto tests incl. lost response and two-writer appends | Opus |
| W2 | Mutations on S3: delete, update, merge_insert, add / drop / alter columns, update_config / metadata, restore, tags, compaction (audit write-time reads to `ByteSource`) | Haiku, reviewed |
| W3 | Index builds and optimize_indices on S3 (all kinds: write_file through sinks) | Haiku, reviewed |
| W4 | Maintenance on S3: cleanup_old_versions (LIST + DELETE, the branch-aware rules), drop, branches and clones (`tree/`, `_refs/`), the pylance S3 branch tests that run on moto | Haiku, reviewed |
| W5 | Race and fault suite in CI (two processes, both engines), docs, the speed check (upload throughput vs pylance on moto and on real S3 if credentials are given) | Opus |

## 6. Rules carried over

A local-disk write must produce the same bytes and files as today (the `ByteSink` for paths is the
current code moved behind an interface). The wasm build has no sinks for object stores (reads only).
No credentials in tests or logs beyond moto's `testing` keys.

## 7. Later, not now

- Streaming multipart uploads from memory (no temp file) for the Lance file writer, so parts go up while
  the file is still being encoded. Measured 2026-10-21 on moto (`bench/s3_upload_speed.py`): 512 MiB in
  4.6 s vs pylance's 4.3 s with 8 parts in flight (4.7 / 6.7 s before, with 4); 16 MiB in 0.19 s vs
  0.12 s, the encode-then-upload gap this would close.
- Parallel uploads of a commit's files (index builds write many).
- `gs://` and `az://` native clients (today S3-compatible endpoints only).

## 8. Out of scope

`s3+ddb://` (Lance's DynamoDB external manifest store): only for stores without conditional PUT, which
section 3.3 detects instead. Revisit if a user needs it.
