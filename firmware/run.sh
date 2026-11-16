#!/usr/bin/env bash
set -euo pipefail

firmware_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${firmware_dir}/.." && pwd)
if [[ -n "${IDF_PATH:-}" ]]; then
    idf_dir=${IDF_PATH}
else
    idf_dir="${repo_dir}/../esp-idf"
fi

if [[ ! -f "${idf_dir}/export.sh" ]]; then
    printf 'ESP-IDF export.sh not found. Set IDF_PATH before running.\n' >&2
    exit 2
fi

graphics=0
build_only=0
for argument in "$@"; do
    case "${argument}" in
        --graphics) graphics=1 ;;
        --build-only) build_only=1 ;;
        *)
            printf 'Usage: %s [--graphics] [--build-only]\n' "$0" >&2
            exit 2
            ;;
    esac
done

mkdir -p "${repo_dir}/build"

python3 "${repo_dir}/tools/mbc" build \
    "${repo_dir}/examples/dashboard.mb" -o "${repo_dir}/build/dashboard.mbp"

set +u
# shellcheck disable=SC1091
. "${idf_dir}/export.sh"
set -u
idf.py -C "${firmware_dir}" set-target esp32s3 build
if (( build_only )); then
    exit 0
fi

qemu_options=("--qemu-extra-args=-m 8M")
if (( graphics )); then
    qemu_options+=(--graphics)
else
    qemu_options=("--qemu-extra-args=-m 8M -display sdl")
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
fi
idf.py -C "${firmware_dir}" qemu "${qemu_options[@]}" monitor
