# Delegating to Haiku: what worked (2026-10-15 to 10-17)

About 21 delegated sessions in three days: EXPR_TYPES (A1-A3, B, C0-C3), MSVC portability, binary
vectors, fuzzy full-text search, count pushdown, io_stats, scan statistics, three small-item
bundles, the WebAssembly spike, the remote read paths, the WASM glue and the weblance page. Opus
(the reviewer) wrote each brief, reviewed each diff, re-ran the checks and committed. Haiku never
committed, pushed or ran `--update`.

## The short version

1. **Measure first, then delegate.** The best sessions started from pylance's behaviour already
   measured and written into the brief: exact messages, types, edge cases. When a session had to
   measure and build at once (phase C), it spent its context measuring, stopped, and the
   measurement became the spec for three smaller sessions (C1-C3) that each landed in one pass.
2. **One bounded item per session, with a stop rule.** "If this needs restructuring shared code,
   stop and ask" saved the most time. Haiku stopped correctly on binary vectors (the vector path is
   float-only: several sessions, so the reviewer cut it to flat search), on phase C, and on
   questions about filter semantics. A clean stop with a question costs minutes; a forced redesign
   costs a day.
3. **The reviewer finds what the tests do not.** Every session's own tests passed; review still
   caught:
   - a cache keyed by filter only, so `count_rows` stayed stale after `delete()`;
   - a reader path that silently returned a short dictionary column (made to fail loudly);
   - `__int128` and `__builtin_clz` breaking MSVC;
   - a test that assumed two scans read identical bytes;
   - license notices filled in from memory;
   - a temp directory left in the repository root by a measurement script.
   Review time goes to keys, versions, concurrency, portability and caching: the places where a
   plausible change is quietly wrong.
4. **Performance work stays with the reviewer.** Haiku implemented the I/O layer and measured
   "regressions" that were its own test runs loading the machine. The real finding of the week, a
   stat storm of ~800 `stat` calls per take, came from the reviewer's strace. Delegate the
   mechanical part; keep root-causing and the before/after verdict.
5. **Budget about 200k tokens a session**, not the 100k the briefs asked for. Sessions used
   120-260k. The 100k instruction still mattered: it kept them reading files in windows instead of
   whole. Two sessions ran out of context and handed back partial reports; one hit the usage
   limit and resumed cleanly when messaged after the reset.

## The brief that worked

Each brief had the same parts, in this order:

1. **Context:** the repo, branch and files to read (with `grep -n` starting points and "never
   read big files whole"), plus the spec section and previous commits to follow.
2. **Scope and out of scope:** the target tests by name and the oracle command that proves them.
   Name the things *not* to do.
3. **Facts already measured:** messages, types, sizes, URLs. Never ask for something to be
   re-measured.
4. **Design constraints:** no slower (with the benchmark to run before and after), portable (no
   `__int128`, no unguarded builtins), filters unchanged, writers stay local, and so on.
5. **The stop rule:** when to stop and ask instead of building.
6. **Rules:** no commits or pushes; scratch files only in a named directory; run the full suites
   **once** at the end, the long one in the background; never two at once; no sleep loops.
7. **Report format:** questions first, then files changed, measured numbers, test results, what
   is left.

## What the reviewer does after each session

1. Read the report, answer its questions, and decide out-of-scope items into the roadmap.
2. Check the diff for stray files (`git status`), debug code and non-portable calls.
3. Read the risky hunks: caches, version handling, anything shared by every read.
4. Verify by **reproducing** the edge case the tests miss: delete then count, nulls and NaN in an
   index, a repeat scan, an empty dataset.
5. Re-run ctest and the targeted tests; trust the session's full pytest only if it ran on the
   same code.
6. For anything performance-related, run an interleaved A/B on a quiet machine yourself.
7. Commit with the session credited and the review fixes named, then push. Update the pylance
   suite's expected list.

## Failure modes seen, and the rule that prevents each

| seen | rule |
|---|---|
| two full pytest runs at once (one spurious failure) | "start the full run once; never a second while it runs" |
| benchmarks run while tests ran: false regressions | benchmark only on a quiet machine, interleaved before/after |
| polling loops in the background, many idle notifications | "wait for the notification; no sleep loops" |
| the `pkill -f` pattern matched the reviewer's own shell | kill by PID, never by a pattern the command line contains |
| answers through this sandbox's proxy took seconds per request | treat network timings here as not representative; measure in a real browser |
| license text "from memory" | copy license files from the source tree; never from memory |

## What not to delegate

- Format research and anything that needs a design call across modules: the I/O interface, caching
  strategy, the decision to split a phase.
- Performance root-causing and the final faster-or-not verdict.
- Publishing decisions: what goes in a public repo, which claims a page makes.

## Cost and yield

About 21 sessions, mostly 120-260k Haiku tokens each. The yield was 29 pylance tests newly passing
(1,171 to 1,200), the expression type system, four search features, the I/O layer and a working
browser demo. Review took roughly a fifth of the effort and caught one correctness bug, one
silent-data bug and two portability bugs that the sessions' own tests passed.
