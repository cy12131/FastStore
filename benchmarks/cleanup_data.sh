#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
data_dir="${FASTSTORE_DATA_DIR:-$project_dir/data}"

if [[ ! -d "$data_dir" ]]; then
    echo "data directory does not exist: $data_dir" >&2
    exit 1
fi

# wrk may exit while the Server is still finishing already-received requests.
# Wait briefly, then require five consecutive empty observations so a late
# commit cannot make a single successful find pass look like stable cleanup.
sleep 0.5
empty_observations=0
for ((attempt = 1; attempt <= 200; ++attempt)); do
    find "$data_dir" -maxdepth 1 -type f \
        -name 'faststore-bench-put-*' -delete 2>/dev/null || true

    if find "$data_dir" -maxdepth 1 -type f \
            -name 'faststore-bench-put-*' -print -quit | grep -q .; then
        empty_observations=0
    else
        empty_observations=$((empty_observations + 1))
        if (( empty_observations >= 5 )); then
            if [[ "${1:-}" == "--all" ]]; then
                rm -f "$data_dir/faststore-bench-small-4k.bin" \
                      "$data_dir/faststore-bench-large-8m.bin" \
                      "$script_dir/generated/put-64k.bin"
            fi
            exit 0
        fi
    fi
    sleep 0.05
done

echo "benchmark PUT cleanup did not reach a stable empty state" >&2
exit 1
