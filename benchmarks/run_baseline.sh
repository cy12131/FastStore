#!/usr/bin/env bash
set -uo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
# shellcheck source=benchmarks/config.sh
source "$script_dir/config.sh"

data_dir="${FASTSTORE_DATA_DIR:-$project_dir/data}"
put_payload="$script_dir/generated/put-64k.bin"
active_wrk_pid=
active_sampler_pid=
active_guard_pid=

cleanup_children() {
    for child in "$active_wrk_pid" "$active_sampler_pid" "$active_guard_pid"; do
        if [[ -n "$child" ]]; then
            kill "$child" 2>/dev/null || true
            wait "$child" 2>/dev/null || true
        fi
    done
}
trap cleanup_children EXIT
trap 'exit 130' INT TERM

for dependency in wrk hey python3 pgrep readlink df; do
    if ! command -v "$dependency" >/dev/null 2>&1; then
        echo "missing benchmark dependency: $dependency" >&2
        exit 1
    fi
done

if [[ -n "${BENCH_SERVER_PID:-}" ]]; then
    server_pid="$BENCH_SERVER_PID"
else
    mapfile -t server_pids < <(pgrep -x faststore || true)
    if [[ "${#server_pids[@]}" -ne 1 ]]; then
        echo "expected exactly one running faststore process; set BENCH_SERVER_PID explicitly" >&2
        exit 1
    fi
    server_pid="${server_pids[0]}"
fi

if [[ ! "$server_pid" =~ ^[0-9]+$ || ! -r "/proc/$server_pid/status" ]]; then
    echo "invalid or unavailable FastStore PID: $server_pid" >&2
    exit 1
fi
server_executable=$(readlink -f "/proc/$server_pid/exe")
server_build_dir=$(dirname -- "$server_executable")
build_type=unknown
if [[ -r "$server_build_dir/CMakeCache.txt" ]]; then
    detected_build_type=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' \
        "$server_build_dir/CMakeCache.txt" | head -n 1)
    if [[ -n "$detected_build_type" ]]; then
        build_type=$detected_build_type
    fi
fi
if [[ "$(basename -- "$server_executable")" != "faststore" ]]; then
    echo "PID $server_pid is not a faststore executable: $server_executable" >&2
    exit 1
fi

logging_mode="${BENCH_LOGGING_MODE:-normal}"
if [[ "$logging_mode" != "normal" && "$logging_mode" != "quiet" ]]; then
    echo "BENCH_LOGGING_MODE must be normal or quiet" >&2
    exit 1
fi
if [[ "$logging_mode" == "quiet" ]] &&
    ! tr '\0' '\n' < "/proc/$server_pid/environ" |
        grep -qx 'FASTSTORE_QUIET=1'; then
    echo "quiet benchmark requested, but FastStore lacks FASTSTORE_QUIET=1" >&2
    exit 1
fi

if ! "$script_dir/prepare_data.sh"; then
    echo "benchmark data preparation failed" >&2
    exit 1
fi

if ! python3 - "$BENCH_HOST" "$BENCH_PORT" <<'PY'
import socket
import sys
with socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=3):
    pass
PY
then
    echo "FastStore is not reachable at $BENCH_HOST:$BENCH_PORT" >&2
    exit 1
fi

run_id=$(date -u +%Y%m%dT%H%M%SZ)
result_prefix="${BENCH_RESULT_PREFIX:-}"
if [[ ! "$result_prefix" =~ ^[A-Za-z0-9._-]*$ ]]; then
    echo "BENCH_RESULT_PREFIX contains unsupported characters" >&2
    exit 1
fi
result_dir="$script_dir/results/${result_prefix}${run_id}"
raw_dir="$result_dir/raw"
sample_dir="$result_dir/samples"
mkdir -p "$raw_dir" "$sample_dir"
raw_csv="$result_dir/raw_results.csv"
failure_csv="$result_dir/failures.csv"
printf 'timestamp,workload,concurrency,repeat,phase,reason,raw_output\n' > "$failure_csv"

