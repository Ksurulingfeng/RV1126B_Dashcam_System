/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：preview_share.h
 * 文件功能：预览帧共享 —— GStreamer appsink 写、多消费者读（NV12）
 *           UI 线程（显示）与 AI 线程（推理）各自独立取帧
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#ifndef PREVIEW_SHARE_H
#define PREVIEW_SHARE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* 预览帧尺寸（主路缩放后，与录像同源同视野） */
#define PREVIEW_WIDTH  1280
#define PREVIEW_HEIGHT 720

/* NV12 缓冲大小：Y 平面 W×H + UV 交错平面 W×H/2 */
#define PREVIEW_SIZE ((PREVIEW_WIDTH) * (PREVIEW_HEIGHT) * 3 / 2)

/* 预览帧共享缓冲（互斥锁保护，多消费者模式） */
typedef struct {
    pthread_mutex_t mutex;
    uint32_t        frame_id; /* 帧序号：push 递增，消费者按序号判断新帧 */
    uint8_t         buf[PREVIEW_SIZE];
} preview_share_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：preview_share_init
 * 功能描述：初始化互斥锁和缓冲
 * 输入参数：@ps - 共享缓冲指针
 *****************************************************************************/
void preview_share_init(preview_share_t *ps);

/*****************************************************************************
 * 函数名称：preview_share_push
 * 功能描述：GStreamer appsink 回调写入新帧（锁内拷贝，帧序号递增）
 * 输入参数：@ps  - 共享缓冲指针
 *           @nv12 - PREVIEW_SIZE 字节的 NV12 数据
 *****************************************************************************/
void preview_share_push(preview_share_t *ps, const uint8_t *nv12);

/*****************************************************************************
 * 函数名称：preview_share_pop
 * 功能描述：读取最新帧（多消费者安全：各消费者维护自己的 frame_id）
 * 输入参数：@ps       - 共享缓冲指针
 * 输出参数：@out      - 拷贝出的 NV12 数据（调用者分配 PREVIEW_SIZE）
 *           @frame_id - 输入：本消费者上次读到的序号；
 *                       输出：更新为本次读到的序号
 * 返回值：  有新帧返回 true，无新帧返回 false
 *****************************************************************************/
bool preview_share_pop(preview_share_t *ps, uint8_t *out,
                       uint32_t *frame_id);

#ifdef __cplusplus
}
#endif

#endif /* PREVIEW_SHARE_H */
