#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${NASHAWK_BUILD_DIR:-$repo_dir/build}"
out_dir="${NASHAWK_PACKAGE_DIR:-$repo_dir/dist}"
package_name="${NASHAWK_PACKAGE_NAME:-nashawk-vps-amd64}"
version="${NASHAWK_PACKAGE_VERSION:-$(git -C "$repo_dir" rev-parse --short=12 HEAD 2>/dev/null || date +%Y%m%d%H%M)}"
staging_root="$(mktemp -d "${TMPDIR:-/tmp}/nashawk-package.XXXXXX")"
staging_dir="$staging_root/$package_name"
archive="$out_dir/$package_name-$version.tar.gz"

cleanup()
{
    rm -rf "$staging_root"
}
trap cleanup EXIT

daemon="$build_dir/daemon/transmission-daemon"
web_home="$repo_dir/web/public_html"
env_example="$repo_dir/.env.example"
usenet_doc="$repo_dir/docs/Usenet-Piece-Backend-README.md"

if [[ ! -x "$daemon" ]]; then
    echo "Missing daemon binary: $daemon" >&2
    echo "Build it first with: cmake --build build -j2 --target transmission-daemon" >&2
    exit 1
fi

if [[ ! -f "$web_home/index.html" ]]; then
    echo "Missing built Web UI: $web_home/index.html" >&2
    echo "Build the Web UI first, then retry." >&2
    exit 1
fi

if [[ ! -f "$env_example" ]]; then
    echo "Missing env example: $env_example" >&2
    exit 1
fi

mkdir -p "$staging_dir/bin" "$staging_dir/web" "$staging_dir/docs" "$staging_dir/scripts" "$out_dir"

install -m 0755 "$daemon" "$staging_dir/bin/transmission-daemon"
cp -a "$web_home" "$staging_dir/web/public_html"
install -m 0644 "$env_example" "$staging_dir/.env.example"
install -m 0644 "$usenet_doc" "$staging_dir/docs/Usenet-Piece-Backend-README.md"

cat >"$staging_dir/run-usenet-daemon.sh" <<'EOF'
#!/usr/bin/env bash

set -euo pipefail

deploy_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

runtime_root="${NASHAWK_RUNTIME_ROOT:-$deploy_dir/runtime}"
config_dir="${NASHAWK_CONFIG_DIR:-$runtime_root/config}"
download_dir="${NASHAWK_DOWNLOAD_DIR:-$runtime_root/downloads}"
incomplete_dir="${NASHAWK_INCOMPLETE_DIR:-$runtime_root/incomplete}"
rpc_bind="${NASHAWK_RPC_BIND:-127.0.0.1}"
rpc_port="${NASHAWK_RPC_PORT:-19091}"
usenet_check_article_size="${NASHAWK_USENET_CHECK_ARTICLE_SIZE:-2097152}"
usenet_upload_concurrency="${NASHAWK_USENET_UPLOAD_CONCURRENCY:-40}"
usenet_eviction_min_age_minutes="${NASHAWK_USENET_EVICTION_MIN_AGE_MINUTES:-0}"
usenet_cache_size_mib="${NASHAWK_USENET_CACHE_SIZE_MIB:-0}"
usenet_evict_after_readback="${NASHAWK_USENET_EVICT_AFTER_READBACK:-1}"
usenet_discovery_enabled="${NASHAWK_USENET_DISCOVERY_ENABLED:-1}"
usenet_discovery_sample_size="${NASHAWK_USENET_DISCOVERY_SAMPLE_SIZE:-16}"
version_compat_enabled="${NASHAWK_VERSION_COMPAT_ENABLED:-1}"
log_level="${NASHAWK_LOG_LEVEL:-info}"
log_file="${NASHAWK_LOG_FILE:-$runtime_root/daemon.log}"
node_version="${NASHAWK_NODE_VERSION:-24}"

daemon="$deploy_dir/bin/transmission-daemon"
web_home="$deploy_dir/web/public_html"
env_file="${NASHAWK_ENV_FILE:-$deploy_dir/.env}"

if [[ ! -x "$daemon" ]]; then
    echo "Missing daemon binary: $daemon" >&2
    exit 1
fi

if [[ ! -f "$web_home/index.html" ]]; then
    echo "Missing Web UI: $web_home/index.html" >&2
    exit 1
fi

