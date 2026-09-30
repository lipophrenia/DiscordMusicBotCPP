#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: ./scripts/build-deb.sh [options]

Build a native Ubuntu .deb package.

Options:
  --install       Install the resulting package with apt.
  --service       Install the package and enable its systemd service.
  --clean         Remove the native build directory before building.
  --skip-deps     Do not install build dependencies with apt.
  -h, --help      Show this help.
EOF
}

install_package=false
enable_service=false
clean_build=false
install_dependencies=true

while (($#)); do
    case "$1" in
        --install) install_package=true ;;
        --service) install_package=true; enable_service=true ;;
        --clean) clean_build=true ;;
        --skip-deps) install_dependencies=false ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [[ ! -r /etc/os-release ]]; then
    echo "This script supports Ubuntu and Debian-family systems only." >&2
    exit 1
fi
. /etc/os-release
case "${ID:-}" in
    ubuntu|debian) ;;
    *) echo "Unsupported distribution: ${ID:-unknown}" >&2; exit 1 ;;
esac

if ((EUID == 0)); then
    sudo_cmd=()
elif command -v sudo >/dev/null 2>&1; then
    sudo_cmd=(sudo)
else
    echo "Run as root or install sudo." >&2
    exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
build_dir="$project_dir/build/native"
package_dir="$project_dir/dist"
tools_dir="$build_dir/tools"

if $clean_build; then
    rm -rf -- "$build_dir"
fi
mkdir -p -- "$build_dir" "$package_dir" "$tools_dir"

if $install_dependencies; then
    "${sudo_cmd[@]}" apt-get update
    "${sudo_cmd[@]}" apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake curl dpkg-dev fakeroot git ninja-build pkg-config unzip \
        libcurl4-openssl-dev libfmt-dev libopus-dev libsodium-dev libssl-dev \
        nlohmann-json3-dev zlib1g-dev
fi

case "$(dpkg --print-architecture)" in
    amd64)
        yt_dlp_asset=yt-dlp_linux
        deno_asset=deno-x86_64-unknown-linux-gnu.zip
        ;;
    arm64)
        yt_dlp_asset=yt-dlp_linux_aarch64
        deno_asset=deno-aarch64-unknown-linux-gnu.zip
        ;;
    *) echo "Only amd64 and arm64 packages are currently supported." >&2; exit 1 ;;
esac
yt_dlp_path="$tools_dir/yt-dlp"
checksums_path="$tools_dir/SHA2-256SUMS"
curl --fail --location --retry 3 \
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/$yt_dlp_asset" \
    --output "$yt_dlp_path"
curl --fail --location --retry 3 \
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS" \
    --output "$checksums_path"
expected_checksum="$(awk -v asset="$yt_dlp_asset" \
    '{ name=$2; sub(/^\*/, "", name); if (name == asset) { print $1; exit } }' \
    "$checksums_path")"
if [[ ! "$expected_checksum" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "The yt-dlp checksum was not found." >&2
    exit 1
fi
printf '%s  %s\n' "$expected_checksum" "$yt_dlp_path" | sha256sum --check --status
chmod 0755 "$yt_dlp_path"

deno_archive="$tools_dir/$deno_asset"
deno_checksum="$deno_archive.sha256sum"
deno_path="$tools_dir/deno"
curl --fail --location --retry 3 \
    "https://github.com/denoland/deno/releases/latest/download/$deno_asset" \
    --output "$deno_archive"
curl --fail --location --retry 3 \
    "https://github.com/denoland/deno/releases/latest/download/$deno_asset.sha256sum" \
    --output "$deno_checksum"
expected_checksum="$(awk 'NR == 1 { print $1 }' "$deno_checksum")"
if [[ ! "$expected_checksum" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "The Deno checksum was not found." >&2
    exit 1
fi
printf '%s  %s\n' "$expected_checksum" "$deno_archive" | sha256sum --check --status
unzip -oq "$deno_archive" deno -d "$tools_dir"
chmod 0755 "$deno_path"

cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMUSICBOT_FETCH_DPP=ON \
    -DMUSICBOT_BUNDLED_YT_DLP="$yt_dlp_path" \
    -DMUSICBOT_BUNDLED_DENO="$deno_path" \
    -DBUILD_TESTING=ON
build_jobs="${BUILD_JOBS:-$(nproc)}"
if [[ ! "$build_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "BUILD_JOBS must be a positive integer." >&2
    exit 2
fi
if ((build_jobs > 4)); then
    build_jobs=4
fi
cmake --build "$build_dir" --parallel "$build_jobs"
ctest --test-dir "$build_dir" --output-on-failure
cpack --config "$build_dir/CPackConfig.cmake" -B "$package_dir" -G DEB

package_path="$(find "$package_dir" -maxdepth 1 -type f -name 'discord-music-bot_*.deb' \
    -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
if [[ -z "$package_path" ]]; then
    echo "Package was not created." >&2
    exit 1
fi
package_contents="$(dpkg-deb --contents "$package_path")"
for expected_path in \
    ./usr/bin/discord-music-bot \
    ./usr/lib/discord-music-bot/yt-dlp \
    ./usr/lib/discord-music-bot/deno \
    ./etc/discord-music-bot/config.env \
    ./lib/systemd/system/discord-music-bot.service; do
    if [[ "$package_contents" != *" $expected_path"* ]]; then
        echo "Package is missing expected path: $expected_path" >&2
        exit 1
    fi
done
echo "Package created: $package_path"

if $install_package; then
    "${sudo_cmd[@]}" apt-get install --reinstall -y "$package_path"
    echo "Package installed. Configuration: /etc/discord-music-bot/config.env"
fi

if $enable_service; then
    "${sudo_cmd[@]}" systemctl daemon-reload
    "${sudo_cmd[@]}" systemctl enable discord-music-bot.service
    if ! "${sudo_cmd[@]}" grep -q '^DISCORD_TOKEN=' /etc/discord-music-bot/config.env || \
       "${sudo_cmd[@]}" grep -Eq '^DISCORD_TOKEN=(replace_me)?$' /etc/discord-music-bot/config.env; then
        echo "Service enabled but not started: set DISCORD_TOKEN in" >&2
        echo "  /etc/discord-music-bot/config.env" >&2
        echo "Then run: sudo systemctl start discord-music-bot" >&2
    else
        "${sudo_cmd[@]}" systemctl restart discord-music-bot.service
        "${sudo_cmd[@]}" systemctl --no-pager --full status discord-music-bot.service
    fi
fi
