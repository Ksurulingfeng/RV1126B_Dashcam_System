/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：video_recover.c
 * 文件功能：断电残留录像恢复 —— 扫描 mdat 中 H264 AVCC 流，
 *           libavformat 重建 moov 写出标准 MP4
 * 作    者：heifast
 * 创建日期：2026-08-17
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>

#include <libavformat/avformat.h>

#include "log.h"
#include "video_recover.h"

/* 路径缓冲统一长度（与 file_mgr.h FILE_PATH_MAX 保持一致） */
#define RECOVER_PATH_MAX 512

/* NAL 类型合法性位图：1 非IDR / 5 IDR / 6 SEI / 7 SPS / 8 PPS / 9 AUD */
#define NAL_TYPE_MASK \
    ((1u << 1) | (1u << 5) | (1u << 6) | \
     (1u << 7) | (1u << 8) | (1u << 9))

/* 单个 NAL 长度合理范围（字节） */
#define NAL_MIN_LEN 2
#define NAL_MAX_LEN (4u * 1024 * 1024)

/* moov 探测窗口（字节）：与 file_mgr 封口探测窗口一致。
 * 头尾各探测一次——qtmux（splitmuxsink 原生）moov 在文件尾，
 * FFmpeg 重建（恢复产物）moov 在文件头 */
#define MOOV_PROBE_SIZE (128u * 1024)

/* 恢复文件体积上限（字节）：超出者非本项目分段产物，拒绝恢复 */
#define RECOVER_MAX_SIZE (512u * 1024 * 1024)

/* SPS 解析失败时的分辨率兜底（本项目固定 1080p 编码配置） */
#define RECOVER_FALLBACK_W 1920
#define RECOVER_FALLBACK_H 1080

/* 收集到的 NAL 三元组（指向文件缓冲内原数据，AVCC 格式） */
typedef struct {
    const uint8_t *data; /* NAL 数据（不含 4 字节长度前缀） */
    uint32_t      size;
    uint8_t       type;
} nal_item_t;

/* SPS/PPS 实际长度上限（字节）：真实 SPS 远小于此，
 * 超长的"类型 7/8 NAL"必是音频区伪命中 */
#define SPS_MAX_LEN 256
#define PPS_MAX_LEN 64

/* NAL 收集动态数组 */
typedef struct {
    nal_item_t *items;
    int         count;
    int         capacity;
} nal_list_t;

/* Exp-Golomb 位读取器 */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int bit_pos;
} bit_reader_t;

/* NAL 类型合法性判断（位图查表） */
#define nal_type_valid(t) \
    (((t) < 32) && (0 != (NAL_TYPE_MASK & (1u << (t)))))

/*****************************************************************************
 * 函数名称：moov_probe
 * 功能描述：探测文件是否已封口（头/尾窗口含 moov 魔数）
 * 输入参数：@filepath - 文件完整路径
 * 返回值：  已封口返回 true
 * 注意事项：语义与 file_mgr 的 file_is_sealed 对齐；恢复场景中
 *           文件必然无 moov，探测命中即跳过（完好文件）
 *****************************************************************************/
static bool moov_probe(const char *filepath)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    long file_size;
    long region_off;
    long region_size;
    int pass;
    bool sealed = false;

    fp = fopen(filepath, "rb");
    if (NULL == fp) {
        return false;
    }
    if (0 != fseek(fp, 0, SEEK_END)) {
        goto done;
    }
    file_size = ftell(fp);
    if (8 > file_size) {
        goto done;
    }

    buf = (uint8_t *)malloc(MOOV_PROBE_SIZE);
    if (NULL == buf) {
        goto done;
    }

    /* 头尾两窗口：qtmux 封口 moov 在尾，FFmpeg 重建 moov 在头 */
    for (pass = 0; pass < 2; pass++) {
        long i;

        if (0 == pass) {
            region_off = 0;
            region_size = (file_size < (long)MOOV_PROBE_SIZE) ?
                          file_size : (long)MOOV_PROBE_SIZE;
        } else {
            region_off = file_size - (long)MOOV_PROBE_SIZE;
            region_size = (long)MOOV_PROBE_SIZE;
            if (0 > region_off) {
                break; /* 小文件：头窗口已覆盖全文件 */
            }
        }
        if (0 != fseek(fp, region_off, SEEK_SET)) {
            goto done;
        }
        if (region_size != (long)fread(buf, 1, (size_t)region_size, fp)) {
            goto done;
        }
        for (i = 0; i + 4 <= region_size; i++) {
            if (0 == memcmp(buf + i, "moov", 4)) {
                sealed = true;
                break;
            }
        }
        if (sealed) {
            break;
        }
    }

