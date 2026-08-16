#!/bin/bash
# 功能：ADB 部署 RV1126B 行车记录仪系统到开发板

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
BINARY_NAME="RV1126B_Dashcam_System"
BINARY_PATH="${BUILD_DIR}/${BINARY_NAME}"
CONFIG_DIR="${PROJECT_ROOT}/config"

TARGET_DIR="/root/RV1126B_Dashcam_System"
INIT_SCRIPT="${PROJECT_ROOT}/scripts/dashcam_init.sh"
DO_LAUNCH=1
DO_RESTART=0
DO_INSTALL_INIT=0

# 颜色
C_R='\033[0;31m'; C_G='\033[0;32m'; C_Y='\033[0;33m'; C_C='\033[0;36m'; C_N='\033[0m'
info()  { echo -e "${C_C}[INFO]${C_N}  $*"; }
ok()    { echo -e "${C_G}[OK]${C_N}    $*"; }
warn()  { echo -e "${C_Y}[WARN]${C_N}  $*"; }
err()   { echo -e "${C_R}[ERROR]${C_N} $*"; }

show_help() {
    cat << EOF
用法: $(basename "$0") [选项]

选项:
  -p <路径>   目标路径（默认: ${TARGET_DIR}）
  -n          部署后启动程序
  -r          启动前杀掉已有进程
  -c <目录>   同步 config 目录到目标
  -i          安装开机自启脚本（S50dashcam）
  -h, --help  显示帮助
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -p) TARGET_DIR="$2"; shift 2 ;;
            -n) DO_LAUNCH=1; shift ;;
            -r) DO_RESTART=1; shift ;;
            -c) CONFIG_TARGET="$2"; shift 2 ;;
            -i) DO_INSTALL_INIT=1; shift ;;
            -h|--help) show_help; exit 0 ;;
            *) err "未知选项: $1"; show_help; exit 1 ;;
        esac
    done
}

check_adb() {
    info "检查 ADB 连接..."
    if ! command -v adb &>/dev/null; then
        err "未找到 adb 命令"
        exit 1
    fi
    # 注意：grep 无匹配时退出码 1，set -e 下直接赋值会终止脚本，
    # 用 || true 保证命令组退出码为 0
    local n
    n=$(adb devices 2>/dev/null | grep -vc "List of devices\|^$" || true)
    if [[ ${n} -eq 0 ]]; then
        err "未检测到 ADB 设备"
        exit 1
    fi
    ok "设备已连接"
}

check_binary() {
    info "检查编译产物..."
    if [[ ! -f "${BINARY_PATH}" ]]; then
        err "未找到 ${BINARY_PATH}，请先执行 build.sh"
        exit 1
    fi
    ok "编译产物就绪 ($(du -h "${BINARY_PATH}" | cut -f1))"
}

ensure_dir() {
    info "确保目录: ${TARGET_DIR}"
    adb shell "mkdir -p ${TARGET_DIR}" 2>/dev/null || { warn "无法创建目录"; return 1; }
    ok "目录就绪"
}

push_binary() {
    info "推送 → ${TARGET_DIR}/${BINARY_NAME}"
    adb push "${BINARY_PATH}" "${TARGET_DIR}/${BINARY_NAME}"
    adb shell "chmod +x ${TARGET_DIR}/${BINARY_NAME}"
    ok "推送完成"
}

push_config() {
    if [[ -z "${CONFIG_TARGET:-}" ]]; then
        return 0
    fi
    info "同步配置 → ${CONFIG_TARGET}"
    if [[ -d "${CONFIG_DIR}" ]] && [[ -n "$(ls -A "${CONFIG_DIR}" 2>/dev/null)" ]]; then
        adb push "${CONFIG_DIR}/." "${CONFIG_TARGET}/"
        ok "配置同步完成"
    else
        warn "config 目录为空，跳过"
    fi
}

install_init() {
    if [[ ${DO_INSTALL_INIT} -ne 1 ]]; then
        return 0
    fi
    info "安装开机自启脚本 → /etc/init.d/S50dashcam"
    adb push "${INIT_SCRIPT}" /etc/init.d/S50dashcam
    adb shell "chmod +x /etc/init.d/S50dashcam"
    ok "开机自启已安装（重启后自动录像）"
}

stop_existing() {
    if [[ ${DO_RESTART} -ne 1 ]]; then
        return 0
    fi
    info "停止已有进程..."
    adb shell "killall ${BINARY_NAME}" 2>/dev/null || true

    # 等旧进程完全退出（EOS 封口写 moov 需要时间），最多 10 秒。
    # 不等就启动新程序会并发写同一批文件，产生打不开的坏段
    for i in $(seq 1 20); do
        if [[ -z "$(adb shell "pidof ${BINARY_NAME}" 2>/dev/null)" ]]; then
            break
        fi
        sleep 0.5
    done
    ok "已停止旧进程"
}

launch_app() {
    if [[ ${DO_LAUNCH} -ne 1 ]]; then
        return 0
    fi
    info "启动程序（Ctrl+C 结束，程序收到 SIGTERM 优雅退出）..."

    # 前台运行：
    #   -t          分配 TTY，Ctrl+C 能转发成板子程序的 SIGINT
    #   stdbuf -oL  行缓冲，管道输出实时显示
    adb shell -t "cd ${TARGET_DIR} && stdbuf -oL ./${BINARY_NAME}"
}

main() {
    echo ""
    info "RV1126B 行车记录仪系统 — ADB 部署"

    parse_args "$@"
    check_binary
    check_adb
    ensure_dir
    stop_existing
    push_binary
    push_config
    install_init
    launch_app
}

main "$@"
