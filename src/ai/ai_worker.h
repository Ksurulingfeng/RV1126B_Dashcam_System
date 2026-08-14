/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：ai_worker.h
 * 文件功能：AI 目标检测线程 —— YOLOv5 NPU 推理（C/C++ 混合接口）
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#ifndef AI_WORKER_H
#define AI_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "file_mgr.h"
#include "frame_share.h"

/* AI 模块路径缓冲长度 */
#define AI_PATH_MAX     128 /* 模型/标签文件路径 */
#define AI_DEV_PATH_MAX 64  /* 摄像头设备节点路径 */

/* AI 线程上下文 */
typedef struct {
    char            model_path[AI_PATH_MAX];      /* .rknn 模型路径 */
    char            labels_path[AI_PATH_MAX];     /* 类别标签文件路径 */
    char            camera_dev[AI_DEV_PATH_MAX];  /* 摄像头节点（video24） */
    volatile bool  *running;            /* 退出标志 */
    frame_share_t  *frame_share;        /* 帧共享缓冲（推给 UI 显示） */
    file_mgr_t     *file_mgr;           /* 文件管理器（person 触发紧急锁定用） */
} ai_worker_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：ai_worker_entry
 * 功能描述：AI 推理线程入口（pthread_create 用）
 * 输入参数：@arg - ai_worker_t 指针
 * 返回值：  NULL
 * 注意事项：循环取帧 → 预处理 → NPU 推理 → 后处理 → 打印检测结果
 *****************************************************************************/
void *ai_worker_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* AI_WORKER_H */
