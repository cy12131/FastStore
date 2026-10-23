#!/usr/bin/env bash

set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "FastStore install must run as root; use sudo deploy/install.sh" >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "${script_dir}/.." && pwd)"
release_binary="${project_root}/build-release/faststore"
unit_source="${script_dir}/faststore.service"

if [[ ! -f ${release_binary} || ! -x ${release_binary} ]]; then
    echo "Release binary is missing or not executable: ${release_binary}" >&2
    echo "Build it first with: cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release" >&2
    exit 1
fi

if [[ ! -f ${unit_source} ]]; then
    echo "systemd unit is missing: ${unit_source}" >&2
    exit 1
fi

echo "Ensuring dedicated faststore service account exists"
if ! getent group faststore >/dev/null; then
    groupadd --system faststore
fi
if ! getent passwd faststore >/dev/null; then
    useradd --system --gid faststore --home-dir /var/lib/faststore \
        --no-create-home --shell /usr/sbin/nologin faststore
fi

echo "Creating FastStore runtime and persistent data directories"
install -d -o root -g root -m 0755 /opt/faststore /opt/faststore/bin
install -d -o root -g faststore -m 0750 /var/lib/faststore
install -d -o faststore -g faststore -m 0750 /var/lib/faststore/data

echo "Installing Release binary"
install -o root -g root -m 0755 \
    "${release_binary}" /opt/faststore/bin/faststore

echo "Installing systemd unit"
install -o root -g root -m 0644 \
    "${unit_source}" /etc/systemd/system/faststore.service

echo "Reloading systemd units"
systemctl daemon-reload

echo "FastStore installed. Start with: systemctl start faststore"
