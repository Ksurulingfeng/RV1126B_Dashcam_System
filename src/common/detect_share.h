/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：detect_share.h
 * 文件功能：检测结果共享 —— AI 线程写、UI 线程读（互斥锁保护）
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#ifndef DETECT_SHARE_H
#define DETECT_SHARE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* 检测框名称最大长度（COCO 标签最长为 "refrigerator" 等） */
#define DETECT_NAME_MAX 32

/* 单帧检测框数量上限（与 postprocess OBJ_NUMB_MAX_SIZE 一致） */
#define DETECT_BOX_MAX 64

/* 单个检测框（C 兼容结构，不依赖 C++ 的 postprocess 头） */
typedef struct {
    int  left;
    int  top;
    int  right;
    int  bottom;
    char name[DETECT_NAME_MAX];
    int  conf; /* 置信度（0~100 百分数，AI 侧 prop×100 取整） */
} detect_box_t;

/* 检测结果共享缓冲 */
typedef struct {
    pthread_mutex_t mutex;
    uint32_t        frame_id; /* 结果对应的预览帧序号 */
    uint32_t        count;
    detect_box_t    boxes[DETECT_BOX_MAX];
} detect_share_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：detect_share_init
 * 功能描述：初始化互斥锁和结果缓冲
 * 输入参数：@ds - 共享缓冲指针
 *****************************************************************************/
void detect_share_init(detect_share_t *ds);

/*****************************************************************************
 * 函数名称：detect_share_push
 * 功能描述：AI 线程写入一帧检测结果（锁内拷贝）
 * 输入参数：@ds     - 共享缓冲指针
 *           @boxes  - 检测框数组
 *           @count  - 框数量
 *           @frame_id - 对应的预览帧序号
 *****************************************************************************/
void detect_share_push(detect_share_t *ds, const detect_box_t *boxes,
                       uint32_t count, uint32_t frame_id);

/*****************************************************************************
 * 函数名称：detect_share_pop
 * 功能描述：UI 线程读取最新检测结果
 * 输入参数：@ds     - 共享缓冲指针
 * 输出参数：@boxes  - 检测框输出数组（调用者分配 DETECT_BOX_MAX）
 *           @count  - 输出框数量
 * 返回值：  有新结果返回 true，无新结果返回 false
 *****************************************************************************/
bool detect_share_pop(detect_share_t *ds, detect_box_t *boxes,
                      uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* DETECT_SHARE_H */
