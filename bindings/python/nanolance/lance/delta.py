"""`LanceDataset.delta`: the rows and transactions between two versions, as pylance's DatasetDelta.

Rows come from the row-version metadata of the newer version (`_row_created_at_version`,
`_row_last_updated_at_version`, with stable row ids): a row created after the older version is
inserted, a row updated after it (and created before it) is updated. Deleted row ids are the row ids
live at the older version that are gone at the newer one, so a row rewritten by an update is not a
deletion.
"""

from __future__ import annotations

from typing import List

import pyarrow as pa

from nanolance.lance._errors import native

_VERSION_COLUMNS = ("_row_created_at_version", "_row_last_updated_at_version")


class DatasetDelta:
    """A view of differences between two versions. Created by :meth:`LanceDataset.delta`."""

    def __init__(self, begin, end):
        self._begin = begin  # LanceDataset at the older version
        self._end = end  # LanceDataset at the newer version

    def list_transactions(self) -> List:
        """The transactions of the versions begin_version + 1 .. end_version (those with a file)."""
        transactions = []
        for version in range(self._begin.version + 1, self._end.version + 1):
            transaction = self._end.read_transaction(version)
            if transaction is not None:
                transactions.append(transaction)
        return transactions

    def get_inserted_rows(self) -> pa.RecordBatchReader:
        """Rows created after the older version and still live at the newer one."""
        begin, end = self._begin.version, self._end.version
        return self._rows(lambda created, updated: (created > begin) & (created <= end))

    def get_updated_rows(self) -> pa.RecordBatchReader:
        """Rows created by the older version or before, updated after it and still live."""
        begin, end = self._begin.version, self._end.version
        return self._rows(lambda created, updated: (created <= begin) & (updated > begin) & (updated <= end))

    def get_deleted_row_ids(self) -> pa.RecordBatchReader:
        """Row ids live at the older version and gone at the newer one, as a `_rowid` column."""
        if not self._begin.has_stable_row_ids:
            raise ValueError("Invalid user input: deleted row ids require stable row ids, "
                             f"version {self._begin.version} does not use them")
        with native():
            before = self._begin.to_table(columns=[], with_row_id=True).column("_rowid")
            after = self._end.to_table(columns=[], with_row_id=True).column("_rowid")
        live = set(after.to_pylist())
        deleted = pa.array([row for row in before.to_pylist() if row not in live], pa.uint64())
        schema = pa.schema([pa.field("_rowid", pa.uint64())])
        return pa.RecordBatchReader.from_batches(schema, [pa.record_batch([deleted], schema=schema)])

    def _rows(self, keep) -> pa.RecordBatchReader:
        end = self._end
        names = list(end.schema.names)
        with native():
            table = end.to_table(columns=names + list(_VERSION_COLUMNS), with_row_id=True)
        created = table.column("_row_created_at_version").to_numpy()
        updated = table.column("_row_last_updated_at_version").to_numpy()
        table = table.filter(pa.array(keep(created, updated)))
        table = table.select(names + ["_rowid", *_VERSION_COLUMNS])
        return pa.RecordBatchReader.from_batches(table.schema, table.to_batches())
