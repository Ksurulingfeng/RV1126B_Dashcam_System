# RV1126B 智能行车记录仪系统

基于瑞芯微 **RV1126B** 的嵌入式 Linux AI 行车记录仪——一个从零构建的完整嵌入式音视频 + AI 项目，覆盖"采集 → 编码 → 存储 → 显示 → 智能联动"全链路。

## 项目简介

本项目以正点原子 ATK-DLRV1126B 开发板为硬件平台，实现了一台功能完整的智能行车记录仪：

- **1080P 循环录像**：GStreamer 管线硬件编码（MPP），分段时长可设（默认 2 分钟），SD 卡满自动覆盖最旧录像
- **断电保护**：短分段兜底 + SIGTERM EOS 优雅封口写 moov 索引；文件库按 moov 封口探测自动隐藏未完成分段
- **崩溃恢复**：启动时扫描断电残留，容错扫描 mdat 中 H264 流重建 moov（借用同目录完好文件解码配置），救回最后一段录像
- **音视频双轨录像**：板载咪头 48kHz 单声道 AAC 录音，与视频同封装（mp4mux 音视频双轨 MP4）
- **录像回放**：LVGL 独立播放器（dashcam_player）——底部进度条拖动 seek + 当前/总时长显示、⏮⏸⏭ ±15s 快进快退/暂停、✕ 退出，文件库点击缩略图全屏回放
- **局域网推流**：编码流 tee 分路 RTP/UDP，Windows VLC 实时观看；设置页"局域网推流"开关随时启停（valve 数据闸门），不影响录像
- **紧急锁定保护**：AI 检测到 person 连续 3 帧自动锁定当前分段（`_E` 后缀持久化），循环覆盖永不删除
- **AI 目标检测**：YOLOv5s 部署于 3.0 TOPS NPU（RKNN INT8 量化），实测约 40fps，检测框实时绘制
- **触摸交互 UI**：LVGL 8.4 + DRM 90° 旋转，自写 Goodix 电容触摸驱动，多页面架构（主页/录像库/设置）
- **设置体系**：AI 识别（画框/自动锁定从属联动）、录像、录音、局域网推流等开关与分段时长设定，key=value 配置持久化，主循环秒级巡检应用
- **GPS 定位**：EC20 4G 模块 NMEA 0183 解析（GGA/RMC），状态栏实时显示
- **FFmpeg 离线处理**：录像缩略图生成（解码抽帧 → 手写 BMP 落盘），回放/导出/修复的扩展基座

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│ 应用层：main（信号处理、模块组装、巡检循环、线程调度）          │
├─────────────────────────────────────────────────────────────┤
│ 业务模块层                                                     │
│  gst_encoder │ file_mgr │ gps_worker │ ai_worker │ ui_main    │
│  thumb_gen   │ preview_share │ detect_share │ thread_mgr       │
│  touch_input │ log（分级日志）                                  │
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

### 实时录像链路（同源 tee 分路：录像 / 局域网推流 / 预览-AI，断电保护）

```
IMX415 → v4l2src → capsfilter(NV12/1080P) → tee ─┬→ queue → mpph264enc(硬编码) → tee te
                                                  │     ├→ valve rv(录像开关) → h264parse → splitmuxsink
                                                  │     │     （分段时长可设，默认 mp4mux：EOS/切段封口写 moov 索引）
                                                  │     │   + alsasrc(48k 单声道) → AAC → 同段音视频双轨
                                                  │     └→ valve sv(推流开关) → h264parse
                                                  │           → rtph264pay(pt=96, config-interval=-1)
                                                  │           → udpsink(host=192.168.26.2:5000) → Windows VLC 实时观看
                                                  │           （关闭开关 VLC 画面停、开启恢复，录像不受影响）
                                                  └→ queue(限1帧丢旧) → videoscale(720p)
                                                        → tee2 ─┬→ appsink(NV12) → AI 推理
                                                                └→ videoconvert(BGRA)
                                                                   → appsink → UI 直拷
                                                        ↓
                                    file_mgr 巡检（超限删最旧、_E 锁定保护、
                                                 moov 封口探测隐藏未完成分段）
```

`mpph264enc` 输出经 `tee name=te` 一分为二：录像分支走 `valve rv` 分段落盘；推流分支走 `valve sv` → RTP/UDP 实时上送，两分支共用同一编码流。推流启停用 GStreamer `valve` 数据闸门（drop=true 停流、false 恢复），运行时切换对录像零影响。

### AI 链路

```
GStreamer tee 预览分支(NV12 720p，与录像同源同视野)
  → preview_share（帧共享）→ RKNN 推理（letterbox 640×640）→ NMS 后处理
  → detect_share（检测结果）→ UI 检测框叠加
  → person 连续 3 帧 → file_mgr_lock_latest（紧急锁定 _E）
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
| 录像回放（本地播放器） | GStreamer（mppvideodec 硬解 + appsink → LVGL canvas） |
| 局域网推流（RTP/UDP 实时上送） | GStreamer（tee 分路 + rtph264pay + udpsink） |
| 离线文件操作（缩略图/导出/修复） | FFmpeg（libavformat/libavcodec/libswscale） |
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
│   ├── av/           # 音视频（gst_encoder 分段录像 + thumb_gen 缩略图 + video_recover 断电恢复）
│   ├── core/         # 核心业务（file_mgr 目录守护：锁定/巡检删除）
│   ├── gps/          # GPS（nmea_parser 解析 + gps_worker 线程）
│   ├── common/       # 公共组件（thread_mgr + preview_share/detect_share 共享 + settings 配置 + log 日志）
│   ├── ui/           # 图形界面（LVGL 多页面 + touch_input 触摸驱动）
│   ├── ai/           # AI 推理（YOLOv5s RKNN + 检测框 + 紧急联动）
│   ├── player/       # 独立回放播放器（dashcam_player：进度条 seek/±15s/暂停/退出）
│   ├── network/      # 网络（RTSP/云端上传规划中；RTP/UDP 推流实现在 av/gst_encoder tee 分支）
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
# file_mgr（循环覆盖 + 锁定保护 + moov 封口探测）
gcc -std=c11 -Wall -I src/core -I src/common -o /tmp/test_file_mgr \
    src/core/file_mgr.c src/test/test_file_mgr.c -lpthread \
    && /tmp/test_file_mgr

# video_recover（断电残留恢复，需真实坏文件）
gcc -std=c11 -Wall -I src/av -I src/common -o /tmp/test_video_recover \
    src/av/video_recover.c src/test/test_video_recover.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil) \
    && /tmp/test_video_recover <断电残留.mp4>

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

## 代码规范

C 代码遵循[华为 C 语言编程规范（2011 版）](.claude/CLAUDE.md)（4 空格缩进、snake_case 命名、函数头注释、goto 集中错误处理）。
