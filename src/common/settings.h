/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：settings.h
 * 文件功能：全局设置 —— 配置文件持久化 + 互斥锁线程安全读写
 * 作    者：heifast
 * 创建日期：2026-08-16
 *****************************************************************************/

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* 设置项（读写接口内部加锁，任意线程安全） */
typedef struct {
    bool     ai_enabled;     /* AI 识别总开关（关闭跳过推理省 CPU） */
    bool     ai_draw_box;    /* AI 检测框绘制开关 */
    bool     ai_auto_lock;   /* person 紧急自动锁定开关 */
    bool     record_enabled; /* 录像开关（GStreamer valve 控制） */
    bool     audio_enabled;  /* 录音开关（占位：音频链路未实现） */
    bool     stream_enabled; /* 局域网推流开关（RTP/UDP valve 控制） */
    uint32_t segment_sec;    /* 分段时长（秒） */
} settings_t;

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * 函数名称：settings_init
 * 功能描述：加载配置文件（不存在则用默认值并写出）
 * 输入参数：@conf_path - 配置文件完整路径
 * 返回值：  成功返回0，失败返回-1（失败时保留默认值继续运行）
 *****************************************************************************/
int settings_init(const char *conf_path);

/*****************************************************************************
 * 函数名称：settings_save
 * 功能描述：把当前设置写回配置文件
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：设置项变化时调用（UI 开关事件中）
 *****************************************************************************/
int settings_save(void);

/* 以下为各设置项的线程安全读写接口 */
bool settings_get_ai_enabled(void);
void settings_set_ai_enabled(bool on);
bool settings_get_ai_draw_box(void);
void settings_set_ai_draw_box(bool on);
bool settings_get_ai_auto_lock(void);
void settings_set_ai_auto_lock(bool on);
bool settings_get_record_enabled(void);
void settings_set_record_enabled(bool on);
bool settings_get_audio_enabled(void);
void settings_set_audio_enabled(bool on);
bool settings_get_stream_enabled(void);
void settings_set_stream_enabled(bool on);
uint32_t settings_get_segment_sec(void);
void settings_set_segment_sec(uint32_t sec);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
