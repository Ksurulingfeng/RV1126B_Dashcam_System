#!/bin/sh
#
# 行车记录仪主程序开机自启
# 依赖: S00mountall(SD挂载) → S40rkaiq_3A(ISP曝光) → 本脚本 S50
#

APP_DIR="/root/RV1126B_Dashcam_System"
APP_BIN="${APP_DIR}/RV1126B_Dashcam_System"

case "$1" in
  start)
    # 等 SD 卡挂载完成
    sleep 2
    if ! mount | grep -q /mnt/sdcard; then
        echo "dashcam: SD card not mounted" | logger -t dashcam
        exit 1
    fi

    if [ ! -x "${APP_BIN}" ]; then
        echo "dashcam: ${APP_BIN} not found" | logger -t dashcam
        exit 1
    fi

    # 后台启动，输出落日志
    # 先 cd 到程序目录：AI 模型的 labels 文件用相对路径
    start-stop-daemon -S -m -b -p /tmp/.dashcam --startas \
      /bin/sh -- -c "cd ${APP_DIR} && ${APP_BIN} 2>&1 | logger -t dashcam"
    echo "dashcam: started" | logger -t dashcam
    ;;
  stop)
    killall RV1126B_Dashcam_System 2>/dev/null
    ;;
  restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart}"
    exit 1
    ;;
esac
exit 0