done:
    free(buf);
    fclose(fp);
    return sealed;
}


/*****************************************************************************
 * 函数名称：rbsp_copy
 * 功能描述：去除 H264 仿真预防字节（0x000003 → 0x0000）
 * 输入参数：@src - 原始 SPS 数据
 *           @len - 原始长度
 * 输出参数：@dst - RBSP 缓冲（调用方保证足够大）
 * 返回值：  RBSP 长度
 *****************************************************************************/
static uint32_t rbsp_copy(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    uint32_t i;
    uint32_t j = 0;

    for (i = 0; i < len; i++) {
        if ((i < 2) || (0 != src[i]) || (0 != src[i - 1]) ||
            (3 != src[i - 2])) {
            dst[j++] = src[i];
        }
    }
    return j;
}


/*****************************************************************************
 * 函数名称：bs_read_bits
 * 功能描述：读取 n 位（≤24），无符号扩展
 * 输入参数：@bs - 位读取器
 *           @n  - 位数
 * 返回值：  读取的位值
 *****************************************************************************/
static uint32_t bs_read_bits(bit_reader_t *bs, int n)
{
    uint32_t val = 0;
    int i;

    for (i = 0; i < n; i++) {
        uint8_t byte;
        uint8_t bit;

        if (bs->p >= bs->end) {
            return val; /* 越界防御：缺位按 0 处理 */
        }
        byte = *(bs->p);
        bit = (byte >> (7 - bs->bit_pos)) & 1;
        val = (val << 1) | bit;
        bs->bit_pos++;
        if (8 == bs->bit_pos) {
            bs->bit_pos = 0;
            bs->p++;
        }
    }
    return val;
}


/*****************************************************************************
 * 函数名称：bs_read_ue
 * 功能描述：读取无符号 Exp-Golomb 编码值
 * 输入参数：@bs - 位读取器
 * 返回值：  解码值
 *****************************************************************************/
static uint32_t bs_read_ue(bit_reader_t *bs)
{
    int zeros = 0;

    while (0 == bs_read_bits(bs, 1)) {
        zeros++;
        if (31 <= zeros) {
            return 0; /* 防损坏数据死循环（连续零超长） */
        }
    }
    return (1u << zeros) - 1 + bs_read_bits(bs, zeros);
}


