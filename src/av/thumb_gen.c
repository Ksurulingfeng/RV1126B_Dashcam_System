/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：thumb_gen.c
 * 文件功能：录像文件缩略图生成（FFmpeg 解码抽帧 → BMP 落盘）
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "log.h"
#include "thumb_gen.h"

/* BMP 格式常量 */
#define BMP_FILE_HEADER_SIZE 14 /* BITMAPFILEHEADER */
#define BMP_INFO_HEADER_SIZE 40 /* BITMAPINFOHEADER */
#define BMP_HEADER_TOTAL     (BMP_FILE_HEADER_SIZE + BMP_INFO_HEADER_SIZE)

/* 缩略图尺寸上限（防止 width*3 等 int 运算溢出） */
#define THUMB_MAX_DIMENSION 8192

/*****************************************************************************
 * 函数名称：fill_bmp_header
 * 功能描述：拼装 24 位 BMP 的 54 字节文件头
 * 输入参数：@header - 输出缓冲（至少 BMP_HEADER_TOTAL 字节）
 *           @width  - 图像宽度
 *           @height - 图像高度
 * 注意事项：1. 小端字节序（RV1126B 与 x86 均为小端，可直接 memcpy）
 *           2. height 写负值表示像素自上而下排列，免去翻转
 *           3. 每行 4 字节对齐的步长由 write_bmp 计算，这里只填头
 *****************************************************************************/
static void fill_bmp_header(uint8_t *header, int width, int height)
{
    uint32_t u32_val;
    uint16_t u16_val;
    int32_t i32_val;
    uint32_t bmp_stride;
    uint32_t image_size;

    /* BMP 每行 4 字节对齐 */
    bmp_stride = ((uint32_t)width * 3 + 3) & ~3U;
    image_size = bmp_stride * (uint32_t)height;

    /* 全部清零，只填需要的字段（保留区/调色板/分辨率保持 0） */
    memset(header, 0, BMP_HEADER_TOTAL);
    header[0] = 'B';
    header[1] = 'M';

    u32_val = BMP_HEADER_TOTAL + image_size;
    memcpy(header + 0x02, &u32_val, sizeof(u32_val)); /* 文件总大小 */
    u32_val = BMP_HEADER_TOTAL;
    memcpy(header + 0x0A, &u32_val, sizeof(u32_val)); /* 像素数据偏移 */
    u32_val = BMP_INFO_HEADER_SIZE;
    memcpy(header + 0x0E, &u32_val, sizeof(u32_val)); /* 信息头大小 */
    i32_val = width;
    memcpy(header + 0x12, &i32_val, sizeof(i32_val)); /* 宽度 */
    i32_val = -height;
    memcpy(header + 0x16, &i32_val, sizeof(i32_val)); /* 高度（负=自上而下） */
    u16_val = 1;
    memcpy(header + 0x1A, &u16_val, sizeof(u16_val)); /* 平面数 */
    u16_val = 24;
    memcpy(header + 0x1C, &u16_val, sizeof(u16_val)); /* 位深 24 */
    /* 0x1E 压缩方式 = 0（不压缩，已清零） */
    u32_val = image_size;
    memcpy(header + 0x22, &u32_val, sizeof(u32_val)); /* 图像数据大小 */
}


/*****************************************************************************
 * 函数名称：write_bmp
 * 功能描述：把 BGR24 像素数据写成 24 位 BMP 文件
 * 输入参数：@path       - 输出文件路径
 *           @data       - BGR24 像素数据指针（首行是图像第一行）
 *           @src_stride - 源数据每行字节数（可能大于 width*3，内存对齐所致）
 *           @width      - 图像宽度
 *           @height     - 图像高度
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：BMP 每行必须 4 字节对齐；源数据按 src_stride 步进逐行读取，
 *           因此对调用方的 linesize 无对齐要求
 *****************************************************************************/
