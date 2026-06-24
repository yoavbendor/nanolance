# Linux local results (developer machine)

_Not yet run on this machine._

This file holds benchmark numbers from a **local** developer run, kept separate from the
CI-owned [`bench/linux-ci-results.md`](linux-ci-results.md) so the two never collide on the
same path (no more perpetual diffs / merge conflicts between local and CI).

Regenerate it with:

```sh
bench/run-local-bench.sh           # uses ./build
bench/run-local-bench.sh /path/to/build
```

Then commit this file if you want to keep the numbers in the repo. The CI workflow never
touches this file — it only writes `bench/linux-ci-results.md`.
