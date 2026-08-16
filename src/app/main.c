/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：main.c
 * 文件功能：RV1126B 行车记录仪系统 —— 程序入口
 * 作    者：heifast
 * 创建日期：2026-08-10
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "ai_worker.h"
#include "log.h"
#include "file_mgr.h"
#include "detect_share.h"
#include "gst_encoder.h"
#include "gps_worker.h"
#include "preview_share.h"
#include "thread_mgr.h"
#include "ui_main.h"

/* 模块启用宏（调试开关，发布时全 1）
 * 依赖关系：AI 依赖 GST（GStreamer tee 预览分支喂帧）；
 *           UI 预览依赖 GST（preview_share），AI 仅提供检测框叠加 */
#define FILE_ON 1
#define GST_ON  1
#define GPS_ON  1
#define AI_ON   0
#define UI_ON   1

/* 依赖校验：AI 帧源来自 GStreamer 预览分支，关 GST 必须关 AI，
 * 否则 AI 线程无帧忙等烧 CPU */
#if AI_ON && !GST_ON
#error "AI_ON 依赖 GST_ON（GStreamer 预览分支喂帧），两者必须同时启用"
#endif

/* 摄像头与编码参数 */
#define CAMERA_DEV    "/dev/video24"
#define CAMERA_WIDTH  1920
#define CAMERA_HEIGHT 1080
#define CAMERA_FPS    30
#define VIDEO_BITRATE 8000000 /* 8Mbps */

/* AI 检测参数 */
#define AI_MODEL_PATH  "/root/RV1126B_Dashcam_System/model/yolov5.rknn"
#define AI_LABELS_PATH "/root/RV1126B_Dashcam_System/model/coco_80_labels_list.txt"

/* GPS 串口设备 */
#define GPS_DEV "/dev/ttyUSB1"

/* 录像存储（SD 卡，需已格式化为 exFAT 并挂载） */
#define RECORD_DIR  "/mnt/sdcard/videos"
/* 单文件 2 分钟：断电最多丢最后一段（2 分钟），粒度兼顾
 * 循环覆盖与丢失容忍（行车记录仪行业常见 1~3 分钟） */
#define SEGMENT_SEC 120
#define MAX_STORAGE (28ULL * 1024 * 1024 * 1024) /* 28GB 上限（30G 卡留 2G 余量） */

/* 系统退出标志（信号处理函数设置） */
static volatile bool s_is_running = true;

/*****************************************************************************
 * 函数名称：preview_frame_cb
 * 功能描述：GStreamer 预览帧回调——NV12 数据写入预览共享缓冲
 * 输入参数：@nv12      - NV12 帧数据
 *           @user_data - preview_share_t 指针
 * 注意事项：在 GStreamer 内部线程执行，仅做锁内拷贝快速返回
 *****************************************************************************/
static void preview_frame_cb(const uint8_t* nv12, void* user_data)
{
    preview_share_push((preview_share_t*)user_data, nv12);
}

/*****************************************************************************
 * 函数名称：preview_bgra_frame_cb
 * 功能描述：GStreamer BGRA 预览帧回调（管线内 videoconvert 转换完成）
 * 输入参数：@bgra      - BGRA 帧数据（UI 显示格式）
 *           @user_data - preview_share_t 指针（BGRA 实例）
 *****************************************************************************/
static void preview_bgra_frame_cb(const uint8_t* bgra, void* user_data)
{
    preview_share_push((preview_share_t*)user_data, bgra);
}

/* 全局模块实例 */
static gst_encoder_t s_encoder;
static file_mgr_t s_file_mgr;
static gps_worker_t s_gps_worker;
static ai_worker_t s_ai_worker;
static ui_worker_t s_ui_worker;
static thread_mgr_t s_thread_mgr;
static detect_share_t s_detect_share;        /* AI → UI 检测结果 */
static preview_share_t s_preview_share;      /* GStreamer → AI（NV12） */
static preview_share_t s_preview_bgra_share; /* GStreamer → UI（BGRA） */

/* 模块初始化完成标志（错误清理时按序释放） */
static bool s_encoder_ready  = false;
static bool s_file_mgr_ready = false;

