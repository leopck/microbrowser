#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${project_dir}/../.." && pwd)
ncs_root=${NCS_ROOT:-"${repo_dir}/.tools/nrfconnect/ncs"}
build_root="${repo_dir}/.tools/nrfconnect/build"
image=${NCS_BSIM_IMAGE:-microbrowser/nrfconnect:v3.4.2-bsim}

if ! command -v docker >/dev/null 2>&1; then
    printf 'Docker or OrbStack is required for the x86 Bsim build.\n' >&2
    exit 2
fi
mkdir -p "${build_root}"

# Build only. Starting the PHY and the two independent devices is a separate step.
docker run --rm --platform linux/amd64 \
    --tmpfs /ncs/zephyr/.cache:rw,exec,size=64m \
    --volume "${ncs_root}:/ncs:ro" \
    --volume "${repo_dir}:/workspace:ro" \
    --volume "${build_root}:/build" \
    --entrypoint /bin/bash "${image}" -lc '
set -euo pipefail
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends g++-multilib
export ZEPHYR_TOOLCHAIN_VARIANT=host
export ZEPHYR_BASE=/ncs/zephyr
export TOOLCHAIN_ROOT=/ncs/zephyr
export BSIM_OUT_PATH=/ncs/tools/bsim
export BSIM_COMPONENTS_PATH=/ncs/tools/bsim/components
cmake -S /ncs/zephyr/share/sysbuild -B /build/nrf5340bsim-ble-peer -G Ninja \
    -DAPP_DIR=/workspace/firmware/nrf5340_ble_peer \
    -DBOARD=nrf5340bsim/nrf5340/cpuapp \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    -DGIT_EXECUTABLE=/usr/bin/git
cmake --build /build/nrf5340bsim-ble-peer -j 4
'

printf 'Bsim application executable: %s/nrf5340bsim-ble-peer/nrf5340_ble_peer/zephyr/zephyr.exe\n' "${build_root}"
printf 'Application ELF:             %s/nrf5340bsim-ble-peer/nrf5340_ble_peer/zephyr/zephyr.elf\n' "${build_root}"
printf 'Network-core ELF:            %s/nrf5340bsim-ble-peer/hci_ipc/zephyr/zephyr.elf\n' "${build_root}"
