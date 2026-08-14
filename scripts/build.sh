#!/bin/bash
# 功能：交叉编译 RV1126B 行车记录仪系统

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
TOOLCHAIN_DIR="/opt/atk-dlrv1126b-toolchain"
TOOLCHAIN_GCC="${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-gcc"
TOOLCHAIN_GXX="${TOOLCHAIN_DIR}/bin/aarch64-buildroot-linux-gnu-g++"

BUILD_TYPE="Release"
DO_CLEAN=0

# 颜色
C_R='\033[0;31m'; C_G='\033[0;32m'; C_Y='\033[0;33m'; C_C='\033[0;36m'; C_N='\033[0m'
info()  { echo -e "${C_C}[INFO]${C_N}  $*"; }
ok()    { echo -e "${C_G}[OK]${C_N}    $*"; }
err()   { echo -e "${C_R}[ERROR]${C_N} $*"; }

show_help() {
    cat << EOF
用法: $(basename "$0") [选项]

选项:
  -d           Debug 编译（默认 Release）
  -c, --clean  清理 build 后重新编译
  -h, --help   显示帮助
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -d) BUILD_TYPE="Debug"; shift ;;
            -c|--clean) DO_CLEAN=1; shift ;;
            -h|--help) show_help; exit 0 ;;
            *) err "未知选项: $1"; show_help; exit 1 ;;
        esac
    done
}

check_toolchain() {
    info "检查工具链..."
    if [[ ! -f "${TOOLCHAIN_GCC}" ]]; then
        err "找不到编译器: ${TOOLCHAIN_GCC}"
        exit 1
    fi
    ok "工具链就绪"
}

run_cmake() {
    info "CMake 配置 (${BUILD_TYPE})..."
    mkdir -p "${BUILD_DIR}"
    cmake -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_C_COMPILER="${TOOLCHAIN_GCC}" \
        -DCMAKE_CXX_COMPILER="${TOOLCHAIN_GXX}" \
        -S "${PROJECT_ROOT}" \
        -B "${BUILD_DIR}"
    ok "CMake 配置完成"
}

run_ninja() {
    info "编译中..."
    ninja -C "${BUILD_DIR}" -j"$(nproc)"
    ok "编译完成"
}

main() {
    parse_args "$@"
    check_toolchain
    [[ ${DO_CLEAN} -eq 1 ]] && { info "清理 build..."; rm -rf "${BUILD_DIR}"; ok "清理完成"; }
    run_cmake
    run_ninja

    local bin="${BUILD_DIR}/RV1126B_Dashcam_System"
    if [[ -f "${bin}" ]]; then
        ok "构建成功 — $(du -h "${bin}" | cut -f1) — ${BUILD_TYPE}"
    else
        err "构建失败"
        exit 1
    fi
}

main "$@"
