#!/usr/bin/env bash
set -euo pipefail

# =============================================================
# flash-xiao.sh — Flash tool for NCS Thread Sensor Node
# =============================================================

APP_DIR="$(cd "$(dirname "$0")" && pwd -P)"

RED="\033[1;31m"
GRN="\033[1;32m"
CYN="\033[1;36m"
RST="\033[0m"

usage() {
    cat <<EOF
Usage:
  ./flash-xiao.sh --node NDxx [--runner <runner>] [--build-dir <dir>]

Options:
  --node NDxx          Required, selects nodes/NDxx.conf + build/NDxx/
  --runner <name>      Optional (jlink | nrfjprog | nrfutil)
  --build-dir <dir>    If you want to override build/<NDxx>
  -h, --help           Show this help
EOF
}

NODE=""
RUNNER=""
BUILD_DIR_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node)
            NODE="${2:-}"
            shift 2
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

BUILD_DIR="${BUILD_DIR_OVERRIDE:-$APP_DIR/build/$NODE}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo -e "${RED}❌ Build dir bestaat niet:${RST} $BUILD_DIR"
    echo -e "   Tip: eerst bouwen via: ./build-xiao.sh --node $NODE"
    exit 1
fi

echo -e "${CYN}---------------------------------------${RST}"
echo -e "${GRN}Flashing node        :${RST} $NODE"
echo -e "${GRN}Build dir            :${RST} $BUILD_DIR"
echo -e "${GRN}Runner               :${RST} ${RUNNER:-<default>}"
echo -e "${CYN}---------------------------------------${RST}"

if [[ -n "$RUNNER" ]]; then
    west flash -d "$BUILD_DIR" -r "$RUNNER"
else
    west flash -d "$BUILD_DIR"
fi

echo -e "${GRN}✔ Flash klaar voor node ${NODE}.${RST}"