if [[ ! -f "$env_file" ]]; then
    echo "Missing .env file: $env_file" >&2
    echo "Create it from .env.example and add your Usenet credentials." >&2
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
    echo "Install Node 24 with nvm and run: npm install -g nyuu" >&2
    exit 1
fi

mkdir -p "$config_dir" "$download_dir" "$incomplete_dir" "$(dirname -- "$log_file")"

export TRANSMISSION_WEB_HOME="$web_home"

echo "Starting Nashawk Usenet daemon"
echo "  RPC: http://$rpc_bind:$rpc_port/transmission/web/"
echo "  Config: $config_dir"
echo "  Downloads: $download_dir"
echo "  Log: $log_file"
echo "  nyuu: $(command -v nyuu)"
echo "  Web UI: $TRANSMISSION_WEB_HOME"
echo "  Evict after readback: $usenet_evict_after_readback"
echo "Press Ctrl-C to stop."

discovery_args=()
if [[ "$usenet_discovery_enabled" == 0 || "$usenet_discovery_enabled" == false ]]; then
    discovery_args+=(--no-usenet-discovery)
else
    discovery_args+=(--usenet-discovery-enabled)
fi
discovery_args+=(--usenet-discovery-sample-size "$usenet_discovery_sample_size")

version_args=()
if [[ "$version_compat_enabled" == 0 || "$version_compat_enabled" == false ]]; then
    version_args+=(--no-version-compat)
fi

eviction_args=()
if [[ "$usenet_evict_after_readback" == 0 || "$usenet_evict_after_readback" == false ]]; then
    eviction_args+=(--no-usenet-evict-after-readback)
else
    eviction_args+=(--usenet-evict-after-readback)
fi

exec "$daemon" -f \
    -g "$config_dir" \
    -w "$download_dir" \
    --incomplete-dir "$incomplete_dir" \
    -r "$rpc_bind" \
    -p "$rpc_port" \
    -T \
    --usenet-enabled \
    --usenet-check-article-size "$usenet_check_article_size" \
    --usenet-upload-concurrency "$usenet_upload_concurrency" \
    --usenet-eviction-enabled \
    --usenet-eviction-min-age-minutes "$usenet_eviction_min_age_minutes" \
    --usenet-cache-size-mib "$usenet_cache_size_mib" \
    "${eviction_args[@]}" \
    "${discovery_args[@]}" \
    "${version_args[@]}" \
    --log-level="$log_level" \
    -e "$log_file"
EOF
chmod 0755 "$staging_dir/run-usenet-daemon.sh"

cat >"$staging_dir/README.deploy.md" <<EOF
# Nashawk VPS Deploy Package

Build commit: $version

This package contains:

- \`bin/transmission-daemon\`
- \`web/public_html/\`
- \`.env.example\`
- \`run-usenet-daemon.sh\`
- Usenet backend documentation under \`docs/\`

It intentionally does not include \`.env\`, torrent data, runtime config,
downloaded pieces, build directories, or Node modules.

## VPS setup

Install Node 24 and nyuu on the VPS:

\`\`\`bash
curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.3/install.sh | bash
source ~/.bashrc
nvm install 24
nvm use 24
nvm alias default 24
npm install -g nyuu
\`\`\`

Create the local secret file:

\`\`\`bash
cp .env.example .env
\$EDITOR .env
\`\`\`

Start the daemon:

\`\`\`bash
./run-usenet-daemon.sh
\`\`\`

By default the daemon exposes Transmission 4.1.2-compatible external version
identifiers for compatibility testing. To expose the real Nashawk development
version instead:

\`\`\`bash
NASHAWK_VERSION_COMPAT_ENABLED=0 ./run-usenet-daemon.sh
\`\`\`

The default Web UI URL is:

\`\`\`text
http://127.0.0.1:19091/transmission/web/
\`\`\`

For remote access, bind explicitly with care, for example:

\`\`\`bash
NASHAWK_RPC_BIND=0.0.0.0 ./run-usenet-daemon.sh
\`\`\`

Use a firewall, SSH tunnel, or reverse proxy authentication before exposing RPC
outside localhost.
EOF

if command -v ldd >/dev/null 2>&1; then
    ldd "$daemon" >"$staging_dir/ldd-transmission-daemon.txt" || true
fi

(
    cd "$staging_dir"
    find . -type f -print0 | sort -z | xargs -0 sha256sum >SHA256SUMS
)

tar -C "$staging_root" -czf "$archive" "$package_name"

echo "Created package: $archive"
echo "Package contents:"
tar -tzf "$archive" | sed -n '1,120p'
