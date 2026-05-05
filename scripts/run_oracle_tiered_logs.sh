#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$script_dir/run_trace_suite.sh" bin/champsim_oracle_tiered oracle_tiered "$@"
