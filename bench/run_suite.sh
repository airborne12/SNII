#!/usr/bin/env bash
# BENCH.7 -- top-level reproducible benchmark-suite driver.
#
# Runs the gated SNII-vs-CLucene suite across the 150K/5M scale matrix and the
# local / real-OSS surfaces, aggregating per-leg exit codes into one suite
# verdict (any threshold breach -> non-zero exit, so CI fails). Each leg invokes
# the snii_bench binary in a gated mode (--bench-out) so it emits a self-
# describing JSONL artifact (BENCH.7 run-header line 1, then one BENCH.3 row per
# scenario) and exits non-zero on any threshold breach (BENCH.4/5/6).
#
# Surfaces:
#   - local cost-model  : always gated (deterministic MeteredFileReader metrics).
#   - real OSS          : gated only when SNII_OSS_AK + SNII_OSS_SK are set AND
#                         the binary was built with -DSNII_WITH_S3=ON; otherwise
#                         the OSS leg is explicitly SKIPPED (noted) and the suite
#                         verdict reflects the local legs only. The suite stays
#                         runnable on a credential-less CI.
#
# Usage:
#   bench/run_suite.sh [--dry-run] [--scales 150000,5000000] [--out DIR]
#                      [--bin PATH] [--seed N]
#
#   --dry-run   print the exact gated commands for every (scale x leg) and exit 0
#               WITHOUT building or running anything (a reviewer audits the full
#               matrix without paying for a multi-minute 5M run).
#   --scales    comma-separated corpus doc-counts (default 150000,5000000).
#   --out       output directory for the per-leg JSONL artifacts (default a temp
#               dir under TMPDIR).
#   --bin       path to the snii_bench binary (default build-bench/bench/snii_bench).
#   --seed      corpus_gen seed pinned into every leg + the run-header (default 1).
#
# Exit code: 0 iff every executed gated leg passed its declared thresholds; the
# first non-zero leg exit is propagated (CI gate). --dry-run always exits 0.

set -u

DRY_RUN=0
SCALES="150000,5000000"
OUT=""
SEED="1"

# Resolve the repo root from this script's location so paths work from anywhere.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BIN="${REPO_ROOT}/build-bench/bench/snii_bench"

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --scales)  SCALES="$2"; shift 2 ;;
    --out)     OUT="$2"; shift 2 ;;
    --bin)     BIN="$2"; shift 2 ;;
    --seed)    SEED="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "${OUT}" ]; then
  OUT="${TMPDIR:-/tmp}/snii_bench_suite_$$"
fi
mkdir -p "${OUT}" 2>/dev/null || true

# OSS gating precondition: both credentials present. The binary additionally
# requires -DSNII_WITH_S3=ON; without it the leg fails fast inside the binary,
# but a missing credential is the common CI case, so we skip cleanly here.
OSS_ENABLED=0
if [ -n "${SNII_OSS_AK:-}" ] && [ -n "${SNII_OSS_SK:-}" ]; then
  OSS_ENABLED=1
fi

# Enumerate (scale, surface, leg) -> exact command. A "leg" is one gated bench
# invocation: the local surface has two (query scenarios + write-side resources),
# the OSS surface has one (real-OSS wall-clock + rounds). Every leg writes its own
# JSONL whose first line is the BENCH.7 run-header.
SUITE_RC=0
RAN=0
SKIPPED=0

run_leg() {
  local desc="$1"; shift
  local out_file="$1"; shift
  # Remaining args are the bench command line (after the binary).
  local cmd=("${BIN}" "$@" "--bench-out" "${out_file}")
  echo "+ [${desc}] ${cmd[*]}"
  if [ "${DRY_RUN}" -eq 1 ]; then
    return 0
  fi
  "${cmd[@]}"
  local rc=$?
  RAN=$((RAN + 1))
  if [ ${rc} -ne 0 ]; then
    echo "  -> leg FAILED (rc=${rc}): ${desc}" >&2
    SUITE_RC=1
  else
    echo "  -> leg PASSED: ${desc}"
  fi
  return ${rc}
}

IFS=',' read -ra SCALE_LIST <<< "${SCALES}"
for scale in "${SCALE_LIST[@]}"; do
  [ -z "${scale}" ] && continue

  # local surface, query golden metrics (serial_rounds / range_gets / bytes).
  run_leg "scale=${scale} surface=local scenarios" \
          "${OUT}/scenarios_local_${scale}.jsonl" \
          --scenarios --docs "${scale}" --seed "${SEED}"

  # local surface, write-side resources (build CPU / peak RSS / disk size).
  run_leg "scale=${scale} surface=local resources" \
          "${OUT}/resources_local_${scale}.jsonl" \
          --resources --engine both --docs "${scale}" --seed "${SEED}"

  # real-OSS surface (wall-clock + rounds). Gated only with credentials.
  if [ "${OSS_ENABLED}" -eq 1 ]; then
    run_leg "scale=${scale} surface=oss" \
            "${OUT}/oss_${scale}.jsonl" \
            --oss --docs "${scale}" --seed "${SEED}"
  else
    echo "+ [scale=${scale} surface=oss] SKIPPED -- SNII_OSS_AK/SNII_OSS_SK unset"
    SKIPPED=$((SKIPPED + 1))
  fi
done

if [ "${DRY_RUN}" -eq 1 ]; then
  echo "=== dry-run: listed commands only, nothing built or executed ==="
  exit 0
fi

echo "=== suite: ${RAN} legs ran, ${SKIPPED} OSS legs skipped, verdict=$([ ${SUITE_RC} -eq 0 ] && echo PASS || echo FAIL) ==="
echo "    artifacts in ${OUT}"
exit ${SUITE_RC}
