#!/usr/bin/env bash
set -euo pipefail

# =============================================================
# flash-xiao.sh — Flash tool for NCS Thread Sensor Node
# =============================================================

APP_DIR="$(cd "$(dirname "$0")" && pwd -P)"

RED="\033[1;31m"
GRN="\033[1;32m"
YLW="\033[1;33m"
CYN="\033[1;36m"
RST="\033[0m"

usage() {
    cat <<EOF
Usage:
  ./flash-xiao.sh --node NDxx [--battery] [--runner <runner>] [--build-dir <dir>]

Options:
  --node NDxx          Required, selects nodes/NDxx.conf + build/NDxx/
  --battery            Flash the battery build (from build/NDxx-battery/)
  --runner <name>      Optional (jlink | nrfjprog | nrfutil)
  --build-dir <dir>    Override build directory entirely
  -h, --help           Show this help
EOF
}

NODE=""
RUNNER=""
BUILD_DIR_OVERRIDE=""
BATTERY_MODE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node)
            NODE="${2:-}"
            shift 2
            ;;
        --battery)
            BATTERY_MODE=1
            shift
            ;;
        --runner|-r)
            RUNNER="${2:-}"
            shift 2
            ;;
        --build-dir|-d)
            BUILD_DIR_OVERRIDE="${2:-}"
            shift 2
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

if [[ -n "$BUILD_DIR_OVERRIDE" ]]; then
    BUILD_DIR="$BUILD_DIR_OVERRIDE"
elif [[ $BATTERY_MODE -eq 1 ]]; then
    BUILD_DIR="$APP_DIR/build/${NODE}-battery"
else
    BUILD_DIR="$APP_DIR/build/${NODE}"
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo -e "${RED}❌ Build dir bestaat niet:${RST} $BUILD_DIR"
    if [[ $BATTERY_MODE -eq 1 ]]; then
        echo -e "   Tip: eerst bouwen via: ./build-xiao.sh --node $NODE --battery"
    else
        echo -e "   Tip: eerst bouwen via: ./build-xiao.sh --node $NODE"
    fi
    exit 1
fi

echo -e "${CYN}---------------------------------------${RST}"
echo -e "${GRN}Flashing node        :${RST} $NODE"
if [[ $BATTERY_MODE -eq 1 ]]; then
echo -e "${YLW}Battery mode         :${RST} YES — UART disabled build"
fi
echo -e "${GRN}Build dir            :${RST} $BUILD_DIR"
echo -e "${GRN}Runner               :${RST} ${RUNNER:-<default>}"
echo -e "${CYN}---------------------------------------${RST}"

if [[ -n "$RUNNER" ]]; then
    west flash -d "$BUILD_DIR" -r "$RUNNER"
else
    west flash -d "$BUILD_DIR"
fi

echo -e "${GRN}✔ Flash klaar voor node ${NODE}.${RST}"