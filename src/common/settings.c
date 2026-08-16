/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：settings.c
 * 文件功能：全局设置实现 —— key=value 文本配置 + 互斥锁保护
 * 作    者：heifast
 * 创建日期：2026-08-16
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>

#include "log.h"
#include "settings.h"

/* 默认配置（首次运行 / 配置文件缺失时） */
#define DEFAULT_SEGMENT_SEC 120

/* 配置文件缓冲 */
#define CONF_PATH_MAX 256
#define LINE_BUF_SIZE 64

/* 全局配置实例（进程唯一，互斥锁保护） */
static settings_t s_settings;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_conf_path[CONF_PATH_MAX];


/*****************************************************************************
 * 函数名称：parse_bool
 * 功能描述：解析 "0"/"1" 为布尔值
 * 输入参数：@value - 字符串值
 * 输出参数：@out   - 解析结果
 * 返回值：  解析成功返回 true
 *****************************************************************************/
static bool parse_bool(const char *value, bool *out)
{
    if (0 == strcmp(value, "1")) {
        *out = true;
        return true;
    }
    if (0 == strcmp(value, "0")) {
        *out = false;
        return true;
    }
    return false;
}


/*****************************************************************************
 * 函数名称：settings_load_file
 * 功能描述：逐行解析 key=value 配置文件（调用方负责持锁）
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
static int settings_load_file(void)
{
    FILE *fp = NULL;
    char line[LINE_BUF_SIZE];
    int loaded = 0;

    fp = fopen(s_conf_path, "r");
    if (NULL == fp) {
        return -1;
    }

    while (NULL != fgets(line, sizeof(line), fp)) {
        char key[32];
        char value[32];
        bool bool_val;
        unsigned int sec = 0;

        if (2 == sscanf(line, "%31[^=]=%31s", key, value)) {
            if (0 == strcmp(key, "ai_draw_box")) {
                if (parse_bool(value, &bool_val)) {
                    s_settings.ai_draw_box = bool_val;
                    loaded++;
                }
            } else if (0 == strcmp(key, "ai_auto_lock")) {
                if (parse_bool(value, &bool_val)) {
                    s_settings.ai_auto_lock = bool_val;
                    loaded++;
                }
            } else if (0 == strcmp(key, "record_enabled")) {
                if (parse_bool(value, &bool_val)) {
                    s_settings.record_enabled = bool_val;
                    loaded++;
                }
            } else if (0 == strcmp(key, "audio_enabled")) {
                if (parse_bool(value, &bool_val)) {
                    s_settings.audio_enabled = bool_val;
                    loaded++;
                }
            } else if (0 == strcmp(key, "segment_sec")) {
                if ((1 == sscanf(value, "%u", &sec)) && (0 < sec)) {
                    s_settings.segment_sec = sec;
                    loaded++;
                }
            }
        }
    }
    fclose(fp);

    LOG_I("SET", "配置加载 %d 项（%s）", loaded, s_conf_path);
    return (0 < loaded) ? 0 : -1;
}


/*****************************************************************************
 * 函数名称：settings_init
 * 功能描述：加载配置文件（不存在则用默认值并写出）
 * 输入参数：@conf_path - 配置文件完整路径
 * 返回值：  成功返回0，失败返回-1（失败时保留默认值继续运行）
 *****************************************************************************/