static int write_bmp(const char *path, const uint8_t *data, int src_stride,
                     int width, int height)
{
    FILE *fp = NULL;
    uint8_t header[BMP_HEADER_TOTAL];
    uint8_t padding[3] = {0, 0, 0};
    uint32_t bmp_stride;
    uint32_t pad_size;
    int row;
    int ret = -1;

    if ((NULL == path) || (NULL == data) || (0 >= width) || (0 >= height) ||
        (src_stride < width * 3)) {
        return -1;
    }

    fp = fopen(path, "wb");
    if (NULL == fp) {
        LOG_E("THUMB", "创建缩略图失败: %s", path);
        return -1;
    }

    /* 写 54 字节文件头 */
    fill_bmp_header(header, width, height);
    if (BMP_HEADER_TOTAL != fwrite(header, 1, BMP_HEADER_TOTAL, fp)) {
        goto error;
    }

    /* 逐行写像素：源行按 src_stride 步进，目标行补 0 至 4 字节对齐 */
    bmp_stride = ((uint32_t)width * 3 + 3) & ~3U;
    pad_size   = bmp_stride - (uint32_t)(width * 3);
    for (row = 0; row < height; row++) {
        if ((size_t)(width * 3) !=
            fwrite(data + row * src_stride, 1, (size_t)(width * 3), fp)) {
            goto error;
        }
        /* 行尾补 0（pad_size 为 0 时 fwrite 写 0 字节，无害） */
        if (pad_size != fwrite(padding, 1, pad_size, fp)) {
            goto error;
        }
    }

    ret = 0;

error:
    fclose(fp);
    if (0 != ret) {
        /* 写盘失败不残留半成品文件 */
        remove(path);
    }
    return ret;
}


/*****************************************************************************
 * 函数名称：open_video_decode
 * 功能描述：打开视频文件并打开对应解码器（FFmpeg 流程 ①~⑤）
 * 输入参数：@src_path      - 视频文件路径
 *           @out_fmt_ctx   - 输出：封装上下文
 *           @out_codec_ctx - 输出：解码器上下文
 *           @out_stream_idx- 输出：视频流索引
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：失败时本函数内部释放已申请资源，调用方无需清理
 *****************************************************************************/
static int open_video_decode(const char *src_path,
                             AVFormatContext **out_fmt_ctx,
                             AVCodecContext **out_codec_ctx,
                             int *out_stream_idx)
{
    AVFormatContext *fmt_ctx   = NULL;
    AVCodecContext *codec_ctx  = NULL;
    const AVCodec *codec       = NULL;
    int stream_idx             = -1;

    /* ① 打开文件并探测封装格式（MP4/MKV/AVI...） */
    if (0 > avformat_open_input(&fmt_ctx, src_path, NULL, NULL)) {
        LOG_E("THUMB", "打开视频失败: %s", src_path);
        return -1;
    }

    /* ② 读部分码流，探测每条流的编码信息 */
    if (0 > avformat_find_stream_info(fmt_ctx, NULL)) {
        LOG_E("THUMB", "探测流信息失败");
        goto error;
    }

    /* ③ 挑出视频流索引 */
    stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO,
                                     -1, -1, NULL, 0);
    if (0 > stream_idx) {
        LOG_W("THUMB", "文件中无视频流");
        goto error;
    }

    /* ④ 按码流编码格式（如 H.264）找对应解码器 */
    codec = avcodec_find_decoder(
        fmt_ctx->streams[stream_idx]->codecpar->codec_id);
    if (NULL == codec) {
        LOG_E("THUMB", "找不到对应解码器");
        goto error;
    }

    /* ⑤ 建解码器上下文 → 复制流参数 → 打开解码器 */
    codec_ctx = avcodec_alloc_context3(codec);
    if (NULL == codec_ctx) {
        goto error;
    }
    if (0 > avcodec_parameters_to_context(
            codec_ctx, fmt_ctx->streams[stream_idx]->codecpar)) {
        goto error;
    }
    if (0 > avcodec_open2(codec_ctx, codec, NULL)) {
        LOG_E("THUMB", "打开解码器失败");
        goto error;
    }

    *out_fmt_ctx    = fmt_ctx;
    *out_codec_ctx  = codec_ctx;
    *out_stream_idx = stream_idx;
    return 0;

