#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${project_dir}/../.." && pwd)
ncs_root=${NCS_ROOT:-"${repo_dir}/.tools/nrfconnect/ncs"}
toolchain_root=${NCS_TOOLCHAIN:-"${repo_dir}/.tools/nrfconnect/toolchains/fa36dcc52a"}
python_path=${NCS_PYTHON:-"${repo_dir}/.tools/nrfconnect/bootstrap-venv/bin/python"}
build_dir=${NCS_BUILD_DIR:-"${repo_dir}/.tools/nrfconnect/build/nrf7002dk-ble-peer"}
git_executable=${GIT_EXECUTABLE:-/usr/bin/git}

if [[ ! -x "${toolchain_root}/bin/cmake" || ! -x "${python_path}" ]]; then
    printf 'Set NCS_TOOLCHAIN and NCS_PYTHON to the NCS 3.4.2 toolchain and Python.\n' >&2
    exit 2
fi
if ! "${python_path}" -c 'import jsonschema, yaml, elftools'; then
    printf 'NCS_PYTHON needs jsonschema==4.26.0, PyYAML and pyelftools==0.32.\n' >&2
    exit 2
fi
mkdir -p "$(dirname -- "${build_dir}")"

export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
export ZEPHYR_SDK_INSTALL_DIR="${toolchain_root}/opt/zephyr-sdk"
export ZEPHYR_BASE="${ncs_root}/zephyr"
export PATH="${toolchain_root}/bin:${toolchain_root}/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:${PATH}"
unset TOOLCHAIN_ROOT

# Build only. Flashing and physical interoperability testing are separate steps.
"${toolchain_root}/bin/cmake" -S "${ncs_root}/zephyr/share/sysbuild" \
    -B "${build_dir}" -G Ninja \
    -DAPP_DIR="${project_dir}" \
    -DBOARD=nrf7002dk/nrf5340/cpuapp \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPython3_EXECUTABLE="${python_path}" \
    -DGIT_EXECUTABLE="${git_executable}"
"${toolchain_root}/bin/cmake" --build "${build_dir}" -j 4

printf 'Application images: %s/nrf5340_ble_peer/zephyr/zephyr.{elf,hex}\n' "${build_dir}"
printf 'Network-core images: %s/hci_ipc/zephyr/zephyr.{elf,hex}\n' "${build_dir}"
