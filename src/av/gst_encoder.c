/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：gst_encoder.c
 * 文件功能：GStreamer 分段录像编码器实现
 * 作    者：heifast
 * 创建日期：2026-08-13
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include <gst/gst.h>

#include "gst_encoder.h"

/* splitmuxsink 文件命名模板缓冲大小 */
#define LOCATION_BUF_SIZE 512

/* EOS 封口等待上限（秒），超时说明流水线卡死 */
#define EOS_WAIT_SEC 2


/*****************************************************************************
 * 函数名称：get_next_file_index
 * 功能描述：扫描目录找最大录像序号 +1，重启后新录像不会覆盖旧文件
 * 输入参数：@dir - 录像目录
 * 返回值：  下一个可用序号（目录不存在时返回 0）
 * 注意事项：只匹配 rec_%05d.mp4 或 rec_%05d_E.mp4，其他文件不参与序号
 *****************************************************************************/
static int get_next_file_index(const char *dir)
{
    DIR *d = NULL;
    struct dirent *entry = NULL;
    int max_index = -1;

    d = opendir(dir);
    if (NULL == d) {
        return 0;
    }

    while ((entry = readdir(d)) != NULL) {
        int idx;
        int consumed;

        /* %n 记录解析停止位置，剩余串必须是 .mp4 或 _E.mp4 才算录像文件 */
        if ((1 == sscanf(entry->d_name, "rec_%d%n", &idx, &consumed)) &&
            ((0 == strcmp(entry->d_name + consumed, ".mp4")) ||
             (0 == strcmp(entry->d_name + consumed, "_E.mp4")))) {
            if (idx > max_index) {
                max_index = idx;
            }
        }
    }
    closedir(d);

    return max_index + 1;
}


/*****************************************************************************
 * 函数名称：setup_sink_props
 * 功能描述：配置 splitmuxsink 属性（命名模板/分段时长/序号接续）
 * 输入参数：@sink   - splitmuxsink 元件
 *           @config - 编码配置
 * 注意事项：序号扫描在重启时接续旧文件，防止覆盖
 *****************************************************************************/
static void setup_sink_props(GstElement *sink,
                             const gst_encoder_config_t *config)
{
    char location[LOCATION_BUF_SIZE];
    int start_index;

    snprintf(location, sizeof(location), "%s/rec_%%05d.mp4", config->dir);
    start_index = get_next_file_index(config->dir);
    g_object_set(G_OBJECT(sink), "location", location, NULL);
    g_object_set(G_OBJECT(sink), "max-size-time",
                 (guint64)config->segment_sec * GST_SECOND, NULL);
    g_object_set(G_OBJECT(sink), "start-index", start_index, NULL);
}


/*****************************************************************************
 * 函数名称：create_record_pipeline
 * 功能描述：创建并组装分段录像流水线
 *           v4l2src → capsfilter → mpph264enc → h264parse → splitmuxsink
 * 输入参数：@enc    - 编码器上下文（保存 pipeline/bus）
 *           @config - 编码配置
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：元件 add 进 bin 后所有权归 bin，失败路径只需释放
 *           caps 和 pipeline（bin 析构会释放全部子元件）
 *****************************************************************************/
static int create_record_pipeline(gst_encoder_t *enc,
                                  const gst_encoder_config_t *config)
{
    GstElement *pipeline = NULL;
    GstElement *src = NULL;
    GstElement *capsfilter = NULL;
    GstElement *encoder = NULL;
    GstElement *parser = NULL;
    GstElement *sink = NULL;
    GstCaps    *caps = NULL;

    /* 创建流水线容器 */
    pipeline = gst_pipeline_new("record-pipeline");
    if (NULL == pipeline) {
        return -1;
    }

    /* 创建各元件：
     * v4l2src 采集 / capsfilter 格式过滤 / mpph264enc RK 硬件编码 /
     * h264parse 码流整理 / splitmuxsink 分段封装 */
    src        = gst_element_factory_make("v4l2src",      "src");
    capsfilter = gst_element_factory_make("capsfilter",   "filter");
    encoder    = gst_element_factory_make("mpph264enc",   "encoder");
    parser     = gst_element_factory_make("h264parse",    "parser");
    sink       = gst_element_factory_make("splitmuxsink", "sink");
    if ((NULL == src) || (NULL == capsfilter) || (NULL == encoder) ||
        (NULL == parser) || (NULL == sink)) {
        fprintf(stderr, "创建 GStreamer 元件失败\n");
        goto error;
    }

    /* 采集与编码属性（gop=帧率 → 关键帧间隔 1 秒） */
    g_object_set(G_OBJECT(src), "device", config->device, NULL);
    g_object_set(G_OBJECT(encoder), "bps", (guint)config->bitrate, NULL);
    g_object_set(G_OBJECT(encoder), "gop", (gint)config->fps, NULL);

    /* splitmuxsink：命名模板/分段时长/序号接续 */
    setup_sink_props(sink, config);

    /* 格式过滤：NV12 / 分辨率 / 帧率 */
    caps = gst_caps_new_simple(
        "video/x-raw",
        "format",    G_TYPE_STRING, "NV12",
        "width",     G_TYPE_INT,    (int)config->width,
        "height",    G_TYPE_INT,    (int)config->height,
        "framerate", GST_TYPE_FRACTION, (int)config->fps, 1,
        NULL);
    if (NULL == caps) {
        goto error;
    }
    g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
    gst_caps_unref(caps);
    caps = NULL;

    /* 组装：先 add（bin 接管元件所有权），后 link */
    gst_bin_add_many(GST_BIN(pipeline), src, capsfilter, encoder,
                     parser, sink, NULL);
    if ((FALSE == gst_element_link(src, capsfilter)) ||
        (FALSE == gst_element_link(capsfilter, encoder)) ||
        (FALSE == gst_element_link(encoder, parser)) ||
        (FALSE == gst_element_link(parser, sink))) {
        fprintf(stderr, "GStreamer 元件连接失败\n");
        goto error;
    }

    /* 保存上下文 */
    enc->pipeline = pipeline;
    enc->bus      = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    return 0;

error:
    /* 元件已入 bin 由 pipeline 析构释放，这里只释放 caps 和 pipeline */
    if (NULL != caps) {
        gst_caps_unref(caps);
    }
    gst_object_unref(pipeline);
    return -1;
}


