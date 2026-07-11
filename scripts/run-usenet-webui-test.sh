#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

test_root="${NASHAWK_TEST_ROOT:-/tmp/nashawk-webui-real}"
config_dir="${NASHAWK_CONFIG_DIR:-$test_root/config}"
download_dir="${NASHAWK_DOWNLOAD_DIR:-$test_root/downloads}"
incomplete_dir="${NASHAWK_INCOMPLETE_DIR:-$test_root/incomplete}"
rpc_bind="${NASHAWK_RPC_BIND:-127.0.0.1}"
rpc_port="${NASHAWK_RPC_PORT:-19091}"
usenet_upload_concurrency="${NASHAWK_USENET_UPLOAD_CONCURRENCY:-40}"
usenet_eviction_min_age_minutes="${NASHAWK_USENET_EVICTION_MIN_AGE_MINUTES:-1}"
usenet_cache_size_mib="${NASHAWK_USENET_CACHE_SIZE_MIB:-0}"
log_level="${NASHAWK_LOG_LEVEL:-info}"
log_file="${NASHAWK_LOG_FILE:-$test_root/daemon.log}"
node_version="${NASHAWK_NODE_VERSION:-24.18.0}"

daemon="$repo_dir/build/daemon/transmission-daemon"
web_home="$repo_dir/web/public_html"
env_file="$repo_dir/.env"

if [[ ! -x "$daemon" ]]; then
    echo "Missing daemon binary: $daemon" >&2
    echo "Build it first with: cmake --build build -j2" >&2
    exit 1
fi

if [[ ! -f "$web_home/index.html" ]]; then
    echo "Missing built Web UI: $web_home/index.html" >&2
    echo "Build it first with: npm run build" >&2
    exit 1
fi

if [[ ! -f "$env_file" ]]; then
    echo "Missing .env file: $env_file" >&2
    echo "Create it with your Usenet settings before starting Usenet mode." >&2
    exit 1
fi

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

if [[ -s "$HOME/.nvm/nvm.sh" ]]; then
    # shellcheck disable=SC1091
    . "$HOME/.nvm/nvm.sh"
    nvm use "$node_version" >/dev/null
fi

if ! command -v nyuu >/dev/null 2>&1; then
    echo "nyuu was not found on PATH after loading the environment." >&2
    echo "Install or select the Node/Nyuu environment, then retry." >&2
    exit 1
fi

mkdir -p "$config_dir" "$download_dir" "$incomplete_dir" "$(dirname -- "$log_file")"

export TRANSMISSION_WEB_HOME="$web_home"

echo "Starting Nashawk Usenet Web UI test daemon"
echo "  RPC: http://$rpc_bind:$rpc_port/transmission/web/"
echo "  Config: $config_dir"
echo "  Downloads: $download_dir"
echo "  Log: $log_file"
echo "  nyuu: $(command -v nyuu)"
echo "Press Ctrl-C to stop."

exec "$daemon" -f \
    -g "$config_dir" \
    -w "$download_dir" \
    --incomplete-dir "$incomplete_dir" \
    -r "$rpc_bind" \
    -p "$rpc_port" \
    -T \
    --usenet-enabled \
    --usenet-upload-concurrency "$usenet_upload_concurrency" \
    --usenet-eviction-enabled \
    --usenet-eviction-min-age-minutes "$usenet_eviction_min_age_minutes" \
    --usenet-cache-size-mib "$usenet_cache_size_mib" \
    --log-level="$log_level" \
    -e "$log_file"