{
    printf 'timestamp_utc=%s\n' "$run_id"
    printf 'server_pid=%s\nserver_executable=%s\nbuild_type=%s\n' \
           "$server_pid" "$server_executable" "$build_type"
    printf 'host=%s\nport=%s\n' "$BENCH_HOST" "$BENCH_PORT"
    printf 'hostname=%s\n' "$(hostname)"
    printf 'warmup_seconds=%s\nmeasurement_seconds=%s\nrepeats=%s\n' \
           "$WARMUP_SECONDS" "$MEASURE_SECONDS" "$REPEATS"
    printf 'concurrency=%s\n' "${CONCURRENCIES[*]}"
    printf 'workloads=%s\nwrk_max_threads=%s\n' \
           "${WORKLOADS[*]}" "$WRK_MAX_THREADS"
    printf 'page_cache=warm-cache steady-state; drop_caches not used\n'
    printf 'logging_mode=%s\n' "$logging_mode"
    if [[ "$logging_mode" == "quiet" ]]; then
        printf 'logging=high-frequency informational logs suppressed; error logs retained\n'
    else
        printf 'logging=normal informational and error logs enabled\n'
    fi
    printf 'head_file_bytes=4096\nget_4k_file_bytes=4096\n'
    printf 'get_8m_file_bytes=8388608\nput_payload_bytes=65536\n'
    printf 'logical_cpu_count=%s\n' "$(getconf _NPROCESSORS_ONLN)"
    printf 'cpu_model='
    sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n 1
    sed -n 's/^MemTotal:[[:space:]]*/memory_kib=/p' /proc/meminfo | head -n 1
    wrk --version 2>&1 | head -n 1
    if command -v dpkg-query >/dev/null 2>&1 && \
        dpkg-query -W -f='hey=${Version}\n' hey 2>/dev/null; then
        :
    else
        printf 'hey_binary=%s\n' "$(command -v hey)"
    fi
    printf 'kernel='; uname -srvm
    printf 'git_commit='; git -C "$project_dir" rev-parse HEAD 2>/dev/null || true
    printf 'git_status_begin\n'
    git -C "$project_dir" status --short 2>/dev/null || true
    printf 'git_status_end\n'
} > "$result_dir/environment.txt"

record_failure() {
    local workload=$1 concurrency=$2 repeat=$3 phase=$4 reason=$5 raw=$6
    python3 - "$failure_csv" "$workload" "$concurrency" "$repeat" \
        "$phase" "$reason" "$raw" <<'PY'
import csv
from datetime import datetime, timezone
import sys
with open(sys.argv[1], "a", newline="", encoding="utf-8") as output:
    csv.writer(output).writerow([
        datetime.now(timezone.utc).isoformat(), *sys.argv[2:]
    ])
PY
}

build_wrk_command() {
    local workload=$1 concurrency=$2 duration=$3 prefix=$4
    local threads=$concurrency
    if (( threads > WRK_MAX_THREADS )); then
        threads=$WRK_MAX_THREADS
    fi
    local base_url="http://$BENCH_HOST:$BENCH_PORT"
    client_command=(wrk --latency --timeout "$WRK_TIMEOUT" \
        -t "$threads" -c "$concurrency" -d "${duration}s")
    case "$workload" in
        head)
            client_command=(hey -m HEAD -disable-keepalive \
                -cpus "$threads" -c "$concurrency" -z "${duration}s" \
                -t 5 "$base_url/files/faststore-bench-small-4k.bin")
            ;;
        get-4k)
            client_command+=( -s "$script_dir/get.lua" \
                "$base_url/files/faststore-bench-small-4k.bin" )
            ;;
        get-8m)
            client_command+=( -s "$script_dir/get.lua" \
                "$base_url/files/faststore-bench-large-8m.bin" )
            ;;
        put-64k)
            client_command+=( -s "$script_dir/put.lua" "$base_url" -- \
                "$prefix" "$put_payload" )
            ;;
        *)
            echo "unknown workload: $workload" >&2
            return 1
            ;;
    esac
}

run_client_guarded() {
    local workload=$1 raw_output=$2
    "${client_command[@]}" >"$raw_output" 2>&1 &
    active_wrk_pid=$!
    disk_guard_marker="$raw_output.disk-low"

    if [[ "$workload" == "put-64k" ]]; then
        (
            while kill -0 "$active_wrk_pid" 2>/dev/null; do
                available_kib=$(df -Pk "$data_dir" | awk 'NR == 2 {print $4}')
                if (( available_kib < MIN_FREE_DISK_KIB )); then
                    : > "$disk_guard_marker"
                    kill "$active_wrk_pid" 2>/dev/null || true
                    exit 1
                fi
                sleep 1
            done
        ) &
        active_guard_pid=$!
    else
        active_guard_pid=
    fi

    wait "$active_wrk_pid"
    wrk_status=$?
    active_wrk_pid=
    if [[ -n "$active_guard_pid" ]]; then
        kill "$active_guard_pid" 2>/dev/null || true
        wait "$active_guard_pid" 2>/dev/null || true
        active_guard_pid=
    fi
    if [[ -e "$disk_guard_marker" ]]; then
        return 90
    fi
    return "$wrk_status"
}