/*****************************************************************************
 * 函数名称：signal_handler
 * 功能描述：捕获 SIGINT/SIGTERM，置退出标志
 * 输入参数：@sig - 信号编号
 * 注意事项：仅设置标志位，不做复杂操作
 *****************************************************************************/
static void signal_handler(int sig)
{
    (void)sig;
    s_is_running = false;
}

/*****************************************************************************
 * 函数名称：register_signals
 * 功能描述：注册信号处理函数
 * 返回值：  成功0，失败-1
 *****************************************************************************/
static int register_signals(void)
{
    if (SIG_ERR == signal(SIGINT, signal_handler)) {
        perror("[ERROR] 注册 SIGINT 失败");
        return -1;
    }
    if (SIG_ERR == signal(SIGTERM, signal_handler)) {
        perror("[ERROR] 注册 SIGTERM 失败");
        return -1;
    }
    return 0;
}

/*****************************************************************************
 * 函数名称：system_init
 * 功能描述：初始化文件管理器和编码器，注册业务线程
 * 返回值：  成功0，失败-1
 * 注意事项：错误路径 goto cleanup 集中清理，顺序 stop→deinit
 *****************************************************************************/
static int system_init(void)
{
    int ret = -1;

    /* 跨线程共享数据 */
    detect_share_init(&s_detect_share); /* 检测结果共享（AI → UI） */
    preview_share_init(&s_preview_share, PREVIEW_NV12_SIZE);
    preview_share_init(&s_preview_bgra_share, PREVIEW_BGRA_SIZE);

#if FILE_ON /* 文件管理器 */
    if (0 != file_mgr_init(&s_file_mgr, RECORD_DIR, MAX_STORAGE)) {
        LOG_E("MAIN", "文件管理器初始化失败");
        goto cleanup;
    }
    s_file_mgr_ready = true;
    LOG_I("MAIN", "已有 %d 个录像文件", file_mgr_get_count(&s_file_mgr));
#endif

#if GST_ON /* GStreamer */
    gst_encoder_config_t enc_config;
    memset(&enc_config, 0, sizeof(enc_config));
    enc_config.device      = CAMERA_DEV;
    enc_config.dir         = RECORD_DIR;
    enc_config.segment_sec = SEGMENT_SEC;
    enc_config.width       = CAMERA_WIDTH;
    enc_config.height      = CAMERA_HEIGHT;
    enc_config.fps         = CAMERA_FPS;
    enc_config.bitrate     = VIDEO_BITRATE;
    if (0 != gst_encoder_init(&s_encoder, &enc_config)) {
        LOG_E("MAIN", "编码器初始化失败");
        goto cleanup;
    }
    s_encoder_ready = true;
    /* 开始录像 */
    if (0 != gst_encoder_start(&s_encoder)) {
        LOG_E("MAIN", "编码器启动失败");
        goto cleanup;
    }
    /* 注册预览帧回调（tee 分支 appsink → 预览共享缓冲 → AI 线程） */
    gst_encoder_set_preview_cb(&s_encoder, preview_frame_cb, &s_preview_share);
    gst_encoder_set_preview_bgra_cb(&s_encoder, preview_bgra_frame_cb,
                                    &s_preview_bgra_share);
#endif

#if GPS_ON /* GPS 线程 */
    memset(&s_gps_worker, 0, sizeof(s_gps_worker));
    strncpy(s_gps_worker.dev, GPS_DEV, sizeof(s_gps_worker.dev) - 1);
    s_gps_worker.running = &s_is_running;
    if (0 != thread_mgr_add(&s_thread_mgr, "gps",
                            gps_worker_entry, &s_gps_worker)) {
        LOG_E("MAIN", "注册 GPS 线程失败");
        goto cleanup;
    }
#endif

#if AI_ON
    /* AI 检测线程 */
    memset(&s_ai_worker, 0, sizeof(s_ai_worker));
    strncpy(s_ai_worker.model_path, AI_MODEL_PATH,
            sizeof(s_ai_worker.model_path) - 1);
    strncpy(s_ai_worker.labels_path, AI_LABELS_PATH,
            sizeof(s_ai_worker.labels_path) - 1);
    s_ai_worker.running       = &s_is_running;
    s_ai_worker.detect_share  = &s_detect_share;
    s_ai_worker.preview_share = &s_preview_share;
    s_ai_worker.file_mgr      = &s_file_mgr; /* person 联动紧急锁定 */
    if (0 != thread_mgr_add(&s_thread_mgr, "ai",
                            ai_worker_entry, &s_ai_worker)) {
        LOG_E("MAIN", "注册 AI 线程失败");
        goto cleanup;
    }
#endif

#if UI_ON
    /* UI 线程 */
    memset(&s_ui_worker, 0, sizeof(s_ui_worker));
    s_ui_worker.running       = &s_is_running;
    s_ui_worker.file_mgr      = &s_file_mgr;
    s_ui_worker.preview_share = &s_preview_bgra_share;
    s_ui_worker.gps           = &s_gps_worker;
    s_ui_worker.detect_share  = &s_detect_share;
    if (0 != thread_mgr_add(&s_thread_mgr, "ui",
                            ui_worker_entry, &s_ui_worker)) {
        LOG_E("MAIN", "注册 UI 线程失败");
        goto cleanup;
    }
#endif

    ret = 0;
    return 0;

cleanup:
    /* 若流水线已创建，先 stop 发 EOS 封口，再 deinit */
    if (s_encoder_ready) {
        gst_encoder_stop(&s_encoder);
    }
    gst_encoder_deinit(&s_encoder);
    if (s_file_mgr_ready) {
        file_mgr_deinit(&s_file_mgr);
    }
    return ret;
}

