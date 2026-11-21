#!/usr/bin/env bash
set -euo pipefail

gatt_mode=0
if [[ "${1:-}" == "--gatt" ]]; then
    gatt_mode=1
    shift
fi

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${project_dir}/../.." && pwd)
ncs_root=${NCS_ROOT:-"${repo_dir}/.tools/nrfconnect/ncs"}
manifest=${NCS_FROZEN_MANIFEST:-"${repo_dir}/.tools/nrfconnect/west-v3.4.2.freeze.yml"}
image_file=${MICROBROWSER_PACKAGE:-"${repo_dir}/build/gpio_conformance.mbp"}
x_image=${NCS_BSIM_IMAGE:-microbrowser/nrfconnect:v3.4.2-bsim}
if [[ ! -f "${image_file}" ]]; then
    printf 'Build the package first; MICROBROWSER_PACKAGE currently names missing file %s\n' "${image_file}" >&2
    exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
    printf 'Docker or OrbStack is required for the x86 Bsim execution environment.\n' >&2
    exit 2
fi
mkdir -p "${repo_dir}/.tools/nrfconnect/build"
package_volume=()
if [[ "${image_file}" == "${repo_dir}/"* ]]; then
    package_container="/workspace/${image_file#"${repo_dir}"/}"
else
    package_container=/package/input.mbp
    package_volume=(--volume "${image_file}:/package/input.mbp:ro")
fi

# Bsim is an x86 host build, unlike the physical ARM target build.
docker run --rm --platform linux/amd64 \
    --tmpfs /ncs/zephyr/.cache:rw,exec,size=64m \
    --volume "${ncs_root}:/ncs:ro" \
    --volume "${manifest}:/west-v3.4.2.freeze.yml:ro" \
    --volume "${repo_dir}:/workspace:ro" \
    --volume "${repo_dir}/.tools/nrfconnect/build:/build" \
    --volume "${repo_dir}/.tools/nrfconnect/build:/build" \
    ${package_volume[@]+"${package_volume[@]}"} \
    --env MICROBROWSER_PACKAGE="${package_container}" \
    --env MICROBROWSER_GATT="${gatt_mode}" \
    --entrypoint /bin/bash "${x_image}" -lc '
set -euo pipefail
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends g++-multilib
export ZEPHYR_TOOLCHAIN_VARIANT=host
export ZEPHYR_BASE=/ncs/zephyr
export TOOLCHAIN_ROOT=/ncs/zephyr
export BSIM_OUT_PATH=/ncs/tools/bsim
export BSIM_COMPONENTS_PATH=/ncs/tools/bsim/components
source_dir=/workspace/firmware/nrf5340_app
build_dir=/build/nrf5340bsim-browser
configure_args=(
    -DBOARD=nrf5340bsim/nrf5340/cpuapp
    -DMICROBROWSER_PACKAGE="${MICROBROWSER_PACKAGE}"
    -DPython3_EXECUTABLE="$(command -v python3)"
    -DGIT_EXECUTABLE=/usr/bin/git
)
if [[ "${MICROBROWSER_GATT}" == 1 ]]; then
    source_dir=/ncs/zephyr/share/sysbuild
    build_dir=/build/nrf5340bsim-gatt
    configure_args+=(
        -DAPP_DIR=/workspace/firmware/nrf5340_app
        -DEXTRA_CONF_FILE=/workspace/firmware/nrf5340_app/gatt.conf
    )
fi
cmake -S "${source_dir}" -B "${build_dir}" -G Ninja "${configure_args[@]}"
cmake --build "${build_dir}" -j 4
if [[ "${MICROBROWSER_GATT}" == 0 ]]; then
    cd /build/nrf5340bsim-browser/zephyr
    timeout 60s ./zephyr.exe -nosim \
        -gpio_in_file=/workspace/firmware/nrf5340_app/bsim_input.csv
fi
'

if [[ "${gatt_mode}" == 1 ]]; then
    gatt_build_dir=${repo_dir}/.tools/nrfconnect/build/nrf5340bsim-gatt
    printf 'Application image:   %s/nrf5340_app/zephyr/zephyr.elf\n' "${gatt_build_dir}"
    printf 'Controller image:    %s/hci_ipc/zephyr/zephyr.elf\n' "${gatt_build_dir}"
    printf 'Combined Bsim model: %s/zephyr/zephyr.exe\n' "${gatt_build_dir}"
    printf '%s\n' 'Build only: run the combined model with a second radio device and the Bsim 2.4 GHz PHY separately.'
fi
