#!/usr/bin/env bash
set -euo pipefail

gatt_mode=0
if [[ "${1:-}" == "--gatt" ]]; then
    gatt_mode=1
    shift
fi

if [[ $# -lt 1 ]]; then
    printf 'Usage: %s [--gatt] nrf7002dk/nrf5340/cpuapp [BUILD_DIR] [CMAKE_ARGS...]\n' "$0" >&2
    printf 'Use bsim.sh for nrf5340bsim/nrf5340/cpuapp.\n' >&2
    printf '%s\n' '--gatt builds the application and the generic Zephyr hci_ipc network-core controller.' >&2
    exit 2
fi
board=$1
shift
if [[ "${board}" != "nrf7002dk/nrf5340/cpuapp" ]]; then
    printf 'build.sh targets the physical nRF7002 DK; use bsim.sh for simulation.\n' >&2
    exit 2
fi

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${project_dir}/../.." && pwd)
ncs_root=${NCS_ROOT:-"${repo_dir}/.tools/nrfconnect/ncs"}
toolchain_root=${NCS_TOOLCHAIN:-"${repo_dir}/.tools/nrfconnect/toolchains/fa36dcc52a"}
python_path=${NCS_PYTHON:-"${repo_dir}/.tools/nrfconnect/bootstrap-venv/bin/python"}
if [[ $# -gt 0 && $1 != -* ]]; then
    build_dir=$1
    shift
else
    build_suffix=
    if [[ "${gatt_mode}" == 1 ]]; then
        build_suffix=-gatt
    fi
    build_dir=${NCS_BUILD_DIR:-"${repo_dir}/.tools/nrfconnect/build/nrf5340-app-${board//\//-}${build_suffix}"}
fi
git_executable=${GIT_EXECUTABLE:-/usr/bin/git}

if [[ ! -x "${toolchain_root}/bin/cmake" || ! -x "${python_path}" ]]; then
    printf 'Set NCS_TOOLCHAIN and NCS_PYTHON to the NCS 3.4.2 toolchain and Python.\n' >&2
    exit 2
fi
if ! "${python_path}" -c 'import jsonschema, yaml, elftools'; then
    printf 'NCS_PYTHON needs jsonschema==4.26.0, PyYAML and pyelftools==0.32.\n' >&2
    exit 2
fi
mkdir -p "${repo_dir}/build" "$(dirname -- "${build_dir}")"

export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
export ZEPHYR_SDK_INSTALL_DIR="${toolchain_root}/opt/zephyr-sdk"
export ZEPHYR_BASE="${ncs_root}/zephyr"
export PATH="${toolchain_root}/bin:${toolchain_root}/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:${PATH}"
unset TOOLCHAIN_ROOT
if [[ "${gatt_mode}" == 1 ]]; then
    # Nested sysbuild images run repository metadata queries. The relocated
    # Nordic Git has a broken install-time PCRE path on this host; use the
    # already-selected system Git while retaining explicit compiler paths.
    export PATH="/usr/bin:${PATH}"
fi

source_dir=${project_dir}
configure_args=(
    -DBOARD="${board}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DPython3_EXECUTABLE="${python_path}"
    -DGIT_EXECUTABLE="${git_executable}"
)
if [[ -n "${MICROBROWSER_PACKAGE:-}" ]]; then
    configure_args+=(
        -DMICROBROWSER_PACKAGE="${MICROBROWSER_PACKAGE}"
    )
fi
if [[ "${gatt_mode}" == 1 ]]; then
    source_dir=${ncs_root}/zephyr/share/sysbuild
    configure_args+=(
        -DAPP_DIR="${project_dir}"
        -DEXTRA_CONF_FILE="${project_dir}/gatt.conf"
        -DCMAKE_PROGRAM_PATH=/usr/bin
    )
fi

"${toolchain_root}/bin/cmake" -S "${source_dir}" -B "${build_dir}" -G Ninja \
    "${configure_args[@]}" "$@"
"${toolchain_root}/bin/cmake" --build "${build_dir}" -j 4

if [[ "${gatt_mode}" == 1 ]]; then
    printf 'Application images: %s/nrf5340_app/zephyr/zephyr.{elf,hex}\n' "${build_dir}"
    printf 'Controller images:  %s/hci_ipc/zephyr/zephyr.{elf,hex}\n' "${build_dir}"
fi
