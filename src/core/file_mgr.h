/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：file_mgr.h
 * 文件功能：录像文件管理 —— 循环覆盖、紧急锁定（守护 splitmuxsink 输出目录）
 * 作    者：heifast
 * 创建日期：2026-08-11
 *****************************************************************************/

#ifndef FILE_MGR_H
#define FILE_MGR_H

#include <stddef.h>  /* size_t */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* 紧急录像后缀（改名持久化锁定：rec_00007.mp4 → rec_00007_E.mp4） */
#define FILE_EMERGENCY_SUFFIX "_E"

/* 路径缓冲统一长度 */
#define FILE_PATH_MAX 512

/* 单个录像文件的元数据 */
typedef struct {
    char     filepath[FILE_PATH_MAX];
    uint64_t size;
    time_t   timestamp;
    bool     is_locked;
} video_entry_t;

/* 双向链表节点 */
typedef struct video_node {
    video_entry_t       entry;
    struct video_node  *next;
    struct video_node  *prev;
} video_node_t;

/* 录像文件管理器（线程安全：公共接口内部加锁） */
typedef struct {
    pthread_mutex_t mutex;             /* 队列保护锁 */
    char            storage_path[256]; /* 存储路径 */
    uint64_t        max_total_size;    /* 总容量（字节） */
    uint64_t        current_used;      /* 当前已用空间（字节） */
    uint32_t        entry_count;       /* 队列中文件数 */
    video_node_t   *head;              /* 最旧文件 */
    video_node_t   *tail;              /* 最新文件（正在写入的分段） */
} file_mgr_t;

#ifdef __cplusplus
extern "C" {
#endif

int  file_mgr_init(file_mgr_t *mgr, const char *path, uint64_t max_size);
int  file_mgr_evict(file_mgr_t *mgr, uint64_t size_needed);
int  file_mgr_lock_file(file_mgr_t *mgr, const char *filepath);
int  file_mgr_lock_latest(file_mgr_t *mgr, char *path, size_t path_size);
int  file_mgr_check(file_mgr_t *mgr);
int  file_mgr_get_count(file_mgr_t *mgr);
uint64_t file_mgr_get_used(file_mgr_t *mgr);
int  file_mgr_get_latest(file_mgr_t *mgr, char *path, size_t path_size);
int  file_mgr_get_list(file_mgr_t *mgr, video_entry_t *out, int max,
                      int offset);
void file_mgr_deinit(file_mgr_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* FILE_MGR_H */