/*****************************************************************************
 * 函数名称：sps_parse_dimensions
 * 功能描述：解析 SPS 获取画面宽高（跳过 VUI）
 * 输入参数：@sps - SPS NAL 数据（不含起始码，允许含仿真预防字节）
 *           @len - SPS 长度
 * 输出参数：@width/@height - 解析结果
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
static int sps_parse_dimensions(const uint8_t *sps, uint32_t len,
                                int *width, int *height)
{
    uint8_t rbsp[256];
    uint32_t rbsp_len;
    bit_reader_t bs;
    uint32_t profile_idc;
    uint32_t frame_mbs_only;
    uint32_t pic_w_mbs;
    uint32_t pic_h_units;
    uint32_t crop_left = 0;
    uint32_t crop_right = 0;
    uint32_t crop_top = 0;
    uint32_t crop_bottom = 0;
    uint32_t crop_unit_x;
    uint32_t crop_unit_y;

    /* 长度防御：真实 SPS 远小于 256 字节，超长为伪命中 */
    if (SPS_MAX_LEN < len) {
        return -1;
    }

    /* 去掉 NAL 头字节 + 仿真预防字节 */
    rbsp_len = rbsp_copy(rbsp, sps + 1, len - 1);
    bs.p = rbsp;
    bs.end = rbsp + rbsp_len;
    bs.bit_pos = 0;

    profile_idc = bs_read_bits(&bs, 8);
    bs_read_bits(&bs, 8);  /* constraint flags */
    bs_read_bits(&bs, 8);  /* level_idc */
    bs_read_ue(&bs);       /* seq_parameter_set_id */

    /* High 系 profile：跳过色度/位深/量化矩阵配置 */
    if ((100 == profile_idc) || (110 == profile_idc) ||
        (122 == profile_idc) || (244 == profile_idc) ||
        (44 == profile_idc) || (83 == profile_idc) ||
        (86 == profile_idc) || (118 == profile_idc) ||
        (128 == profile_idc) || (138 == profile_idc) ||
        (139 == profile_idc) || (134 == profile_idc) ||
        (135 == profile_idc)) {
        uint32_t chroma_idc = bs_read_ue(&bs);

        if (3 == chroma_idc) {
            bs_read_bits(&bs, 1); /* separate_colour_plane_flag */
        }
        bs_read_ue(&bs); /* bit_depth_luma_minus8 */
        bs_read_ue(&bs); /* bit_depth_chroma_minus8 */
        bs_read_bits(&bs, 1); /* qpprime_y_zero_transform_bypass_flag */
        if (bs_read_bits(&bs, 1)) {
            /* seq_scaling_matrix_present_flag：跳过缩放矩阵 */
            int idx;
            for (idx = 0; idx < 8; idx++) {
                if (bs_read_bits(&bs, 1)) {
                    int last = 8;
                    int next = 8;
                    int j;
                    for (j = 0; j < 64; j++) {
                        if (0 != next) {
                            next = (int)bs_read_ue(&bs) + 1;
                        }
                        if (0 != next) {
                            last = next;
                        }
                        (void)last;
                    }
                }
            }
        }
    }

    bs_read_ue(&bs); /* log2_max_frame_num_minus4 */
    {
        uint32_t poc_type = bs_read_ue(&bs);
        if (0 == poc_type) {
            bs_read_ue(&bs); /* log2_max_pic_order_cnt_lsb_minus4 */
        } else if (1 == poc_type) {
            uint32_t i;
            uint32_t n;

            bs_read_bits(&bs, 1); /* delta_pic_order_always_zero_flag */
            bs_read_ue(&bs);      /* offset_for_non_ref_pic（se 与 ue 同宽） */
            bs_read_ue(&bs);      /* offset_for_top_to_bottom_field */
            n = bs_read_ue(&bs);  /* num_ref_frames_in_pic_order_cnt_cycle */
            for (i = 0; i < n; i++) {
                bs_read_ue(&bs);
            }
        }
    }
    bs_read_ue(&bs); /* max_num_ref_frames */
    bs_read_bits(&bs, 1); /* gaps_in_frame_num_value_allowed_flag */
    pic_w_mbs = bs_read_ue(&bs) + 1;
    pic_h_units = bs_read_ue(&bs) + 1;
    frame_mbs_only = bs_read_bits(&bs, 1);
    if (0 == frame_mbs_only) {
        bs_read_bits(&bs, 1); /* mb_adaptive_frame_field_flag */
    }
    bs_read_bits(&bs, 1); /* direct_8x8_inference_flag */
    if (bs_read_bits(&bs, 1)) {
        crop_left = bs_read_ue(&bs);
        crop_right = bs_read_ue(&bs);
        crop_top = bs_read_ue(&bs);
        crop_bottom = bs_read_ue(&bs);
    }

    crop_unit_x = (0 == frame_mbs_only) ? 2 : 1;
    crop_unit_y = 2 - frame_mbs_only;
    *width = (int)(pic_w_mbs * 16 -
                   (crop_left + crop_right) * crop_unit_x);
    *height = (int)((2 - frame_mbs_only) * pic_h_units * 16 -
                    (crop_top + crop_bottom) * crop_unit_y);
    return 0;
}


