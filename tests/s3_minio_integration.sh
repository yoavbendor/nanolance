#!/usr/bin/env bash
# Run the built-in S3 reader's live integration test against a throwaway MinIO.
#
# Spins up MinIO in Docker, uploads the deterministic object the test expects (byte[i] == i % 251), points
# the test at it, then tears everything down. Exits 0 (skip) — never failing the build — if Docker is
# unavailable. Pass the already-built test binary as $1, or set S3_TEST_BIN.
#
#   tests/s3_minio_integration.sh build/nano_lance_s3_min_integration_test
#
# Env overrides: MINIO_IMAGE, MINIO_PORT, MINIO_USER, MINIO_PASSWORD, MC_IMAGE.
set -uo pipefail

TEST_BIN="${1:-${S3_TEST_BIN:-}}"
MINIO_IMAGE="${MINIO_IMAGE:-minio/minio:latest}"
MC_IMAGE="${MC_IMAGE:-minio/mc:latest}"
MINIO_PORT="${MINIO_PORT:-9000}"
MINIO_USER="${MINIO_USER:-testkey}"
MINIO_PASSWORD="${MINIO_PASSWORD:-testsecret123}"
BUCKET="blobs"
KEY="capture.bin"
SIZE=100000
CNAME="nanolance-minio-it"

skip() { echo "SKIP: $*"; exit 0; }

[ -n "${TEST_BIN}" ] || skip "no test binary given (arg1 or S3_TEST_BIN)"
[ -x "${TEST_BIN}" ] || skip "test binary '${TEST_BIN}' not found/executable"
command -v docker >/dev/null 2>&1 || skip "docker not available"
docker info >/dev/null 2>&1 || skip "docker daemon not reachable"
command -v python3 >/dev/null 2>&1 || skip "python3 not available (needed to generate the pattern object)"

WORKDIR="$(mktemp -d)"
cleanup() {
    docker rm -f "${CNAME}" >/dev/null 2>&1
    rm -rf "${WORKDIR}"
}
trap cleanup EXIT

echo ">> starting MinIO (${MINIO_IMAGE}) on 127.0.0.1:${MINIO_PORT}"
docker rm -f "${CNAME}" >/dev/null 2>&1
docker run -d --name "${CNAME}" -p "127.0.0.1:${MINIO_PORT}:9000" \
    -e "MINIO_ROOT_USER=${MINIO_USER}" -e "MINIO_ROOT_PASSWORD=${MINIO_PASSWORD}" \
    "${MINIO_IMAGE}" server /data >/dev/null || skip "could not start MinIO (image pull blocked?)"

echo ">> waiting for MinIO to be ready"
ready=""
for _ in $(seq 1 30); do
    if curl -sf -m 2 "http://127.0.0.1:${MINIO_PORT}/minio/health/ready" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
[ -n "${ready}" ] || skip "MinIO did not become ready"

echo ">> generating the pattern object and uploading via mc"
python3 - "${WORKDIR}/${KEY}" "${SIZE}" <<'PY'
import sys
path, size = sys.argv[1], int(sys.argv[2])
with open(path, "wb") as f:
    f.write(bytes((i % 251) for i in range(size)))
PY

# Upload with the mc client container, sharing the host network and the generated file.
docker run --rm --network host -v "${WORKDIR}:/work:ro" --entrypoint /bin/sh "${MC_IMAGE}" -c "
    mc alias set it http://127.0.0.1:${MINIO_PORT} ${MINIO_USER} ${MINIO_PASSWORD} >/dev/null &&
    mc mb -p it/${BUCKET} >/dev/null &&
    mc cp /work/${KEY} it/${BUCKET}/${KEY} >/dev/null
" || skip "could not upload test object (mc image pull blocked?)"

echo ">> running the integration test against s3://${BUCKET}/${KEY}"
AWS_ACCESS_KEY_ID="${MINIO_USER}" \
AWS_SECRET_ACCESS_KEY="${MINIO_PASSWORD}" \
AWS_ENDPOINT_URL="http://127.0.0.1:${MINIO_PORT}" \
AWS_REGION="us-east-1" \
NANOLANCE_S3_TEST_URI="s3://${BUCKET}/${KEY}" \
NANOLANCE_S3_TEST_SIZE="${SIZE}" \
    "${TEST_BIN}"
rc=$?

echo ">> negative control: a wrong secret must be rejected"
if AWS_ACCESS_KEY_ID="${MINIO_USER}" AWS_SECRET_ACCESS_KEY="wrong-secret" \
   AWS_ENDPOINT_URL="http://127.0.0.1:${MINIO_PORT}" AWS_REGION="us-east-1" \
   NANOLANCE_S3_TEST_URI="s3://${BUCKET}/${KEY}" NANOLANCE_S3_TEST_SIZE="${SIZE}" \
   "${TEST_BIN}" >/dev/null 2>&1; then
    echo "FAIL: wrong secret was accepted (endpoint not verifying signatures?)"
    rc=1
else
    echo "ok: wrong secret rejected"
fi

exit "${rc}"
