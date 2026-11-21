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

package=${MICROBROWSER_PACKAGE:-"${repo_dir}/build/dashboard.mbp"}
package_explicit=0
owner_profile=""
gpio_conformance=0
graphics=0
build_only=0
smoke=0
while (( $# > 0 )); do
    case "$1" in
        --graphics) graphics=1; shift ;;
        --build-only) build_only=1; shift ;;
        --gpio-conformance) gpio_conformance=1; shift ;;
        --smoke) smoke=1; shift ;;
        --owner-profile)
            if (( $# < 2 )); then
                printf '%s requires an sdkconfig fragment path\n' "$1" >&2
                exit 2
            fi
            owner_profile=$2
            shift 2
            ;;
        --package)
            if (( $# < 2 )); then
                printf '%s requires a compiled .mbp path\n' "$1" >&2
                exit 2
            fi
            package=$2
            package_explicit=1
            shift 2
            ;;
        *)
            printf 'Usage: %s [--graphics] [--build-only] [--smoke] [--gpio-conformance | --owner-profile FRAGMENT] [--package PATH.mbp]\n' "$0" >&2
            exit 2
            ;;
    esac
done
if (( gpio_conformance && package_explicit )); then
    printf '%s\n' '--gpio-conformance selects its matching package; do not also use --package.' >&2
    exit 2
fi
if (( gpio_conformance )) && [[ -n "${owner_profile}" ]]; then
    printf '%s\n' '--gpio-conformance and --owner-profile are mutually exclusive.' >&2
    exit 2
fi
if (( gpio_conformance )); then
    package="${repo_dir}/build/gpio_conformance.mbp"
fi

mkdir -p "${repo_dir}/build"
if [[ "${package}" != /* ]]; then
    package="${repo_dir}/${package}"
fi

default_package="${repo_dir}/build/dashboard.mbp"
if (( ! package_explicit )) && [[ "${package}" == "${default_package}" ]]; then
    python3 "${repo_dir}/tools/mbc" build \
        "${repo_dir}/examples/dashboard.mb" -o "${default_package}"
fi
if (( gpio_conformance )); then
    python3 "${repo_dir}/tools/mbc" build \
        "${repo_dir}/examples/gpio_conformance.mb" -o "${package}"
fi
if [[ ! -f "${package}" ]]; then
    printf 'Missing compiled application package: %s\n' "${package}" >&2
    exit 2
fi
printf 'Embedding application: %s\n' "${package}"
python3 -c 'import hashlib, pathlib, sys; print(f"SHA-256: {hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()}")' "${package}"

idf_options=(-C "${firmware_dir}" -D MICROBROWSER_PACKAGE="${package}")
firmware_build_dir="${firmware_dir}/build"
if (( gpio_conformance )) || [[ -n "${owner_profile}" ]]; then
    if (( gpio_conformance )); then
        owner_profile="${firmware_dir}/sdkconfig.gpio_conformance"
        profile_name=gpio-conformance
        printf '%s\n' 'WARNING: user_button=GPIO4 is a development example, not a detected board connection.' >&2
        printf '%s\n' 'WARNING: QEMU supplies no physical button or GPIO edge/IRQ stimulus; this is not an IRQ edge test.' >&2
    else
        profile_name=owner-profile
        printf '%s\n' 'WARNING: native SPI/I2C/I2S transactions require attached hardware; do not infer peripheral support from this build.' >&2
    fi
    if [[ "${owner_profile}" != /* ]]; then
        owner_profile="${repo_dir}/${owner_profile}"
    fi
    if [[ ! -f "${owner_profile}" ]]; then
        printf 'Missing owner sdkconfig fragment: %s\n' "${owner_profile}" >&2
        exit 2
    fi
    profile_build_dir="${repo_dir}/build/firmware-${profile_name}"
    firmware_build_dir="${profile_build_dir}"
    profile_sdkconfig="${repo_dir}/build/firmware-${profile_name}.sdkconfig"
    # Regenerate this profile-specific file from the two explicit fragments;
    # defaults in earlier/default sdkconfig files cannot keep endpoints enabled.
    rm -f "${profile_sdkconfig}" "${profile_sdkconfig}.old"
    idf_options+=(
        -B "${profile_build_dir}"
        -D SDKCONFIG="${profile_sdkconfig}"
        -D "SDKCONFIG_DEFAULTS=${firmware_dir}/sdkconfig.defaults;${owner_profile}"
    )
fi

set +u
# shellcheck disable=SC1091
. "${idf_dir}/export.sh"
set -u
idf.py "${idf_options[@]}" set-target esp32s3 build
if (( build_only )); then
    exit 0
fi
if (( smoke )); then
    scenario=dashboard
    if (( gpio_conformance )); then scenario=gpio-conformance; fi
    smoke_args=(--firmware-build "${firmware_build_dir}" --scenario "${scenario}")
    if (( gpio_conformance )); then
        smoke_args+=(--screenshot "${repo_dir}/build/qemu-gpio-conformance.ppm")
    fi
    if (( ! graphics )); then smoke_args+=(--headless); fi
    exec python3 "${repo_dir}/tools/qemu_smoke.py" "${smoke_args[@]}"
fi

qemu_options=("--qemu-extra-args=-m 8M")
if (( graphics )); then
    qemu_options+=(--graphics)
else
    qemu_options=("--qemu-extra-args=-m 8M -display sdl")
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
fi
idf.py "${idf_options[@]}" qemu "${qemu_options[@]}" monitor
