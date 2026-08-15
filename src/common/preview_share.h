/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：preview_share.h
 * 文件功能：预览帧共享 —— GStreamer appsink 写、多消费者读（格式无关）
 *           NV12 实例（AI 推理）与 BGRA 实例（UI 显示）共用本模块
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

/* NV12 帧字节数：Y 平面 W×H + UV 交错平面 W×H/2 */
#define PREVIEW_NV12_SIZE ((PREVIEW_WIDTH) * (PREVIEW_HEIGHT) * 3 / 2)

/* BGRA 帧字节数（UI 显示格式） */
#define PREVIEW_BGRA_SIZE ((PREVIEW_WIDTH) * (PREVIEW_HEIGHT) * 4)

/* 预览帧共享缓冲（互斥锁保护，多消费者模式，帧大小由 init 指定） */
typedef struct {
    pthread_mutex_t mutex;
    uint32_t        frame_id; /* 帧序号：push 递增，消费者按序号判断新帧 */
    uint32_t        size;     /* 帧字节数 */
    uint8_t        *buf;      /* 帧缓冲（init 时堆分配） */
} preview_share_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：preview_share_init
 * 功能描述：初始化互斥锁并按指定帧大小分配缓冲
 * 输入参数：@ps   - 共享缓冲指针
 *           @size - 帧字节数（如 PREVIEW_NV12_SIZE / PREVIEW_BGRA_SIZE）
 * 注意事项：与 preview_share_deinit 配对使用
 *****************************************************************************/
void preview_share_init(preview_share_t *ps, uint32_t size);

/*****************************************************************************
 * 函数名称：preview_share_deinit
 * 功能描述：销毁互斥锁并释放帧缓冲
 * 输入参数：@ps - 共享缓冲指针
 * 注意事项：必须在所有消费者停止访问后调用
 *****************************************************************************/
void preview_share_deinit(preview_share_t *ps);

/*****************************************************************************
 * 函数名称：preview_share_push
 * 功能描述：GStreamer appsink 回调写入新帧（锁内拷贝，帧序号递增）
 * 输入参数：@ps  - 共享缓冲指针
 *           @data - 帧数据（size 字节）
 *****************************************************************************/
void preview_share_push(preview_share_t *ps, const uint8_t *data);

/*****************************************************************************
 * 函数名称：preview_share_pop
 * 功能描述：读取最新帧（多消费者安全：各消费者维护自己的 frame_id）
 * 输入参数：@ps       - 共享缓冲指针
 * 输出参数：@out      - 拷贝出的帧数据（调用者分配 size 字节）
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
