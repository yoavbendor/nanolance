---
name: delegate-to-haiku
description: Delegate a bounded coding task (implement, fix with a known cause, tests, benchmark, docs) to the newest Haiku with a precise brief, then test and review it before it reaches the session branch. Use whenever work in this repo can be cut into a well-specified piece, or the user says delegate/Haiku. Not for design choices, perf root-causing without a profile, or one-line fixes.
---

# Delegate to Haiku, review before merging

Longer lessons: `docs/DELEGATION.md`. You own the result; Haiku owns the typing.

## 1. Before delegating
- Read/measure first so the brief carries facts: file:line, exact messages, reference behaviour
  (e.g. pylance's source and output). Haiku should not have to discover the spec.
- One bounded item per agent. Measuring *and* building in one session fails: split it.

## 2. Launch
- Agent tool, `model: "haiku"` (resolves to the newest Haiku), `run_in_background: true`.
- Make the worktree yourself and pass its path (`isolation: worktree` fails when cwd is not a repo):
  `git worktree add .claude/worktrees/<name> -b <name> <session-branch>`.

## 3. Brief: every brief has all of these
Goal · verified context · exact change and files · out of scope / do not touch · Definition of Done
(tests by name, commands to run, numbers to report, targets) · stop rule ("if X, stop and report") ·
environment rules · commit message + trailers · report format.
Environment rules to paste: cd into the worktree for every command; never edit the main checkout or
other worktrees; never push; Python via private `pip install --no-deps --target DIR` and drop the
editable hook first (`sys.meta_path[:] = [f for f in sys.meta_path if 'editable' not in
type(f).__name__.lower() and 'nanolance' not in type(f).__module__.lower()]`), no xdist; pytest
`--basetemp` under /tmp/claude-0/<name>; `rm` only with literal paths or `"${VAR:?}"`; disk is small,
delete outputs after measuring; don't commit `bench/results/matrix.json`.
Report: commit hashes, verbatim test summary lines, numbers with machine and load, deviations, not done.

## 4. Review before merging (never skip)
- Read the whole diff, not the summary. Check the claims against code.
- Re-run the DoD checks yourself on the merged tree (build, ctest, the new tests).
- Probe one case the agent did not choose: another reader/version (pylance 12 and 14), hostile input,
  thread counts. Perf: rerun the benchmark once and verify byte-identical output.
- Tests must fail when the feature is undone (sabotage); doc numbers must come from runs.
- Send findings back to the same agent (SendMessage) with exact items; fix yourself only if trivial.
- An agent that stops early or hits a limit: resume it with the exact list of what remains.

## 5. Integrate
`git merge --no-ff` into the session branch, build + ctest, push; remove the worktree and branch;
clean scratch. Tell the user what you verified yourself versus what the agent reported.
