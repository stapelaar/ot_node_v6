#!/usr/bin/env bash
set -euo pipefail

# =============================================================
#  build-xiao.sh — NCS 5.0.0 Thread sensor node builder
#  - Builds the Xiao nRF54L15 cpuapp target
#  - Per-node configuration via nodes/NDxx.conf
# =============================================================

BOARD="xiao_nrf54l15/nrf54l15/cpuapp"
NODE=""
EXTRA_OVERLAYS=""
BUILD_TYPE="pristine"

APP_DIR="$(cd "$(dirname "$0")" && pwd -P)"

ALWAYS_54L15="${APP_DIR}/overlays/overlay-54l15.conf"
ALWAYS_OT_BASIS="${APP_DIR}/overlays/overlay-OT-network-basis.conf"
BOARD_DTS="${APP_DIR}/boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay"

ROLE_FTD="${APP_DIR}/overlays/overlay-OT-network-ftd.conf"
ROLE_MTD="${APP_DIR}/overlays/overlay-OT-network-mtd.conf"
ROLE_MTD_SED="${APP_DIR}/overlays/overlay-OT-network-mtd-sed.conf"

SENSOR_SHT41="${APP_DIR}/overlays/overlay-sensors-sht41.overlay"
SENSOR_SCD41="${APP_DIR}/overlays/overlay-sensors-scd41.overlay"
SENSOR_SEN50="${APP_DIR}/overlays/overlay-sensors-sen50.overlay"
SENSOR_BMP388="${APP_DIR}/overlays/overlay-sensors-bmp388.overlay"
SENSOR_NONE="${APP_DIR}/overlays/overlay-sensors-none.overlay"

RED="\033[1;31m"
GRN="\033[1;32m"
CYN="\033[1;36m"
RST="\033[0m"

usage() {
    cat <<USAGE

Usage:
  ./build-xiao.sh --node NDxx [--overlays <list>] [--no-pristine]

Options:
  --node NDxx        Node config (from nodes/NDxx.conf)
  --overlays list    Extra overlay(s), semicolon separated
  --no-pristine      Incremental build
  -h, --help         Show this help

Notes:
  * Role overlay auto-selected: FTD / MTD / MTD-SED
  * Sensor overlay auto-selected: sht41 / scd41 / sen50 / bmp388 / none
  * Node config ALWAYS wins last.
  * Battery deployment: set CONFIG_SERIAL=n etc. in node conf directly.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node)       NODE="${2:-}"; shift 2 ;;
        --overlays)   EXTRA_OVERLAYS="${2:-}"; shift 2 ;;
        --no-pristine) BUILD_TYPE="incremental"; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo -e "${RED}Unknown option:${RST} $1"; usage; exit 2 ;;
    esac
done

if [[ -z "$NODE" ]]; then
    echo -e "${RED}ERROR:${RST} --node NDxx is verplicht."
    exit 1
fi

NODE_CONF="${APP_DIR}/nodes/${NODE}.conf"
if [[ ! -f "$NODE_CONF" ]]; then
    echo -e "${RED}ERROR:${RST} Node config ontbreekt: ${NODE_CONF}"
    exit 1
fi

# Role detection
want_mtd=0; want_ftd=0; want_sed=0
grep -Eq '^\s*CONFIG_OPENTHREAD_MTD\s*=\s*y'     "$NODE_CONF" && want_mtd=1
grep -Eq '^\s*CONFIG_OPENTHREAD_FTD\s*=\s*y'     "$NODE_CONF" && want_ftd=1
grep -Eq '^\s*CONFIG_OPENTHREAD_MTD_SED\s*=\s*y' "$NODE_CONF" && want_sed=1

if [[ $want_mtd -eq 1 && $want_ftd -eq 1 ]]; then
    echo -e "${RED}ERROR:${RST} Zowel MTD als FTD aangezet in ${NODE_CONF}"
    exit 1
fi

