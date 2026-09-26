#!/usr/bin/env bash

# M6-B baseline parameters. Environment variables may override these values for
# smoke tests without changing the committed baseline definition.
BENCH_HOST="${BENCH_HOST:-127.0.0.1}"
BENCH_PORT="${BENCH_PORT:-8080}"
WARMUP_SECONDS="${WARMUP_SECONDS:-5}"
MEASURE_SECONDS="${MEASURE_SECONDS:-20}"
REPEATS="${REPEATS:-3}"
WRK_MAX_THREADS="${WRK_MAX_THREADS:-4}"
WRK_TIMEOUT="${WRK_TIMEOUT:-5s}"
SAMPLE_INTERVAL_SECONDS="${SAMPLE_INTERVAL_SECONDS:-1}"
MIN_FREE_DISK_KIB="${MIN_FREE_DISK_KIB:-2097152}"
read -r -a CONCURRENCIES <<< "${BENCH_CONCURRENCIES:-1 4 8 16 32 64 128}"
read -r -a WORKLOADS <<< "${BENCH_WORKLOADS:-head get-4k get-8m put-64k}"
