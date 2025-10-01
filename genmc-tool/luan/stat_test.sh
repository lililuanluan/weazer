#!/usr/bin/env zsh
# Run all statistical tests over the five benchmark sets.
#
# This script runs two kinds of analyses:
#  1) Cox regression (time-to-event) over per-run durations (Sec) with stratification by Benchmark
#  2) Stratified Mann-Whitney U for coverage rate (Cover/SecElapsed)
#
# Parameters explained (passed to stat_test.py):
#  --analysis cox|coverage   # which analysis to run (cox regression or coverage mann-whitney)
#  --dir <path>              # root directory containing the benchmark set CSVs/logs
#  --m1 <method1>            # baseline method (e.g., Random)
#  --m2 <method2>            # comparison method (e.g., 3phstar)
#  --duration <column>       # duration column name for Cox (default: Sec)
#  --by-benchmark            # optional; for Cox, also print per-benchmark results
#
# Notes:
# - Cox regression expects CSVs having columns: Benchmark, Method(or Caption), and the --duration column.
# - Coverage analysis expects run logs with lines "Iter,Cover,SecElapsed" and uses the last valid line per file.
# - The Python virtualenv is expected at ../.venv relative to this script directory; adjust if needed.

SCRIPT_DIR=${0:a:h}
VENV_PY="$SCRIPT_DIR/../.venv/bin/python"

# Fail early if virtualenv python is missing
if [ ! -x "$VENV_PY" ]; then
  echo "Virtualenv python not found: $VENV_PY"
  echo "Create a venv and install deps (lifelines, pandas, numpy, scipy), then retry."
  exit 1
fi

run_cox() {
  local DIR="$1"
  local NAME="$2"
  echo "\n================ COX: $NAME ($DIR) ================"
  # Skip gracefully if directory empty or missing
  if [ ! -d "$DIR" ] || [ -z "$(find "$DIR" -type f -name '*.csv' 2>/dev/null)" ]; then
    echo "No CSVs found in $DIR, skipping Cox for $NAME"
    return 0
  fi
  "$VENV_PY" "$SCRIPT_DIR/stat_test.py" --analysis cox --dir "$DIR" --m1 Random --m2 3phstar --duration Sec
}

run_coverage() {
  local DIR="$1"
  echo "\n================ COVERAGE: $DIR ================"
  if [ ! -d "$DIR" ] || [ -z "$(ls -A "$DIR" 2>/dev/null)" ]; then
    echo "No files found in $DIR, skipping coverage analysis"
    return 0
  fi
  "$VENV_PY" "$SCRIPT_DIR/stat_test.py" --analysis coverage --dir "$DIR" --m1 Random --m2 3phstar
}

# Benchmark sets
COX_SETS=(
  "$SCRIPT_DIR/out/rff"
  "$SCRIPT_DIR/out/shallow"
  "$SCRIPT_DIR/out/buggy"
  "$SCRIPT_DIR/out/synthetic"
)

COVERAGE_SET="$SCRIPT_DIR/out/coverage"

# Run Cox on the four sets
run_cox "$COX_SETS[1]" "rff"
run_cox "$COX_SETS[2]" "shallow"
run_cox "$COX_SETS[3]" "buggy"
run_cox "$COX_SETS[4]" "synthetic"

# Run coverage Mann-Whitney
run_coverage "$COVERAGE_SET"

echo "\nAll analyses attempted. See messages above for any skipped sets."