error:
    if (NULL != codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    avformat_close_input(&fmt_ctx);
    return -1;
}


/*****************************************************************************
 * 函数名称：decode_first_frame
 * 功能描述：从打开的视频流中解码出第一帧
 * 输入参数：@fmt_ctx    - 已打开的封装上下文
 *           @codec_ctx  - 已打开的解码器上下文
 *           @stream_idx - 视频流索引
 *           @frame      - 输出：解码得到的帧（调用方分配）
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：1. av_read_frame 读到的包是压缩数据，
 *              必须 send_packet → receive_frame 两步得到解码帧
 *           2. B 帧解码时 send 一个包可能 receive 不出帧（EAGAIN），
 *              必须循环喂包
 *           3. send_packet 后 pkt 数据已被解码器引用，可立即 unref；
 *              循环退出时残留的最后一个包由 av_packet_free 兜底释放
 *****************************************************************************/
static int decode_first_frame(AVFormatContext *fmt_ctx,
                              AVCodecContext *codec_ctx,
                              int stream_idx, AVFrame *frame)
{
    AVPacket *pkt = NULL;
    bool is_got_frame = false;
    int ret = -1;

    pkt = av_packet_alloc();
    if (NULL == pkt) {
        return -1;
    }

    /* 循环读包：视频包送解码器，音频等其它流的包直接丢弃 */
    while (0 <= av_read_frame(fmt_ctx, pkt)) {
        if (stream_idx == pkt->stream_index) {
            bool is_cfg_nal = false;

            /* 跳过配置/辅助 NAL（SPS/PPS/SEI/AUD）：崩溃恢复
             * 文件的 mdat 含流内 SPS/PPS 样本，与 extradata 的
             * avcC 重复会致新版解码器 "missing picture" 报错 */
            if (4 < pkt->size) {
                int nal_type = pkt->data[4] & 0x1F;

                is_cfg_nal = ((6 == nal_type) || (7 == nal_type) ||
                              (8 == nal_type) || (9 == nal_type));
            }
            if (false == is_cfg_nal) {
                ret = avcodec_send_packet(codec_ctx, pkt);
                if (0 > ret) {
                    break; /* 送包失败 */
                }

                ret = avcodec_receive_frame(codec_ctx, frame);
                if (0 == ret) {
                    is_got_frame = true;
                    break; /* 第一帧到手 */
                }
                if ((AVERROR(EAGAIN) != ret) && (AVERROR_EOF != ret)) {
                    break; /* 其他解码错误 */
                }
                if (AVERROR_EOF == ret) {
                    break; /* 码流结束仍未出帧 */
                }
                /* EAGAIN：解码器需要更多包，继续循环 */
            }
        }
        av_packet_unref(pkt);
    }

    /* 收尾：flush 解码器内部缓冲（B 帧延迟场景），挤最后一帧 */
    if ((false == is_got_frame) &&
        (0 <= avcodec_send_packet(codec_ctx, NULL)) &&
        (0 == avcodec_receive_frame(codec_ctx, frame))) {
        is_got_frame = true;
    }

    av_packet_free(&pkt);
    return is_got_frame ? 0 : -1;
}


/*****************************************************************************
 * 函数名称：scale_to_bgr
 * 功能描述：解码帧缩放转换为 BGR24（sws_scale 一步完成）
 * 输入参数：@src - 源帧（解码输出的 YUV 帧，格式/尺寸取帧内字段）
 *           @dst - 目标帧（需已设置 format/width/height 并分配内存）
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：sws_scale 返回输出平面高度，等于目标高度即转换成功
 *****************************************************************************/