/*****************************************************************************
 * 函数名称：gst_encoder_init
 * 功能描述：创建分段录像流水线（参数校验 + 状态清理 + 组装）
 * 输入参数：@enc    - 编码器上下文
 *           @config - 编码配置
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：重复 init 会先清理旧流水线；调用契约 start→stop→deinit
 *****************************************************************************/
int gst_encoder_init(gst_encoder_t *enc, const gst_encoder_config_t *config)
{
    if ((NULL == enc) || (NULL == config) ||
        (NULL == config->device) || (NULL == config->dir)) {
        return -1;
    }

    /* 参数校验：0 无物理意义 */
    if ((0 == config->segment_sec) || (0 == config->width) ||
        (0 == config->height) || (0 == config->fps) ||
        (0 == config->bitrate)) {
        fprintf(stderr, "[encoder] 无效编码参数\n");
        return -1;
    }

    /* 重复 init 时先清理旧流水线，防止引用丢失 */
    if (NULL != enc->pipeline) {
        gst_encoder_deinit(enc);
    }
    memset(enc, 0, sizeof(*enc));

    /* GStreamer 框架初始化（幂等，可安全多次调用） */
    gst_init(NULL, NULL);

    return create_record_pipeline(enc, config);
}


/*****************************************************************************
 * 函数名称：gst_encoder_start
 * 功能描述：启动流水线（PLAYING），开始采集编码
 * 输入参数：@enc - 编码器上下文
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：须在 init 成功后调用
 *****************************************************************************/
int gst_encoder_start(gst_encoder_t *enc)
{
    GstStateChangeReturn ret;

    if ((NULL == enc) || (NULL == enc->pipeline)) {
        return -1;
    }

    /* NULL → PLAYING，GStreamer 内部自动经过 READY/PAUSED */
    ret = gst_element_set_state(enc->pipeline, GST_STATE_PLAYING);
    if (GST_STATE_CHANGE_FAILURE == ret) {
        fprintf(stderr, "流水线启动失败\n");
        return -1;
    }

    return 0;
}


/*****************************************************************************
 * 函数名称：gst_encoder_stop
 * 功能描述：停止流水线
 * 输入参数：@enc - 编码器上下文
 * 注意事项：必须发 EOS 让 splitmuxsink 把最后一个分段正确封口；
 *           EOS 2 秒未到达则告警（文件可能损坏）
 *****************************************************************************/
void gst_encoder_stop(gst_encoder_t *enc)
{
    GstMessage *msg;

    if ((NULL == enc) || (NULL == enc->pipeline)) {
        return;
    }

    /* 发 EOS：事件沿数据流向下游传播，splitmuxsink 收到后封口当前文件 */
    if (FALSE == gst_element_send_event(enc->pipeline,
                                        gst_event_new_eos())) {
        fprintf(stderr, "[encoder] EOS 发送失败（流水线未运行？）\n");
    }

    /* 等 EOS 传递完（最多 EOS_WAIT_SEC 秒） */
    if (NULL != enc->bus) {
        msg = gst_bus_timed_pop_filtered(enc->bus,
                                         EOS_WAIT_SEC * GST_SECOND,
                                         GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (NULL == msg) {
            fprintf(stderr, "[encoder] EOS 超时，最后分段可能不完整\n");
        } else {
            gst_message_unref(msg);
        }
    }

    /* 回到 NULL 状态 */
    gst_element_set_state(enc->pipeline, GST_STATE_NULL);
}


/*****************************************************************************
 * 函数名称：gst_encoder_deinit
 * 功能描述：销毁流水线，释放全部资源
 * 输入参数：@enc - 编码器上下文
 * 注意事项：防御性先回 NULL 态——未 stop 直接 deinit 时仍能触发
 *           EOS 收尾流程，避免当前分段缺 moov 损坏
 *****************************************************************************/
void gst_encoder_deinit(gst_encoder_t *enc)
{
    if (NULL == enc) {
        return;
    }

    if (NULL != enc->bus) {
        gst_object_unref(enc->bus);
        enc->bus = NULL;
    }

    if (NULL != enc->pipeline) {
        /* 幂等：已回 NULL 态时此调用无副作用 */
        gst_element_set_state(enc->pipeline, GST_STATE_NULL);
        gst_object_unref(enc->pipeline);
        enc->pipeline = NULL;
    }
}