int settings_init(const char *conf_path)
{
    char dir[CONF_PATH_MAX];
    char *slash = NULL;
    struct stat st;
    int ret;

    if (NULL == conf_path) {
        return -1;
    }

    strncpy(s_conf_path, conf_path, sizeof(s_conf_path) - 1);
    s_conf_path[sizeof(s_conf_path) - 1] = '\0';

    /* 确保配置目录存在（首次部署时可能没有 config 目录） */
    strncpy(dir, s_conf_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    slash = strrchr(dir, '/');
    if (NULL != slash) {
        *slash = '\0';
        if (0 != stat(dir, &st)) {
            mkdir(dir, 0755);
        }
    }

    pthread_mutex_lock(&s_mutex);

    /* 默认值先行，加载覆盖 */
    s_settings.ai_draw_box   = true;
    s_settings.ai_auto_lock  = true;
    s_settings.record_enabled = true;
    s_settings.audio_enabled = false;
    s_settings.segment_sec   = DEFAULT_SEGMENT_SEC;

    if (0 != settings_load_file()) {
        pthread_mutex_unlock(&s_mutex);
        LOG_W("SET", "配置文件加载失败，使用默认值并重建");
        return settings_save();
    }
    pthread_mutex_unlock(&s_mutex);
    ret = 0;

    return ret;
}


/*****************************************************************************
 * 函数名称：settings_save
 * 功能描述：把当前设置写回配置文件（key=value 文本）
 * 返回值：  成功返回0，失败返回-1
 *****************************************************************************/
int settings_save(void)
{
    FILE *fp = NULL;
    int ret = 0;

    pthread_mutex_lock(&s_mutex);

    fp = fopen(s_conf_path, "w");
    if (NULL == fp) {
        LOG_E("SET", "配置文件写入失败: %s", s_conf_path);
        ret = -1;
    } else {
        fprintf(fp, "# 行车记录仪设置（自动生成，可手改）\n");
        fprintf(fp, "ai_draw_box=%d\n", s_settings.ai_draw_box ? 1 : 0);
        fprintf(fp, "ai_auto_lock=%d\n", s_settings.ai_auto_lock ? 1 : 0);
        fprintf(fp, "record_enabled=%d\n",
                s_settings.record_enabled ? 1 : 0);
        fprintf(fp, "audio_enabled=%d\n", s_settings.audio_enabled ? 1 : 0);
        fprintf(fp, "segment_sec=%u\n", s_settings.segment_sec);
        fclose(fp);
    }

    pthread_mutex_unlock(&s_mutex);
    return ret;
}


/*****************************************************************************
 * 函数名称：settings_get_ai_draw_box
 * 功能描述：读取 AI 检测框绘制开关
 * 返回值：  开启返回 true
 *****************************************************************************/
bool settings_get_ai_draw_box(void)
{
    bool on;

    pthread_mutex_lock(&s_mutex);
    on = s_settings.ai_draw_box;
    pthread_mutex_unlock(&s_mutex);
    return on;
}


/*****************************************************************************
 * 函数名称：settings_set_ai_draw_box
 * 功能描述：设置 AI 检测框绘制开关
 * 输入参数：@on - 是否开启
 *****************************************************************************/
void settings_set_ai_draw_box(bool on)
{
    pthread_mutex_lock(&s_mutex);
    s_settings.ai_draw_box = on;
    pthread_mutex_unlock(&s_mutex);
}


/*****************************************************************************
 * 函数名称：settings_get_ai_auto_lock
 * 功能描述：读取 person 紧急自动锁定开关
 * 返回值：  开启返回 true
 *****************************************************************************/
bool settings_get_ai_auto_lock(void)
{
    bool on;

    pthread_mutex_lock(&s_mutex);
    on = s_settings.ai_auto_lock;
    pthread_mutex_unlock(&s_mutex);
    return on;
}


/*****************************************************************************
 * 函数名称：settings_set_ai_auto_lock
 * 功能描述：设置 person 紧急自动锁定开关
 * 输入参数：@on - 是否开启
 *****************************************************************************/
void settings_set_ai_auto_lock(bool on)
{
    pthread_mutex_lock(&s_mutex);
    s_settings.ai_auto_lock = on;
    pthread_mutex_unlock(&s_mutex);
}


/*****************************************************************************
 * 函数名称：settings_get_record_enabled
 * 功能描述：读取录像开关
 * 返回值：  开启返回 true
 *****************************************************************************/
bool settings_get_record_enabled(void)
{
    bool on;

    pthread_mutex_lock(&s_mutex);
    on = s_settings.record_enabled;
    pthread_mutex_unlock(&s_mutex);
    return on;
}


/*****************************************************************************
 * 函数名称：settings_set_record_enabled
 * 功能描述：设置录像开关
 * 输入参数：@on - 是否开启
 *****************************************************************************/
void settings_set_record_enabled(bool on)
{
    pthread_mutex_lock(&s_mutex);
    s_settings.record_enabled = on;
    pthread_mutex_unlock(&s_mutex);
}


/*****************************************************************************
 * 函数名称：settings_get_audio_enabled
 * 功能描述：读取录音开关（占位：音频链路未实现）
 * 返回值：  开启返回 true
 *****************************************************************************/
bool settings_get_audio_enabled(void)
{
    bool on;

    pthread_mutex_lock(&s_mutex);
    on = s_settings.audio_enabled;
    pthread_mutex_unlock(&s_mutex);
    return on;
}


/*****************************************************************************
 * 函数名称：settings_set_audio_enabled
 * 功能描述：设置录音开关（占位：音频链路未实现）
 * 输入参数：@on - 是否开启
 *****************************************************************************/
void settings_set_audio_enabled(bool on)
{
    pthread_mutex_lock(&s_mutex);
    s_settings.audio_enabled = on;
    pthread_mutex_unlock(&s_mutex);
}


/*****************************************************************************
 * 函数名称：settings_get_segment_sec
 * 功能描述：读取分段时长（秒）
 * 返回值：  分段时长
 *****************************************************************************/
uint32_t settings_get_segment_sec(void)
{
    uint32_t sec;

    pthread_mutex_lock(&s_mutex);
    sec = s_settings.segment_sec;
    pthread_mutex_unlock(&s_mutex);
    return sec;
}


/*****************************************************************************
 * 函数名称：settings_set_segment_sec
 * 功能描述：设置分段时长（秒），调用方需同步应用到编码器
 * 输入参数：@sec - 分段时长
 *****************************************************************************/
void settings_set_segment_sec(uint32_t sec)
{
    if (0 == sec) {
        return;
    }

    pthread_mutex_lock(&s_mutex);
    s_settings.segment_sec = sec;
    pthread_mutex_unlock(&s_mutex);
}