if   [[ $want_mtd -eq 1 && $want_sed -eq 1 ]]; then ROLE_OVERLAY="$ROLE_MTD_SED"
elif [[ $want_mtd -eq 1 ]];                         then ROLE_OVERLAY="$ROLE_MTD"
elif [[ $want_ftd -eq 1 ]];                         then ROLE_OVERLAY="$ROLE_FTD"
else                                                      ROLE_OVERLAY="$ROLE_MTD"
fi

# Sensor overlay detection
use_sht41=0; use_scd41=0; use_sen50=0; use_bmp388=0
grep -Eq '^\s*CONFIG_APP_USE_SHT41_SENSOR\s*=\s*y'  "$NODE_CONF" && use_sht41=1
grep -Eq '^\s*CONFIG_APP_USE_SCD41_SENSOR\s*=\s*y'  "$NODE_CONF" && use_scd41=1
grep -Eq '^\s*CONFIG_APP_USE_SEN50_SENSOR\s*=\s*y'  "$NODE_CONF" && use_sen50=1
grep -Eq '^\s*CONFIG_APP_USE_BMP388_SENSOR\s*=\s*y' "$NODE_CONF" && use_bmp388=1

if   [[ $use_sht41  -eq 1 ]]; then SENSOR_OVERLAY="$SENSOR_SHT41"
elif [[ $use_scd41  -eq 1 ]]; then SENSOR_OVERLAY="$SENSOR_SCD41"
elif [[ $use_sen50  -eq 1 ]]; then SENSOR_OVERLAY="$SENSOR_SEN50"
elif [[ $use_bmp388 -eq 1 ]]; then SENSOR_OVERLAY="$SENSOR_BMP388"
else                                SENSOR_OVERLAY="$SENSOR_NONE"
fi

# Kconfig overlay list
OVERLAY_CONFIG="${ALWAYS_54L15};${ALWAYS_OT_BASIS}"
if [[ -n "$EXTRA_OVERLAYS" ]]; then
    OVERLAY_CONFIG="${OVERLAY_CONFIG};${EXTRA_OVERLAYS}"
fi
OVERLAY_CONFIG="${OVERLAY_CONFIG};${ROLE_OVERLAY};${NODE_CONF}"

DTC_OVERLAYS="${BOARD_DTS};${SENSOR_OVERLAY}"
BUILD_DIR="${APP_DIR}/build/${NODE}"
mkdir -p "$BUILD_DIR"

echo -e "${CYN}---------------------------------------${RST}"
echo -e "${GRN}Building node       :${RST} $NODE"
echo -e "${GRN}Role overlay        :${RST} $(basename "$ROLE_OVERLAY")"
echo -e "${GRN}Sensor overlay      :${RST} $(basename "$SENSOR_OVERLAY")"
echo -e "${GRN}Board               :${RST} $BOARD"
echo -e "${GRN}Overlays            :${RST} $OVERLAY_CONFIG"
echo -e "${GRN}DTC overlays        :${RST} $DTC_OVERLAYS"
echo -e "${GRN}Build dir           :${RST} $BUILD_DIR"
echo -e "${CYN}---------------------------------------${RST}"

pushd "$APP_DIR" >/dev/null
APP_DIR="${APP_DIR:-${PWD}/app}"
export APP_DIR

if [[ "$BUILD_TYPE" == "pristine" ]]; then
    west build -p always \
        -b "${BOARD}" \
        -d "${BUILD_DIR}" \
        -- -DOVERLAY_CONFIG="${OVERLAY_CONFIG}" \
           -DDTC_OVERLAY_FILE="${DTC_OVERLAYS}"
else
    west build \
        -b "${BOARD}" \
        -d "${BUILD_DIR}" \
        -- -DOVERLAY_CONFIG="${OVERLAY_CONFIG}" \
           -DDTC_OVERLAY_FILE="${DTC_OVERLAYS}"
fi

popd >/dev/null
echo -e "${GRN}✔ Build voltooid voor node ${NODE}.${RST}"