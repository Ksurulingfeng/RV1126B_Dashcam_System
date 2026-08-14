# RV1126B 智能行车记录仪系统

基于瑞芯微 **RV1126B** 的嵌入式 Linux AI 行车记录仪——一个从零构建的完整嵌入式音视频 + AI 项目，覆盖"采集 → 编码 → 存储 → 显示 → 智能联动"全链路。

## 项目简介

本项目以正点原子 ATK-DLRV1126B 开发板为硬件平台，实现了一台功能完整的智能行车记录仪：

- **1080P 循环录像**：GStreamer 管线硬件编码（MPP），5 分钟无缝分段，SD 卡满自动覆盖最旧录像
- **紧急锁定保护**：AI 检测到 person 连续 3 帧自动锁定当前分段（`_E` 后缀持久化），循环覆盖永不删除
- **AI 目标检测**：YOLOv5s 部署于 3.0 TOPS NPU（RKNN INT8 量化），实测约 40fps，检测框实时绘制
- **触摸交互 UI**：LVGL 8.4 + DRM 90° 旋转，自写 Goodix 电容触摸驱动，多页面架构（主页/录像库/设置）
- **GPS 定位**：EC20 4G 模块 NMEA 0183 解析（GGA/RMC），状态栏实时显示
- **FFmpeg 离线处理**：录像缩略图生成（解码抽帧 → 手写 BMP 落盘），回放/导出/修复的扩展基座

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│ 应用层：main（信号处理、模块组装、巡检循环、线程调度）          │
├─────────────────────────────────────────────────────────────┤
│ 业务模块层                                                     │
│  gst_encoder │ file_mgr │ gps_worker │ ai_worker │ ui_main    │
│  thumb_gen   │ frame_share │ thread_mgr │ touch_input          │
├─────────────────────────────────────────────────────────────┤
│ 框架层：                                                       │
│  GStreamer（实时链路：v4l2src/mpph264enc/splitmuxsink）        │
│  FFmpeg（离线文件操作：demux/decode/scale）                    │
│  LVGL + DRM（图形界面与触摸交互）                              │
├─────────────────────────────────────────────────────────────┤
│ 系统库层：V4L2 / DRM / pthread / RKNN / OpenCV / termios      │
├─────────────────────────────────────────────────────────────┤
│ 内核驱动层：IMX415 / RKISP / RKAIQ(3A) / EC20 / MPP           │
├─────────────────────────────────────────────────────────────┤
│ 硬件层：RV1126B + IMX415 + EC20 + LCD + SD                   │
└─────────────────────────────────────────────────────────────┘
```

### 实时录像链路

```
IMX415 → v4l2src → capsfilter(NV12/1080P) → mpph264enc(硬件编码)
       → h264parse → splitmuxsink(5分钟分段+MP4封装) → SD 卡 rec_XXXXX.mp4
                                                        ↓
                                    file_mgr 巡检（超限删最旧、_E 锁定保护）
```

### AI 链路

```
/dev/video24(ISP selfpath) → OpenCV 采集 → letterbox 640×640
                           → RKNN NPU 推理 → NMS 后处理
                           → 检测框绘制（滞后一帧）→ frame_share → UI 显示
                           → person 连续 3 帧 → file_mgr_lock_latest