static int scale_to_bgr(const AVFrame *src, AVFrame *dst)
{
    struct SwsContext *sws_ctx = NULL;
    int dst_h;

    /* 创建缩放上下文：源格式/尺寸 → BGR24/目标尺寸 */
    sws_ctx = sws_getContext(src->width, src->height,
                             (enum AVPixelFormat)src->format,
                             dst->width, dst->height, AV_PIX_FMT_BGR24,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (NULL == sws_ctx) {
        LOG_E("THUMB", "创建缩放上下文失败");
        return -1;
    }

    /* 执行转换（BGR24 是 BMP 原生像素顺序，落盘免二次转换） */
    dst_h = sws_scale(sws_ctx, (const uint8_t *const *)src->data,
                      src->linesize, 0, src->height,
                      dst->data, dst->linesize);
    sws_freeContext(sws_ctx);

    return (dst_h == dst->height) ? 0 : -1;
}


/*****************************************************************************
 * 函数名称：alloc_rgb_frame
 * 功能描述：分配 BGR24 目标帧（设置格式/尺寸并分配 32 字节对齐内存）
 * 输入参数：@width  - 目标宽度
 *           @height - 目标高度
 * 返回值：  成功返回帧指针（调用方负责 av_frame_free），失败返回 NULL
 *****************************************************************************/
static AVFrame *alloc_rgb_frame(int width, int height)
{
    AVFrame *frame_rgb = NULL;

    frame_rgb = av_frame_alloc();
    if (NULL == frame_rgb) {
        return NULL;
    }

    frame_rgb->format = AV_PIX_FMT_BGR24;
    frame_rgb->width  = width;
    frame_rgb->height = height;
    if (0 > av_frame_get_buffer(frame_rgb, 32)) {
        LOG_E("THUMB", "分配帧内存失败");
        av_frame_free(&frame_rgb);
        return NULL;
    }

    return frame_rgb;
}


/*****************************************************************************
 * 函数名称：thumb_gen_from_video
 * 功能描述：解码视频文件的第一个视频帧，缩放后保存为 BMP 缩略图
 * 输入参数：@src_path - 源视频路径
 *           @bmp_path - 缩略图输出路径
 *           @width    - 缩略图宽度
 *           @height   - 缩略图高度
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：错误路径 goto cleanup 集中释放，顺序与申请相反
 *****************************************************************************/
int thumb_gen_from_video(const char *src_path, const char *bmp_path,
                         int width, int height)
{
    AVFormatContext *fmt_ctx  = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVFrame *frame            = NULL;
    AVFrame *frame_rgb        = NULL;
    int stream_idx            = -1;
    int ret                   = -1;

    /* 参数校验（含尺寸上限，防 int 运算溢出） */
    if ((NULL == src_path) || (NULL == bmp_path) ||
        (0 >= width) || (0 >= height) ||
        (THUMB_MAX_DIMENSION < width) || (THUMB_MAX_DIMENSION < height)) {
        LOG_E("THUMB", "无效参数");
        return -1;
    }

    /* ①~⑤ 打开文件 + 打开视频解码器 */
    if (0 != open_video_decode(src_path, &fmt_ctx, &codec_ctx, &stream_idx)) {
        return -1;
    }

    /* ⑥ 解出第一帧 */
    frame = av_frame_alloc();
    if (NULL == frame) {
        goto cleanup;
    }
    if (0 != decode_first_frame(fmt_ctx, codec_ctx, stream_idx, frame)) {
        LOG_W("THUMB", "解码第一帧失败");
        goto cleanup;
    }

    /* ⑦ 目标帧：BGR24 + 目标尺寸，分配 32 字节对齐像素内存 */
    frame_rgb = alloc_rgb_frame(width, height);
    if (NULL == frame_rgb) {
        goto cleanup;
    }

    /* ⑧ YUV → BGR24 缩放 */
    if (0 != scale_to_bgr(frame, frame_rgb)) {
        goto cleanup;
    }

    /* ⑨ BMP 落盘（linesize 可能大于 width*3，write_bmp 内部逐行处理） */
    if (0 != write_bmp(bmp_path, frame_rgb->data[0],
                       frame_rgb->linesize[0], width, height)) {
        goto cleanup;
    }

    ret = 0;

cleanup:
    /* 释放顺序与申请相反 */
    if (NULL != frame_rgb) {
        av_frame_free(&frame_rgb);
    }
    if (NULL != frame) {
        av_frame_free(&frame);
    }
    if (NULL != codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    if (NULL != fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    return ret;
}
