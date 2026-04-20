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
BUILD_TYPE="pristine"   # or "incremental"

APP_DIR="$(cd "$(dirname "$0")" && pwd -P)"

# Always-on overlays
ALWAYS_54L15="${APP_DIR}/overlays/overlay-54l15.conf"
ALWAYS_OT_BASIS="${APP_DIR}/overlays/overlay-OT-network-basis.conf"

# Role overlays
ROLE_FTD="${APP_DIR}/overlays/overlay-OT-network-ftd.conf"
ROLE_MTD="${APP_DIR}/overlays/overlay-OT-network-mtd.conf"

# Colors
RED="\033[1;31m"
GRN="\033[1;32m"
YLW="\033[1;33m"
CYN="\033[1;36m"
RST="\033[0m"

usage() {
    cat <<EOF

Usage:
  ./build-xiao.sh --node NDxx [--overlays <list>] [--no-pristine]

Options:
  --node NDxx        Node config (from nodes/NDxx.conf)
  --overlays list    Extra overlay(s), semicolon separated
  --no-pristine      Incremental build (don't wipe build dir)
  -h, --help         Show this help

Notes:
  * Per-node config in: nodes/NDxx.conf
  * Always-enabled overlays:
        overlay-54l15.conf
        overlay-OT-network-basis.conf
  * Node config ALWAYS wins last.
EOF
}

# =============================================================
# Parse args
# =============================================================

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node)
            NODE="${2:-}"
            shift 2
            ;;
        --overlays)
            EXTRA_OVERLAYS="${2:-}"
            shift 2
            ;;
        --no-pristine)
            BUILD_TYPE="incremental"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option:${RST} $1"
            usage
            exit 2
            ;;
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

# =============================================================
# Determine Thread role (MTD/FTD)
# =============================================================

want_mtd=0
want_ftd=0

grep -Eq '^\s*CONFIG_OPENTHREAD_MTD\s*=\s*y' "$NODE_CONF"  && want_mtd=1
grep -Eq '^\s*CONFIG_OPENTHREAD_FTD\s*=\s*y' "$NODE_CONF"  && want_ftd=1

if [[ $want_mtd -eq 1 && $want_ftd -eq 1 ]]; then
    echo -e "${RED}ERROR:${RST} Zowel MTD als FTD aangezet in ${NODE_CONF}"
    exit 1
fi

if [[ $want_mtd -eq 1 ]]; then
    ROLE_OVERLAY="$ROLE_MTD"
elif [[ $want_ftd -eq 1 ]]; then
    ROLE_OVERLAY="$ROLE_FTD"
else
    ROLE_OVERLAY="$ROLE_MTD"
fi

# =============================================================
# Build overlay list
# =============================================================

OVERLAY_CONFIG="${ALWAYS_54L15};${ALWAYS_OT_BASIS}"

if [[ -n "$EXTRA_OVERLAYS" ]]; then
    OVERLAY_CONFIG="${OVERLAY_CONFIG};${EXTRA_OVERLAYS}"
fi

OVERLAY_CONFIG="${OVERLAY_CONFIG};${ROLE_OVERLAY};${NODE_CONF}"

# =============================================================
# Build directory
# =============================================================

BUILD_DIR="${APP_DIR}/build/${NODE}"
mkdir -p "$BUILD_DIR"

echo -e "${CYN}---------------------------------------${RST}"
echo -e "${GRN}Building node       :${RST} $NODE"
echo -e "${GRN}Role overlay        :${RST} $(basename "$ROLE_OVERLAY")"
echo -e "${GRN}Board               :${RST} $BOARD"
echo -e "${GRN}Overlays            :${RST} $OVERLAY_CONFIG"
echo -e "${GRN}Build dir           :${RST} $BUILD_DIR"
echo -e "${CYN}---------------------------------------${RST}"

pushd "$APP_DIR" >/dev/null

# Forceer app-mode
APP_DIR="${APP_DIR:-${PWD}/app}"
export APP_DIR


if [[ "$BUILD_TYPE" == "pristine" ]]; then
    west build -p always \
        -b "${BOARD}" \
        -d "${BUILD_DIR}" \
        -- -DOVERLAY_CONFIG="${OVERLAY_CONFIG}"
else
    west build \
        -b "${BOARD}" \
        -d "${BUILD_DIR}" \
        -- -DOVERLAY_CONFIG="${OVERLAY_CONFIG}"
fi


popd >/dev/null

echo -e "${GRN}✔ Build voltooid voor node ${NODE}.${RST}"