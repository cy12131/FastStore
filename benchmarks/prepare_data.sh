#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)
data_dir="${FASTSTORE_DATA_DIR:-$project_dir/data}"
generated_dir="$script_dir/generated"

mkdir -p "$data_dir" "$generated_dir"

# Dedicated deterministic benchmark files. They are intentionally generated,
# ignored by Git, and never mixed with user-selected filenames.
dd if=/dev/zero of="$data_dir/faststore-bench-small-4k.bin" \
   bs=4096 count=1 status=none
dd if=/dev/zero of="$data_dir/faststore-bench-large-8m.bin" \
   bs=1M count=8 status=none
dd if=/dev/zero of="$generated_dir/put-64k.bin" \
   bs=64K count=1 status=none

small_size=$(stat -c %s "$data_dir/faststore-bench-small-4k.bin")
large_size=$(stat -c %s "$data_dir/faststore-bench-large-8m.bin")
put_size=$(stat -c %s "$generated_dir/put-64k.bin")

if [[ "$small_size" != 4096 || "$large_size" != 8388608 || \
      "$put_size" != 65536 ]]; then
    echo "benchmark data size verification failed" >&2
    exit 1
fi

printf 'Prepared benchmark data:\n  GET small: %s bytes\n  GET large: %s bytes\n  PUT body:  %s bytes\n' \
       "$small_size" "$large_size" "$put_size"
