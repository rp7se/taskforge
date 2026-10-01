#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build-release}"
output_file="${2:-${project_root}/docs/benchmark-results.md}"
benchmark="${build_dir}/taskforge_bench"

if [[ ! -x "${benchmark}" ]]; then
    echo "benchmark executable not found: ${benchmark}" >&2
    echo "build with: cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release --parallel" >&2
    exit 2
fi

build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "${build_dir}/CMakeCache.txt" | head -n 1)"
if [[ "${build_type}" != "Release" ]]; then
    echo "benchmark results require a Release build (found: ${build_type:-unset})" >&2
    exit 2
fi

if grep -qi microsoft /proc/version 2>/dev/null; then
    wsl="yes"
else
    wsl="no"
fi

cpu_model="$(awk -F ': ' '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null || true)"
memory_kib="$(awk '/MemTotal/{print $2; exit}' /proc/meminfo 2>/dev/null || true)"
result_file="$(mktemp)"
trap 'rm -f "${result_file}"' EXIT
"${benchmark}" --scenario all --format text > "${result_file}"

metric() {
    local scenario="$1"
    local key="$2"
    awk -v scenario="${scenario}" -v key="${key}" '
        $1 == "scenario=" scenario { in_scenario = 1; next }
        in_scenario && $1 ~ /^scenario=/ { exit }
        in_scenario && $1 ~ ("^" key "=") {
            sub("^[^=]*=", "", $1)
            print $1
            exit
        }
    ' "${result_file}"
}

elapsed() {
    awk -v scenario="$1" '
        $1 == "scenario=" scenario {
            for (field = 1; field <= NF; ++field) {
                if ($field ~ /^elapsed_ms=/) {
                    sub("^[^=]*=", "", $field)
                    print $field
                    exit
                }
            }
        }
    ' "${result_file}"
}

throughput() {
    awk -v scenario="$1" '
        $1 == "scenario=" scenario {
            for (field = 1; field <= NF; ++field) {
                if ($field ~ /^tasks_per_second=/) {
                    sub("^[^=]*=", "", $field)
                    print $field
                    exit
                }
            }
        }
    ' "${result_file}"
}

{
    echo "# TaskForge Phase 9 benchmark results"
    echo
    echo "## Environment (sanitized)"
    echo
    echo "- Date: $(date --iso-8601=seconds)"
    echo "- OS/kernel/architecture: $(uname -srm)"
    echo "- WSL: ${wsl}"
    echo "- CPU model: ${cpu_model:-unavailable}"
    echo "- Logical CPUs: $(getconf _NPROCESSORS_ONLN)"
    echo "- Memory KiB: ${memory_kib:-unavailable}"
    echo "- Compiler: $(c++ --version | sed -n '1p')"
    echo "- CMake: $(cmake --version | sed -n '1p')"
    echo "- Build type: ${build_type}"
    echo "- Git SHA: $(git -C "${project_root}" rev-parse HEAD)"
    echo
    echo "No usernames, home paths, hostnames, tokens, or environment variables are recorded."
    echo
    echo "## Command"
    echo
    echo "\`\`\`sh"
    echo "cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release"
    echo "cmake --build build-release --parallel"
    echo "./bench/run_benchmarks.sh build-release docs/benchmark-results.md"
    echo "\`\`\`"
    echo
    echo "## Results"
    echo
    echo "| Scenario | Configuration | Tasks / iterations | Elapsed ms | Throughput / latency | Observed invariant |"
    echo "| --- | --- | ---: | ---: | --- | --- |"
    echo "| Launch latency | helper exit 0; 10 warm-ups | $(metric launch iterations) | $(elapsed launch) | p50 $(metric launch p50_us) us; p95 $(metric launch p95_us) us | completed |"
    echo "| Worker scaling (1) | 25 ms sleep; 1 worker | $(metric worker_scaling tasks) | $(metric worker_scaling worker_1_elapsed_ms) | $(metric worker_scaling worker_1_tasks_per_second) tasks/s | n/a |"
    echo "| Worker scaling (2) | 25 ms sleep; 2 workers | $(metric worker_scaling tasks) | $(metric worker_scaling worker_2_elapsed_ms) | $(metric worker_scaling worker_2_tasks_per_second) tasks/s | n/a |"
    echo "| Worker scaling (4) | 25 ms sleep; 4 workers | $(metric worker_scaling tasks) | $(metric worker_scaling worker_4_elapsed_ms) | $(metric worker_scaling worker_4_tasks_per_second) tasks/s | n/a |"
    if [[ -n "$(metric worker_scaling worker_8_tasks_per_second)" ]]; then
        echo "| Worker scaling (8) | 25 ms sleep; 8 workers | $(metric worker_scaling tasks) | $(metric worker_scaling worker_8_elapsed_ms) | $(metric worker_scaling worker_8_tasks_per_second) tasks/s | n/a |"
    fi
    echo "| CPU admission | 4 workers; 2000m capacity; 1000m/task | 100 | $(elapsed resource_admission) | $(throughput resource_admission) tasks/s | max running $(metric resource_admission observed_max_running); disabled control $(metric resource_admission resource_disabled_tasks_per_second) tasks/s |"
    echo "| Memory admission | 3 workers; 256 MiB capacity; 128 MiB/task | 100 | $(elapsed memory_admission) | $(throughput memory_admission) tasks/s | max running $(metric memory_admission observed_max_running) |"
    echo "| Queue backpressure | 2 workers; queue capacity 8; burst 32 | $(metric bounded_queue_backpressure accepted) accepted | $(elapsed bounded_queue_backpressure) | $(throughput bounded_queue_backpressure) tasks/s | queue_full $(metric bounded_queue_backpressure queue_full); completed $(metric bounded_queue_backpressure completed) |"
    echo "| Bounded output | 16 MiB stdout + 16 MiB stderr; 64 KiB/stream retained | 1 | $(elapsed bounded_output_capture) | retained $(metric bounded_output_capture stdout_retained_bytes) + $(metric bounded_output_capture stderr_retained_bytes) bytes | totals $(metric bounded_output_capture stdout_total_bytes) + $(metric bounded_output_capture stderr_total_bytes) bytes; truncated |"
    echo "| Mixed stress | deterministic 12-case pattern | $(metric mixed_stress accepted) accepted | $(elapsed mixed_stress) | $(throughput mixed_stress) tasks/s | completed $(metric mixed_stress completed); unique IDs $(metric mixed_stress unique_id_count) |"
    echo
    echo "\`\`\`text"
    cat "${result_file}"
    echo "\`\`\`"
    echo
    echo "## Methodology and limitations"
    echo
    echo "- Launch uses 10 warm-ups and 100 measured helper exit-zero processes; p95 is the nearest-rank percentile."
    echo "- Worker scaling uses the same 25 ms sleep workload and task count for each worker count."
    echo "- Resource scenarios validate declared logical admission budgets, not host CPU utilization or cgroup limits."
    echo "- Output capture retains the first 64 KiB per stream while draining and accounting for 16 MiB per stream."
    echo "- Mixed stress is deterministic and verifies accepted/completed equality and task-ID uniqueness."
    echo "- Measurements are local observations, not cross-machine performance claims or CI timing gates."
} > "${output_file}"
