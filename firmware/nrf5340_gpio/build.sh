#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${project_dir}/../.." && pwd)
ncs_root=${NCS_ROOT:-"${repo_dir}/.tools/nrfconnect/ncs"}
toolchain_root=${NCS_TOOLCHAIN:-"${repo_dir}/.tools/nrfconnect/toolchains/fa36dcc52a"}
python_path=${NCS_PYTHON:-"${repo_dir}/.tools/nrfconnect/bootstrap-venv/bin/python"}
build_dir=${NCS_BUILD_DIR:-"${repo_dir}/.tools/nrfconnect/build/nrf5340dk-gpio"}
git_executable=${GIT_EXECUTABLE:-/usr/bin/git}

if [[ ! -x "${toolchain_root}/bin/cmake" || ! -x "${python_path}" ]]; then
    printf 'Nordic SDK tools not found. Set NCS_TOOLCHAIN and NCS_PYTHON.\n' >&2
    exit 2
fi
if ! "${python_path}" -c 'import jsonschema, yaml, elftools'; then
    printf 'Install jsonschema==4.26.0 and pyelftools==0.32 in NCS_PYTHON.\n' >&2
    exit 2
fi

export ZEPHYR_TOOLCHAIN_VARIANT=zephyr/gnu
export ZEPHYR_SDK_INSTALL_DIR="${toolchain_root}/opt/zephyr-sdk"
export ZEPHYR_BASE="${ncs_root}/zephyr"
export PATH="${toolchain_root}/bin:${toolchain_root}/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:${PATH}"
unset TOOLCHAIN_ROOT
mkdir -p "${repo_dir}/build" "$(dirname -- "${build_dir}")"

"${toolchain_root}/bin/cmake" -S "${project_dir}" -B "${build_dir}" -G Ninja \
    -DBOARD=nrf5340dk/nrf5340/cpuapp \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPython3_EXECUTABLE="${python_path}" \
    -DGIT_EXECUTABLE="${git_executable}"
"${toolchain_root}/bin/cmake" --build "${build_dir}" -j 4
