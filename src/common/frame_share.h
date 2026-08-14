/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：frame_share.h
 * 文件功能：跨线程帧共享缓冲 —— AI 线程写、UI 线程读（互斥锁保护）
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#ifndef FRAME_SHARE_H
#define FRAME_SHARE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* 显示分辨率：全屏 1280×720（与 AI 采集源 1280×720 一致，零缩放） */
#define FRAME_SHARE_WIDTH   1280
#define FRAME_SHARE_HEIGHT  720
/* 像素格式 BGRA，4 字节/像素——LV_COLOR_DEPTH=32 时 LVGL
 * LV_IMG_CF_TRUE_COLOR 的 lv_color_t 即 4 字节（内存序 B,G,R,A），
 * 两者一致可直接拷贝到 canvas 缓冲 */
#define FRAME_SHARE_BPP     4
#define FRAME_SHARE_SIZE    (FRAME_SHARE_WIDTH * FRAME_SHARE_HEIGHT * FRAME_SHARE_BPP)

/* 帧共享缓冲 */
typedef struct {
    pthread_mutex_t mutex;              /* 读写锁 */
    volatile bool   updated;            /* AI 写完新帧置 true，UI 读后清 false */
    uint8_t         buf[FRAME_SHARE_SIZE];
} frame_share_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：frame_share_init
 * 功能描述：初始化互斥锁和缓冲
 * 输入参数：@fs - 共享缓冲指针
 *****************************************************************************/
void frame_share_init(frame_share_t *fs);

/*****************************************************************************
 * 函数名称：frame_share_push
 * 功能描述：AI 线程写入新帧（调用前应完成缩放/格式转换，锁内仅拷贝）
 * 输入参数：@fs  - 共享缓冲指针
 *           @rgb - FRAME_SHARE_SIZE 字节的 BGRA 数据
 *****************************************************************************/
void frame_share_push(frame_share_t *fs, const uint8_t *bgra);

/*****************************************************************************
 * 函数名称：frame_share_pop
 * 功能描述：UI 线程读取新帧
 * 输入参数：@fs  - 共享缓冲指针
 * 输出参数：@out - 拷贝出的帧数据（调用者分配 FRAME_SHARE_SIZE）
 * 返回值：  有新帧返回 true，无新帧返回 false
 *****************************************************************************/
bool frame_share_pop(frame_share_t *fs, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_SHARE_H */