/*****************************************************************************
 * 函数名称：avcc_build
 * 功能描述：构造 avcC extradata（版本1 + SPS/PPS）
 * 输入参数：@sps/@sps_len - SPS 数据与长度（含 NAL 头）
 *           @pps/@pps_len - PPS 数据与长度（含 NAL 头）
 * 输出参数：@out - 输出缓冲（调用方保证 ≥ 7+sps_len+2+pps_len 字节）
 * 返回值：  avcC 总长度
 *****************************************************************************/
static uint32_t avcc_build(uint8_t *out, const uint8_t *sps,
                           uint32_t sps_len, const uint8_t *pps,
                           uint32_t pps_len)
{
    uint32_t off = 0;

    out[off++] = 1;                        /* configurationVersion */
    out[off++] = sps[1];                   /* AVCProfileIndication */
    out[off++] = sps[2];                   /* profile_compatibility */
    out[off++] = sps[3];                   /* AVCLevelIndication */
    out[off++] = 0xFC | 3;                 /* 长度前缀 4 字节 */
    out[off++] = 0xE0 | 1;                 /* numOfSequenceParameterSets=1 */
    out[off++] = (uint8_t)(sps_len >> 8);
    out[off++] = (uint8_t)sps_len;
    memcpy(out + off, sps, sps_len);
    off += sps_len;
    out[off++] = 1;                        /* numOfPictureParameterSets=1 */
    out[off++] = (uint8_t)(pps_len >> 8);
    out[off++] = (uint8_t)pps_len;
    memcpy(out + off, pps, pps_len);
    off += pps_len;
    return off;
}


/*****************************************************************************
 * 函数名称：mp4_write_file
 * 功能描述：用 libavformat 把收集的 NAL 流重封装为标准 MP4
 * 输入参数：@out_path - 输出路径（临时文件，成功后调用方 rename）
 *           @list     - NAL 列表（data 指向 [4字节长度][NAL] 前缀处）
 *           @width/@height - 画面宽高（SPS 解析或兜底值）
 *           @extradata/@extradata_size - avcC 解码配置
 *           @fps - 帧率（时间戳均匀递增）
 * 输出参数：@out_frames - 输出的视频帧数
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：mdat 中 AVCC 格式 [4字节长度][NAL] 与 mp4 muxer
 *           期望的包格式完全一致，pkt 直接指向原缓冲零拷贝
 *****************************************************************************/
