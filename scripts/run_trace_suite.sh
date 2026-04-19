#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 6 ]]; then
  echo "Usage: $0 <binary> <label> [warmup_instructions] [simulation_instructions] [trace_dir] [log_root]" >&2
  exit 1
fi

binary_path=$1
label=$2
warmup_instructions=${3:-10000000}
simulation_instructions=${4:-50000000}
trace_dir=${5:-traces}
log_root=${6:-logs}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

binary="$repo_root/$binary_path"
trace_root="$repo_root/$trace_dir"
output_dir="$repo_root/$log_root/$label"
run_info="$output_dir/_run_info.txt"

traces=(
  "600.perlbench_s-210B.champsimtrace.xz"
  "605.mcf_s-1152B.champsimtrace.xz"
  "619.lbm_s-2676B.champsimtrace.xz"
  "620.omnetpp_s-141B.champsimtrace.xz"
)

if [[ ! -x "$binary" ]]; then
  echo "Binary not found or not executable: $binary" >&2
  echo "Build it manually first, then rerun this script." >&2
  exit 1
fi

mkdir -p "$output_dir"

for trace in "${traces[@]}"; do
  if [[ ! -f "$trace_root/$trace" ]]; then
    echo "Missing trace: $trace_root/$trace" >&2
    exit 1
  fi
done

{
  echo "label=$label"
  echo "binary=$binary"
  echo "warmup_instructions=$warmup_instructions"
  echo "simulation_instructions=$simulation_instructions"
  echo "trace_dir=$trace_root"
  echo "generated_at=$(date --iso-8601=seconds)"
} > "$run_info"

for trace in "${traces[@]}"; do
  base_name=${trace%.champsimtrace.xz}
  short_name=${base_name#*.}
  log_file="$output_dir/$short_name.log"

  echo "Running $trace -> $log_file"
  "$binary" \
    --warmup-instructions "$warmup_instructions" \
    --simulation-instructions "$simulation_instructions" \
    "$trace_root/$trace" > "$log_file" 2>&1
done

echo "Logs written under $output_dir"
