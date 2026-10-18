#!/usr/bin/env bash
# Builds nanolance's Python test environment from a package mirror, without reaching PyPI or GitHub.
#
#   UV_DEFAULT_INDEX=https://mirror.example.com/api/pypi/simple \
#   NANOLANCE_GIT_BASE=https://gitlab.example.com/mirrors \
#       tools/python_env_behind_firewall.sh [WORKDIR]
#
# What it does, in WORKDIR (default build/python-mirror, which git ignores):
#   1. copies bindings/python/pyproject.toml into WORKDIR and locks it against the mirror
#      (WORKDIR/uv.lock). The committed bindings/python/uv.lock is left alone: it records PyPI's
#      URLs, which a firewall blocks, while this one records the mirror's.
#   2. installs the locked `test` group into WORKDIR/.venv, from the mirror.
#   3. builds nanolance from this checkout into that environment (its CMake build fetches nanoarrow,
#      zstd and nanom from NANOLANCE_GIT_BASE).
#
# Afterwards:  cd bindings/python && WORKDIR/.venv/bin/python -m pytest
# Keep WORKDIR/uv.lock if you want the exact versions frozen: rerunning the script re-locks.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/bindings/python"
WORK="${1:-$ROOT/build/python-mirror}"

if [ -z "${UV_DEFAULT_INDEX:-}" ]; then
    echo "error: set UV_DEFAULT_INDEX to the simple index URL of your package mirror" >&2
    exit 2
fi
if ! command -v uv >/dev/null 2>&1; then
    echo "error: uv is not installed (get it from your mirror: pip install uv)" >&2
    exit 2
fi

mkdir -p "$WORK"
cp "$PROJECT/pyproject.toml" "$PROJECT/README.md" "$WORK/"

echo "== 1/3 lock against $UV_DEFAULT_INDEX"
(cd "$WORK" && uv lock)

echo "== 2/3 install the locked test group"
(cd "$WORK" && UV_PROJECT_ENVIRONMENT="$WORK/.venv" uv sync --frozen --no-install-project --group test)

echo "== 3/3 build and install nanolance from $PROJECT (editable, as the tests expect)"
uv pip install --python "$WORK/.venv/bin/python" --no-deps -e "$PROJECT"

echo
echo "ready: $WORK/.venv"
echo "test:  cd $PROJECT && $WORK/.venv/bin/python -m pytest"