failures=0
for workload in "${WORKLOADS[@]}"; do
    for concurrency in "${CONCURRENCIES[@]}"; do
        for ((repeat = 1; repeat <= REPEATS; ++repeat)); do
            prefix="${run_id}-${concurrency}-${repeat}-$(date +%s%N)"
            warmup_raw="$raw_dir/${workload}-c${concurrency}-r${repeat}-warmup.txt"
            measurement_raw="$raw_dir/${workload}-c${concurrency}-r${repeat}.txt"
            samples="$sample_dir/${workload}-c${concurrency}-r${repeat}.csv"
            printf 'Running %-8s c=%-3s repeat=%s warm-up...\n' \
                   "$workload" "$concurrency" "$repeat"

            build_wrk_command "$workload" "$concurrency" \
                "$WARMUP_SECONDS" "warmup-$prefix" || exit 1
            run_client_guarded "$workload" "$warmup_raw"
            warmup_status=$?
            if [[ "$workload" == "put-64k" ]]; then
                "$script_dir/cleanup_data.sh"
                cleanup_status=$?
            else
                cleanup_status=0
            fi
            if [[ "$workload" == "head" ]]; then
                warmup_tool=hey
            else
                warmup_tool=wrk
            fi
            if (( warmup_status == 0 )) && ! python3 \
                "$script_dir/validate_output.py" --tool "$warmup_tool" \
                --output "$warmup_raw"; then
                warmup_status=91
            fi
            if (( warmup_status != 0 || cleanup_status != 0 )); then
                reason="warm-up failed: wrk=$warmup_status cleanup=$cleanup_status"
                record_failure "$workload" "$concurrency" "$repeat" \
                    warmup "$reason" "$warmup_raw"
                failures=$((failures + 1))
                continue
            fi

            printf 'Running %-8s c=%-3s repeat=%s measurement...\n' \
                   "$workload" "$concurrency" "$repeat"
            build_wrk_command "$workload" "$concurrency" \
                "$MEASURE_SECONDS" "$prefix" || exit 1
            python3 "$script_dir/sample_process.py" \
                --pid "$server_pid" --duration "$MEASURE_SECONDS" \
                --interval "$SAMPLE_INTERVAL_SECONDS" --output "$samples" &
            active_sampler_pid=$!
            run_client_guarded "$workload" "$measurement_raw"
            wrk_status=$?
            wait "$active_sampler_pid"
            sampler_status=$?
            active_sampler_pid=

            if [[ "$workload" == "put-64k" ]]; then
                "$script_dir/cleanup_data.sh"
                cleanup_status=$?
            else
                cleanup_status=0
            fi

            if (( wrk_status != 0 || sampler_status != 0 || cleanup_status != 0 )); then
                reason="measurement failed: wrk=$wrk_status sampler=$sampler_status cleanup=$cleanup_status"
                record_failure "$workload" "$concurrency" "$repeat" \
                    measurement "$reason" "$measurement_raw"
                failures=$((failures + 1))
                continue
            fi

            if [[ "$workload" == "head" ]]; then
                parser_command=(python3 "$script_dir/parse_hey.py"
                    --hey-output "$measurement_raw")
            else
                parser_command=(python3 "$script_dir/parse_wrk.py"
                    --wrk-output "$measurement_raw")
            fi
            if ! "${parser_command[@]}" --samples "$samples" \
                --csv "$raw_csv" --workload "$workload" \
                --concurrency "$concurrency" --repeat "$repeat" \
                --duration "$MEASURE_SECONDS"; then
                record_failure "$workload" "$concurrency" "$repeat" \
                    parse "output validation or parsing failed" "$measurement_raw"
                failures=$((failures + 1))
            fi
        done
    done
done

if [[ -s "$raw_csv" ]]; then
    python3 "$script_dir/summarize.py" --raw "$raw_csv" \
        --csv "$result_dir/summary.csv" \
        --markdown "$result_dir/summary.md"
fi

printf 'Benchmark results: %s\n' "$result_dir"
if (( failures != 0 )); then
    printf '%s run(s) failed; see %s\n' "$failures" "$failure_csv" >&2
    exit 1
fi
printf 'All benchmark runs completed successfully.\n'
