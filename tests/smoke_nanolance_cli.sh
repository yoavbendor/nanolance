#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
#
# `nanolance <sub>` and the standalone binary it replaces must be the same tool.
#
# They are compiled from the same source, so the risk is not that the logic diverges -- it is that the
# dispatcher mangles argv on the way through (drops a flag, miscounts argc, forwards the subcommand
# name as an argument). That is what this checks: the same input, both front doors, byte-identical
# output. No Python required, so it runs in the default smoke set.
set -euo pipefail

nanolance="${1:?nanolance binary path required}"
arrowipc2lance="${2:?arrowipc2lance binary path required}"
nlance2table="${3:?nlance2table binary path required}"
nlance_info="${4:?nlance_info binary path required}"

here="$(cd "$(dirname "$0")" && pwd)"
ipc="$here/golden/strings_binary/input.arrow"
[ -f "$ipc" ] || { echo "missing golden input: $ipc" >&2; exit 1; }

tmpdir="$(mktemp -d)"
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

fail() { echo "smoke_nanolance_cli: $1" >&2; exit 1; }

# --- the dispatcher's own surface -------------------------------------------------------------
"$nanolance" --version | grep -q '^nanolance ' || fail "--version does not print a version"
"$nanolance" --help | grep -q 'nanolance <command>' || fail "--help does not show usage"
for sub in import info cat stitch; do
  "$nanolance" --help | grep -q "^  $sub" || fail "--help does not list the '$sub' command"
done

# No command at all, and an unknown one, are both usage errors -- not silent successes.
if "$nanolance" >/dev/null 2>&1; then fail "bare invocation must fail"; fi
if "$nanolance" frobnicate >/dev/null 2>&1; then fail "an unknown command must fail"; fi
# Redirect to a file rather than piping: these commands exit non-zero on purpose, and `set -o
# pipefail` would blame the pipeline rather than let grep judge the text.
"$nanolance" frobnicate > "$tmpdir/unknown.txt" 2>&1 || true
grep -q "unknown command 'frobnicate'" "$tmpdir/unknown.txt" || fail "unknown command is not named"

# --- import: same dataset through both front doors ---------------------------------------------
"$arrowipc2lance" --create -o "$tmpdir/direct.lance" < "$ipc" 2>/dev/null
"$nanolance" import --create -o "$tmpdir/via_cli.lance" < "$ipc" 2>/dev/null
direct_file="$(find "$tmpdir/direct.lance/data" -name '*.lance' | head -1)"
via_file="$(find "$tmpdir/via_cli.lance/data" -name '*.lance' | head -1)"
cmp -s "$direct_file" "$via_file" || fail "'nanolance import' wrote different bytes than arrowipc2lance"

# --- a flag that takes a VALUE has to survive the forwarding, not just a bare flag --------------
"$arrowipc2lance" --create --compress -l 9 -o "$tmpdir/z9_direct.lance" < "$ipc" 2>/dev/null
"$nanolance" import --create --compress -l 9 -o "$tmpdir/z9_via.lance" < "$ipc" 2>/dev/null
z9_direct="$(find "$tmpdir/z9_direct.lance/data" -name '*.lance' | head -1)"
z9_via="$(find "$tmpdir/z9_via.lance/data" -name '*.lance' | head -1)"
cmp -s "$z9_direct" "$z9_via" || fail "a value-taking flag is mangled on the way through the dispatcher"
# ...and it has to have DONE something, or the comparison above proves nothing.
cmp -s "$z9_via" "$via_file" && fail "--compress -l 9 produced the same bytes as no compression at all"

# --- cat and info: identical output, modulo the program name in diagnostics ---------------------
"$nlance2table" "$tmpdir/direct.lance" --format ndjson > "$tmpdir/cat_direct.txt"
"$nanolance" cat "$tmpdir/direct.lance" --format ndjson > "$tmpdir/cat_via.txt"
cmp -s "$tmpdir/cat_direct.txt" "$tmpdir/cat_via.txt" || fail "'nanolance cat' differs from nlance2table"
[ -s "$tmpdir/cat_via.txt" ] || fail "'nanolance cat' produced nothing"

"$nlance_info" "$tmpdir/direct.lance" > "$tmpdir/info_direct.txt"
"$nanolance" info "$tmpdir/direct.lance" > "$tmpdir/info_via.txt"
cmp -s "$tmpdir/info_direct.txt" "$tmpdir/info_via.txt" || fail "'nanolance info' differs from nlance_info"

# A subcommand's usage text must name itself the way you would retype it.
"$nanolance" info > "$tmpdir/info_usage.txt" 2>&1 || true
grep -q 'Usage: nanolance info' "$tmpdir/info_usage.txt" || fail "'nanolance info' usage names the old binary"
"$nanolance" import --help | grep -q 'Usage: nanolance import' || fail "'nanolance import' usage names the old binary"

echo "smoke_nanolance_cli: OK"