static int mp4_write_file(const char *out_path, const nal_list_t *list,
                          int width, int height,
                          const uint8_t *extradata, int extradata_size,
                          uint32_t fps, int *out_frames)
{
    AVFormatContext *ctx = NULL;
    AVStream *st = NULL;
    AVCodecParameters *par = NULL;
    AVPacket *pkt = NULL;
    int frames = 0;
    int i;
    int ret = -1;

    /* 显式指定 mp4 格式：临时文件名带 .recover 后缀，自动推断会失败 */
    if (0 > avformat_alloc_output_context2(&ctx, NULL, "mp4", out_path)) {
        return -1;
    }
    st = avformat_new_stream(ctx, NULL);
    if (NULL == st) {
        goto done;
    }
    par = st->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = width;
    par->height = height;
    if (0 < extradata_size) {
        par->extradata = (uint8_t *)av_mallocz(
            (size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (NULL == par->extradata) {
            goto done;
        }
        memcpy(par->extradata, extradata, (size_t)extradata_size);
        par->extradata_size = extradata_size;
    }
    st->time_base.num = 1;
    st->time_base.den = (int)fps;

    if (0 > avio_open(&ctx->pb, out_path, AVIO_FLAG_WRITE)) {
        goto done;
    }
    if (0 > avformat_write_header(ctx, NULL)) {
        goto done;
    }

    pkt = av_packet_alloc();
    if (NULL == pkt) {
        goto done;
    }
    for (i = 0; i < list->count; i++) {
        const nal_item_t *item = &list->items[i];

        /* pkt.data 指向原缓冲的长度前缀处：[4字节长度][NAL] */
        pkt->data = (uint8_t *)item->data - 4;
        pkt->size = (int)item->size + 4;
        pkt->stream_index = st->index;
        pkt->pts = frames;
        pkt->dts = frames;
        if (5 == item->type) {
            pkt->flags = AV_PKT_FLAG_KEY;
        } else {
            pkt->flags = 0;
        }
        if (0 > av_interleaved_write_frame(ctx, pkt)) {
            av_packet_unref(pkt);
            goto done;
        }
        av_packet_unref(pkt);
        if ((1 == item->type) || (5 == item->type)) {
            frames++;
        }
    }

    if (0 > av_write_trailer(ctx)) {
        goto done;
    }
    *out_frames = frames;
    ret = 0;

done:
    if (NULL != pkt) {
        av_packet_free(&pkt);
    }
    if (NULL != ctx) {
        avio_closep(&ctx->pb);
        avformat_free_context(ctx);
    }
    return ret;
}


/*****************************************************************************
 * 函数名称：nal_peek_valid
 * 功能描述：检查 p 处是否为合法 NAL 头（4 字节长度前缀 + 合法类型）
 * 输入参数：@p      - 待检查位置
 *           @remain - 剩余字节数
 * 输出参数：@len/@type - 命中时的 NAL 长度与类型
 * 返回值：  合法返回 true
 *****************************************************************************/
static bool nal_peek_valid(const uint8_t *p, size_t remain,
                           uint32_t *len, uint8_t *type)
{
    uint32_t nal_len;

    if (remain < 8) {
        return false;
    }
    nal_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
              ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if ((NAL_MIN_LEN > nal_len) || (NAL_MAX_LEN < nal_len) ||
        ((size_t)nal_len + 4 > remain)) {
        return false;
    }
    if (!nal_type_valid(p[4] & 0x1F)) {
        return false;
    }
    *len = nal_len;
    *type = p[4] & 0x1F;
    return true;
}


/*****************************************************************************
 * 函数名称：nal_scan
 * 功能描述：容错扫描 mdat 中交错存储的 H264 流，收集全部视频 NAL
 * 输入参数：@base - mdat payload 起点
 *           @size - payload 大小
 * 输出参数：@list - NAL 列表（动态扩容）
 * 注意事项：mdat 中视频 chunk（AVCC 长度前缀）与音频 chunk
 *           （raw AAC，无索引不可恢复）交错。音频数据的前 4 字节
 *           通常超出 NAL 合法长度域，逐字节滑动即可跳过；
 *           合法 NAL 段连续收集，遇非法处回落到滑动模式
 *****************************************************************************/
static void nal_scan(const uint8_t *base, size_t size, nal_list_t *list)
{
    size_t pos = 0;

    while (pos < size) {
        uint32_t nal_len;
        uint8_t nal_type;
        uint32_t next_len;
        uint8_t next_type;

        /* 连续两帧验证：音频区可能单点伪命中（4 字节长度恰好
         * 合法 + 类型字节巧合），要求紧跟的下一个 NAL 也合法
         * 才进入收集（真视频 chunk 至少含一个 GOP） */
        if (nal_peek_valid(base + pos, size - pos, &nal_len, &nal_type) &&
            nal_peek_valid(base + pos + 4 + nal_len,
                           size - pos - 4 - nal_len,
                           &next_len, &next_type)) {
            /* 连续合法段：逐条收集直到非法边界
             * （chunk 末尾 NAL 由单条验证保留，不丢帧） */
            while (pos < size) {
                if (list->count >= list->capacity) {
                    nal_item_t *tmp;
                    int new_cap = list->capacity * 2;

                    tmp = (nal_item_t *)realloc(list->items,
                        sizeof(nal_item_t) * (size_t)new_cap);
                    if (NULL == tmp) {
                        LOG_W("REC", "NAL 列表扩容失败，截断恢复");
                        return;
                    }
                    list->items = tmp;
                    list->capacity = new_cap;
                }
                if (!nal_peek_valid(base + pos, size - pos,
                                    &nal_len, &nal_type)) {
                    break;
                }
                list->items[list->count].data = base + pos + 4;
                list->items[list->count].size = nal_len;
                list->items[list->count].type = nal_type;
                list->count++;
                pos += (size_t)nal_len + 4;
            }
        } else {
            pos++; /* 音频区/损坏字节：逐字节滑动 */
        }
    }
}


/*****************************************************************************
 * 函数名称：mdat_locate
 * 功能描述：解析顶层 box 定位 mdat payload 起点
 * 输入参数：@buf - 文件缓冲
 *           @size - 文件大小
 * 输出参数：@payload/@payload_size - mdat payload 位置与长度
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：mp4mux 对未知大小的 mdat 写 size=0（box 延伸到 EOF）
 *****************************************************************************/
static int mdat_locate(const uint8_t *buf, size_t size,
                       const uint8_t **payload, size_t *payload_size)
{
    size_t pos = 0;

    while (pos + 8 <= size) {
        uint64_t box_size = ((uint64_t)buf[pos] << 24) |
                            ((uint64_t)buf[pos + 1] << 16) |
                            ((uint64_t)buf[pos + 2] << 8) |
                            (uint64_t)buf[pos + 3];
        uint8_t type[4];

        memcpy(type, buf + pos + 4, 4);
        if (0 == box_size) {
            box_size = size - pos; /* 延伸到文件尾 */
        } else if (1 == box_size) {
            if (pos + 16 > size) {
                return -1;
            }
            box_size = ((uint64_t)buf[pos + 8] << 56) |
                       ((uint64_t)buf[pos + 9] << 48) |
                       ((uint64_t)buf[pos + 10] << 40) |
                       ((uint64_t)buf[pos + 11] << 32) |
                       ((uint64_t)buf[pos + 12] << 24) |
                       ((uint64_t)buf[pos + 13] << 16) |
                       ((uint64_t)buf[pos + 14] << 8) |
                       (uint64_t)buf[pos + 15];
        }
        if (box_size > size - pos) {
            return -1;
        }
        if (0 == memcmp(type, "mdat", 4)) {
            *payload = buf + pos + 8;
            *payload_size = (size_t)box_size - 8;
            return 0;
        }
        pos += (size_t)box_size;
    }
    return -1;
}


/*****************************************************************************
 * 函数名称：find_avcc_from_peer
 * 功能描述：从同目录完好文件提取 avcC（H264 解码配置）
 * 输入参数：@dir - 目录路径
 * 输出参数：@out - 输出缓冲（调用方分配）
 *           @out_size - 缓冲大小
 * 返回值：  avcC 长度（>0），未找到返回 0
 * 注意事项：断电残留流的 mdat 中不含 SPS/PPS 样本（qtmux 把它们
 *           写在文件头 avcC 里），需借用同目录完好文件的解码配置
 *           ——同管线产物配置完全一致（untrunc 同思路）
 *****************************************************************************/
static int find_avcc_from_peer(const char *dir, uint8_t *out, int out_size)
{
    DIR *d = NULL;
    struct dirent *entry = NULL;
    int avcc_size = 0;

    d = opendir(dir);
    if (NULL == d) {
        return 0;
    }

    while (NULL != (entry = readdir(d))) {
        char fullpath[RECOVER_PATH_MAX];
        char *dot = strrchr(entry->d_name, '.');

        if ((NULL != dot) && (0 == strcmp(dot, ".mp4"))) {
            AVFormatContext *ictx = NULL;
            int stream_idx;

            snprintf(fullpath, sizeof(fullpath), "%s/%s",
                     dir, entry->d_name);
            if (moov_probe(fullpath) &&
                (0 == avformat_open_input(&ictx, fullpath,
                                          NULL, NULL))) {
                stream_idx = av_find_best_stream(ictx,
                    AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
                if (0 <= stream_idx) {
                    const AVCodecParameters *par =
                        ictx->streams[stream_idx]->codecpar;

                    if ((NULL != par->extradata) &&
                        (0 < par->extradata_size) &&
                        (par->extradata_size <= out_size)) {
                        memcpy(out, par->extradata,
                               (size_t)par->extradata_size);
                        avcc_size = par->extradata_size;
                    }
                }
                avformat_close_input(&ictx);
            }
            if (0 < avcc_size) {
                break;
            }
        }
    }
    closedir(d);
    return avcc_size;
}


/*****************************************************************************
 * 函数名称：video_recover_file
 * 功能描述：恢复单个断电残留文件（探测 → 扫描 → 重建 → 原子替换）
 * 输入参数：@path - 残留文件完整路径
 *           @fps  - 视频帧率
 * 返回值：  成功返回恢复的视频帧数（>0）；已封口返回 0；失败返回 -1
 * 注意事项：临时文件 path.recover，成功后 rename 覆盖；
 *           SPS 解析失败用 1080p 兜底分辨率
 *****************************************************************************/
int video_recover_file(const char *path, uint32_t fps)
{
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    long file_size;
    const uint8_t *payload = NULL;
    size_t payload_size;
    nal_list_t list = {0}; /* 零初始化：早退路径 free(items) 安全 */
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    uint32_t sps_len = 0;
    uint32_t pps_len = 0;
    int width = RECOVER_FALLBACK_W;
    int height = RECOVER_FALLBACK_H;
    char tmp_path[RECOVER_PATH_MAX];
    int frames = 0;
    int i;
    int ret = -1;

    if (moov_probe(path)) {
        return 0; /* 完好文件，无需恢复 */
    }
    fp = fopen(path, "rb");
    if (NULL == fp) {
        return -1;
    }
    if (0 != fseek(fp, 0, SEEK_END)) {
        goto done;
    }
    file_size = ftell(fp);
    if ((RECOVER_MIN_SIZE > file_size) ||
        ((long)RECOVER_MAX_SIZE < file_size)) {
        goto done;
    }
    if (0 != fseek(fp, 0, SEEK_SET)) {
        goto done;
    }
    buf = (uint8_t *)malloc((size_t)file_size);
    if (NULL == buf) {
        LOG_W("REC", "恢复缓冲分配失败（%ld 字节）", file_size);
        goto done;
    }
    if (file_size != (long)fread(buf, 1, (size_t)file_size, fp)) {
        goto done;
    }

    if (0 != mdat_locate(buf, (size_t)file_size, &payload, &payload_size)) {
        LOG_W("REC", "mdat 定位失败: %s", path);
        goto done;
    }

    memset(&list, 0, sizeof(list));
    list.capacity = 1024;
    list.items = (nal_item_t *)malloc(sizeof(nal_item_t) *
                                      (size_t)list.capacity);
    if (NULL == list.items) {
        goto done;
    }
    nal_scan(payload, payload_size, &list);
    if (0 == list.count) {
        LOG_W("REC", "未扫描到视频数据: %s", path);
        goto done;
    }

    /* 收集 SPS/PPS（avcC 构造用；缺 PPS 时放弃 extradata）。
     * 长度过滤：真实的 SPS/PPS 远小于上限，超长的"类型 7/8"
     * 必是音频区伪命中，跳过继续找 */
    for (i = 0; i < list.count; i++) {
        if ((7 == list.items[i].type) && (NULL == sps) &&
            (SPS_MAX_LEN >= list.items[i].size)) {
            sps = list.items[i].data;
            sps_len = list.items[i].size;
        }
        if ((8 == list.items[i].type) && (NULL == pps) &&
            (PPS_MAX_LEN >= list.items[i].size)) {
            pps = list.items[i].data;
            pps_len = list.items[i].size;
        }
        if ((NULL != sps) && (NULL != pps)) {
            break;
        }
    }
    if ((NULL != sps) && (sps_len > 4)) {
        (void)sps_parse_dimensions(sps, sps_len, &width, &height);
    }

    /* avcC 提取：优先从流内 SPS/PPS 构建；断电流的 mdat 不含
     * SPS/PPS 样本，借用同目录完好文件的解码配置（untrunc 思路） */
    {
        uint8_t extradata_buf[256];
        int extradata_size = 0;

        if ((NULL != sps) && (NULL != pps)) {
            extradata_size = (int)avcc_build(extradata_buf,
                                             sps, sps_len, pps, pps_len);
        } else {
            char dir_buf[RECOVER_PATH_MAX];
            const char *slash = strrchr(path, '/');

            if (NULL != slash) {
                size_t dir_len = (size_t)(slash - path);

                if (dir_len >= sizeof(dir_buf)) {
                    dir_len = sizeof(dir_buf) - 1;
                }
                memcpy(dir_buf, path, dir_len);
                dir_buf[dir_len] = '\0';
            } else {
                dir_buf[0] = '.';
                dir_buf[1] = '\0';
            }
            extradata_size = find_avcc_from_peer(dir_buf,
                extradata_buf, (int)sizeof(extradata_buf));
            if (0 >= extradata_size) {
                LOG_W("REC", "无参考 avcC（目录无完好文件），"
                      "恢复文件可能不可播: %s", path);
            }
        }

        snprintf(tmp_path, sizeof(tmp_path), "%s.recover", path);
        if (0 != mp4_write_file(tmp_path, &list, width, height,
                                extradata_buf, extradata_size,
                                fps, &frames)) {
            LOG_E("REC", "MP4 重建失败: %s", path);
            unlink(tmp_path);
            goto done;
        }
    }
    if (0 != rename(tmp_path, path)) {
        LOG_E("REC", "恢复文件替换失败: %s", path);
        unlink(tmp_path);
        goto done;
    }

    LOG_I("REC", "恢复 %s → %d 帧视频（%dx%d）",
          path, frames, width, height);
    ret = frames;

done:
    free(list.items);
    free(buf);
    fclose(fp);
    return ret;
}


/*****************************************************************************
 * 函数名称：video_recover_scan_dir
 * 功能描述：扫描目录，恢复全部断电残留文件
 * 输入参数：@dir - 录像目录
 *           @fps - 视频帧率
 * 返回值：  成功恢复的文件数（≥0），扫描失败返回 -1
 * 注意事项：供启动流程在编码器启动前调用
 *****************************************************************************/
int video_recover_scan_dir(const char *dir, uint32_t fps)
{
    DIR *d = NULL;
    struct dirent *entry = NULL;
    int recovered = 0;

    d = opendir(dir);
    if (NULL == d) {
        return -1;
    }

    while (NULL != (entry = readdir(d))) {
        char fullpath[RECOVER_PATH_MAX];
        char *dot = strrchr(entry->d_name, '.');

        if ((NULL != dot) && (0 == strcmp(dot, ".mp4"))) {
            snprintf(fullpath, sizeof(fullpath), "%s/%s",
                     dir, entry->d_name);
            if (0 < video_recover_file(fullpath, fps)) {
                recovered++;
            }
        }
    }
    closedir(d);

    if (0 < recovered) {
        LOG_I("REC", "断电恢复完成：%d 个文件", recovered);
    }
    return recovered;
}