/*****************************************************************************
 * 函数名称：system_deinit
 * 功能描述：停止录像，等待线程收尾，释放资源
 *****************************************************************************/
static void system_deinit(void)
{
    /* 等业务线程退出（先置标志，线程自己收尾） */
    thread_mgr_join_all(&s_thread_mgr);

    gst_encoder_stop(&s_encoder);
    gst_encoder_deinit(&s_encoder);
    file_mgr_deinit(&s_file_mgr);
    /* 预览共享缓冲：线程已全部退出，可安全释放 */
    preview_share_deinit(&s_preview_share);
    preview_share_deinit(&s_preview_bgra_share);
    LOG_I("MAIN", "系统资源已释放");
}

/*****************************************************************************
 * 函数名称：main
 * 功能描述：程序入口 —— 初始化、录像巡检循环、等信号、收尾
 * 输入参数：@argc/@argv - 命令行参数
 * 返回值：  成功0，失败非0
 *****************************************************************************/
int main(int argc, char* argv[])
{
    int ret = EXIT_FAILURE;

    (void)argc;
    (void)argv;

    /* 关闭 RGA 库的调试日志输出（必须在库加载前设置）。
     * 注：lv_drivers 内部调用 legacy RGA API 的"弃用警告"是
     * 库内硬编码打印，该环境变量无法静默（实测 1/0 均打印），
     * 属预编译依赖库的固有噪音，功能无影响 */
    setenv("ROCKCHIP_RGA_LOG", "0", 1);

    printf("========================================\n");
    printf("  RV1126B 行车记录仪系统 v0.4.0\n");
    printf("========================================\n\n");

    if (0 != register_signals()) {
        goto error;
    }

    if (0 != system_init()) {
        LOG_E("MAIN", "系统初始化失败");
        goto error;
    }

    /* 启动全部业务线程（GPS/AI/UI） */
    if (0 != thread_mgr_start_all(&s_thread_mgr)) {
        LOG_E("MAIN", "线程启动失败");
        /* 先置退出标志让已启动线程自行退出，否则 join 永久阻塞 */
        s_is_running = false;
        system_deinit();
        goto error;
    }

#if GST_ON
    LOG_I("MAIN", "循环录像中 → %s/rec_XXXXX.mp4", RECORD_DIR);
#endif

    /* 主循环：编码在 GStreamer 内部线程跑，GPS 在业务线程跑，
     * 主线程每秒巡检一次文件系统（新文件入队 + 超限删除） */
    while (s_is_running) {
        sleep(1);
#if FILE_ON
        if (0 != file_mgr_check(&s_file_mgr)) {
            LOG_W("MAIN", "文件巡检失败（SD 卡异常？）");
        }
#endif
    }

    LOG_I("MAIN", "收到退出信号，停止录像...");

    system_deinit();

    ret = EXIT_SUCCESS;

error:
    LOG_I("MAIN", "系统已关闭");
    return ret;
}