```

## 硬件平台

| 组件 | 型号 | 说明 |
|------|------|------|
| 主控 | Rockchip RV1126B | 4×Cortex-A53 + 3.0 TOPS NPU |
| 传感器 | Sony IMX415 Starvis | 4K@30fps / 1080P@60fps，星光级 |
| 屏幕 | MIPI-DSI LCD | 720×1280 竖装面板，软件旋转 90° 横屏使用，Goodix 电容触摸 |
| 4G+GPS | Quectel EC20 | LTE Cat.4 + GNSS，已验证 |
| 内存 | 2GB LPDDR4 | — |
| 存储 | 32GB eMMC + 32GB SD | eMMC 系统 / SD 录像（exFAT） |

## 技术栈分工

| 职责 | 技术 |
|------|------|
| 实时录像链路（采集→编码→封装→分段） | GStreamer 管线（v4l2src/mpph264enc/splitmuxsink） |
| 离线文件操作（缩略图/回放/导出/修复） | FFmpeg（libavformat/libavcodec/libswscale） |
| AI 推理 | RKNN（YOLOv5s INT8，NPU 3.0 TOPS） |
| 图形界面 | LVGL 8.4 + DRM + freetype 中文字体 |
| 触摸输入 | 自写 read_cb（Goodix MT Type B 协议解析 + 旋转坐标映射） |
| GPS | EC20 NMEA 0183（GGA/RMC） |
| 构建 | CMake + Ninja + pkg-config 交叉编译 |

## 目录结构

```
RV1126B_Dashcam_System/
├── src/
│   ├── app/          # 程序入口（信号处理、模块组装、巡检循环）
│   ├── av/           # 音视频（gst_encoder 分段录像 + thumb_gen FFmpeg 缩略图）
│   ├── core/         # 核心业务（file_mgr 目录守护：锁定/巡检删除）
│   ├── gps/          # GPS（nmea_parser 解析 + gps_worker 线程）
│   ├── common/       # 公共组件（thread_mgr 线程注册表 + frame_share 帧共享）
│   ├── ui/           # 图形界面（LVGL 多页面 + touch_input 触摸驱动）
│   ├── ai/           # AI 推理（YOLOv5s RKNN + 检测框 + 紧急联动）
│   ├── network/      # 网络（RTSP 推流、云端上传规划中）
│   └── test/         # 单元测试（PC 端可运行）
├── scripts/          # 构建与部署脚本（build.sh / deploy.sh / dashcam_init.sh）
├── config/           # 配置文件
├── docs/             # 文档（需求分析、概要设计、测试报告、开发日志）
└── build/            # 构建产物（交叉编译，不入库）
```

## 快速开始

### 环境要求

- Ubuntu 22.04
- 正点原子 RV1126B 交叉编译工具链（`/opt/atk-dlrv1126b-toolchain`）
- CMake 3.12+ / Ninja
- adb（部署用）

### 编译

```bash
./scripts/build.sh          # Release 交叉编译
./scripts/build.sh -d       # Debug
./scripts/build.sh -c       # 清理重建
```

### 部署到开发板

```bash
./scripts/deploy.sh             # 推送并前台启动（Ctrl+C 优雅退出）
./scripts/deploy.sh -r          # 启动前杀掉旧进程
./scripts/deploy.sh -i          # 安装开机自启脚本
```

板端运行前置条件：

```bash
# 1. SD 卡已格式化为 exFAT 并挂载
mkfs.exfat /dev/mmcblk1p1 && mount /dev/mmcblk1p1 /mnt/sdcard

# 2. ISP 3A 服务器运行（画面曝光）
rkaiq_3A_server &

# 3. EC20 GPS 启用（可选）
printf 'AT+QGPS=1\r' > /dev/ttyUSB2
```

### 单元测试（PC 端，无需开发板）

```bash
# file_mgr（循环覆盖 + 锁定保护）
gcc -std=c11 -Wall -I src/core -o /tmp/test_file_mgr \
    src/core/file_mgr.c src/test/test_file_mgr.c && /tmp/test_file_mgr

# nmea_parser（GGA/RMC 解析）
gcc -std=c11 -Wall -I src/gps -o /tmp/test_nmea \
    src/gps/nmea_parser.c src/test/test_nmea.c -lm && /tmp/test_nmea

# thumb_gen（FFmpeg 缩略图，需先合成测试视频）
gst-launch-1.0 videotestsrc num-buffers=250 ! \
    video/x-raw,width=640,height=360 ! x264enc ! h264parse ! \
    mp4mux ! filesink location=/tmp/test.mp4
gcc -std=c11 -Wall -I src/av -o /tmp/test_thumb_gen \
    src/av/thumb_gen.c src/test/test_thumb_gen.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale) \
    && /tmp/test_thumb_gen /tmp/test.mp4 /tmp/test.bmp 320 180
```

## 开发进度

- [x] GStreamer 分段录像（splitmuxsink 无缝切文件）
- [x] 循环覆盖（file_mgr 巡检，_E 锁定保护）
- [x] NMEA 解析 + GPS 线程（GGA/RMC）
- [x] AI 目标检测（YOLOv5s RKNN，约 40fps）
- [x] 紧急录像联动（person 连续 3 帧 + 双重防重复）
- [x] FFmpeg 缩略图（解码抽帧 + 手写 BMP，PC/板端双验证）
- [x] LVGL 多页面 UI（主页/录像库/设置 + 触摸交互）
- [ ] 缩略图接入录像页
- [ ] 回放功能（FFmpeg 解封装 + h264_rkmpp 硬解）
- [ ] 车牌识别（LPRNet）
- [ ] RTSP 推流与 4G 云端

## 代码规范

C 代码遵循[华为 C 语言编程规范（2011 版）](.claude/CLAUDE.md)（4 空格缩进、snake_case 命名、函数头注释、goto 集中错误处理）。
