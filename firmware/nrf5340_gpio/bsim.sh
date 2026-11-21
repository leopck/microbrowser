#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${project_dir}/../.." && pwd)
ncs_root=${NCS_ROOT:-"${repo_dir}/.tools/nrfconnect/ncs"}
manifest=${NCS_FROZEN_MANIFEST:-"${repo_dir}/.tools/nrfconnect/west-v3.4.2.freeze.yml"}
x_image=${NCS_BSIM_IMAGE:-microbrowser/nrfconnect:v3.4.2-bsim}

if ! command -v docker >/dev/null 2>&1; then
    printf 'Docker or OrbStack is required for the x86 Bsim execution environment.\n' >&2
    exit 2
fi
mkdir -p "${repo_dir}/.tools/nrfconnect/build"

docker run --rm --platform linux/amd64 \
    --tmpfs /ncs/zephyr/.cache:rw,exec,size=64m \
    --volume "${ncs_root}:/ncs:ro" \
    --volume "${manifest}:/west-v3.4.2.freeze.yml:ro" \
    --volume "${repo_dir}:/workspace:ro" \
    --volume "${repo_dir}/.tools/nrfconnect/build:/build" \
    --entrypoint /bin/bash "${x_image}" -lc '
set -euo pipefail
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends g++-multilib
export ZEPHYR_TOOLCHAIN_VARIANT=host
export ZEPHYR_BASE=/ncs/zephyr
export TOOLCHAIN_ROOT=/ncs/zephyr
export BSIM_OUT_PATH=/ncs/tools/bsim
export BSIM_COMPONENTS_PATH=/ncs/tools/bsim/components
cmake -S /workspace/firmware/nrf5340_gpio -B /build/nrf5340bsim-gpio -G Ninja \
    -DBOARD=nrf5340bsim/nrf5340/cpuapp \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    -DGIT_EXECUTABLE=/usr/bin/git
cmake --build /build/nrf5340bsim-gpio -j 4
cd /build/nrf5340bsim-gpio/zephyr
timeout 60s ./zephyr.exe -nosim \
    -gpio_in_file=/workspace/firmware/nrf5340_gpio/bsim_input.csv
'
