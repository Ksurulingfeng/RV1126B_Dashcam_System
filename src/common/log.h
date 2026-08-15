/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：log.h
 * 文件功能：全局分级日志 —— 总开关 + 等级阈值 + 模块标签
 * 作    者：heifast
 * 创建日期：2026-08-15
 *****************************************************************************/

#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/* ===================== 总开关 =====================
 * LOG_ENABLE = 0：编译期删除全部日志代码（量产静默，零开销）
 * LOG_ENABLE = 1：按等级阈值过滤输出 */
#define LOG_ENABLE 1

/* ===================== 等级定义（值越小越重要） ===================== */
#define LOG_LVL_ERROR 0 /* 致命错误（模块不可用，必须看到） */
#define LOG_LVL_WARN  1 /* 异常但可恢复（如取帧失败） */
#define LOG_LVL_INFO  2 /* 状态变化（启动/退出/关键事件） */
#define LOG_LVL_DEBUG 3 /* 高频调试（检测结果/推理耗时） */

/* 全局打印阈值：低于等于该等级的输出全部打印 */
#define LOG_LEVEL LOG_LVL_INFO

/* ===================== 通用日志宏 =====================
 * tag 为模块标签字符串（如 "AI"/"GST"/"UI"），打印格式 [标签][等级]
 * do-while(0) 保证宏像单条语句安全使用；
 * ##__VA_ARGS__ 处理无额外参数时的尾逗号 */
#define LOG_E(tag, fmt, ...)                                              \
    do {                                                                  \
        if ((LOG_ENABLE) && (LOG_LVL_ERROR <= LOG_LEVEL)) {               \
            fprintf(stderr, "[" tag "][E] " fmt "\n", ##__VA_ARGS__);     \
        }                                                                 \
    } while (0)

#define LOG_W(tag, fmt, ...)                                              \
    do {                                                                  \
        if ((LOG_ENABLE) && (LOG_LVL_WARN <= LOG_LEVEL)) {                \
            fprintf(stderr, "[" tag "][W] " fmt "\n", ##__VA_ARGS__);     \
        }                                                                 \
    } while (0)

#define LOG_I(tag, fmt, ...)                                              \
    do {                                                                  \
        if ((LOG_ENABLE) && (LOG_LVL_INFO <= LOG_LEVEL)) {                \
            printf("[" tag "][I] " fmt "\n", ##__VA_ARGS__);              \
        }                                                                 \
    } while (0)

#define LOG_D(tag, fmt, ...)                                              \
    do {                                                                  \
        if ((LOG_ENABLE) && (LOG_LVL_DEBUG <= LOG_LEVEL)) {               \
            printf("[" tag "][D] " fmt "\n", ##__VA_ARGS__);              \
        }                                                                 \
    } while (0)

#endif /* LOG_H */
