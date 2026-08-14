/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：file_mgr.c
 * 文件功能：录像文件管理 —— 循环覆盖、紧急锁定（线程安全版）
 * 作    者：heifast
 * 创建日期：2026-08-11
 *****************************************************************************/

#include <stdio.h>    /* snprintf, fprintf */
#include <stdlib.h>   /* malloc, free, realloc */
#include <string.h>   /* strncpy, strlen, memset, strcmp, strrchr */
#include <sys/stat.h> /* mkdir, stat */
#include <dirent.h>   /* opendir, readdir, closedir */
#include <time.h>     /* time_t */
#include <unistd.h>   /* remove */

#include "file_mgr.h"

/* 扫描缓冲初始容量（文件数），不足自动翻倍 */
#define SCAN_INIT_CAPACITY 32


/* =========================================================================
 * 内部队列操作（调用方需持有 mgr->mutex）
 * ========================================================================= */

/*****************************************************************************
 * 队尾插入新节点
 *****************************************************************************/
static int queue_push_tail(file_mgr_t *mgr, video_entry_t *entry)
{
    video_node_t *new_node;

    if ((NULL == mgr) || (NULL == entry)) {
        return -1;
    }

    new_node = (video_node_t *)malloc(sizeof(video_node_t));
    if (NULL == new_node) {
        return -1;
    }

    new_node->entry = *entry;
    new_node->prev  = mgr->tail;
    new_node->next  = NULL;

    if (NULL == mgr->tail) {
        mgr->head = new_node;
    } else {
        mgr->tail->next = new_node;
    }
    mgr->tail = new_node;

    mgr->entry_count++;
    mgr->current_used += entry->size;

    return 0;
}


/*****************************************************************************
 * 从队列中移除指定节点，释放节点内存
 *****************************************************************************/
static void queue_remove(file_mgr_t *mgr, video_node_t *node)
{
    if ((NULL == mgr) || (NULL == node)) {
        return;
    }

    /* 从链表中摘除 */
    if (NULL == node->prev) {
        mgr->head = node->next;
    } else {
        node->prev->next = node->next;
    }

    if (NULL == node->next) {
        mgr->tail = node->prev;
    } else {
        node->next->prev = node->prev;
    }

    mgr->current_used -= node->entry.size;
    mgr->entry_count--;

    free(node);
}


/*****************************************************************************
 * 从队头开始找第一个可删除的节点（未锁定，且不是正在写入的最新分段）
 * 返回：节点指针（找到），NULL（无可删文件）
 *****************************************************************************/
static video_node_t *queue_find_evictable(file_mgr_t *mgr)
{
    video_node_t *cur;

    if (NULL == mgr) {
        return NULL;
    }

    cur = mgr->head;
    while (NULL != cur) {
        /* 跳过锁定文件；跳过队尾（splitmuxsink 正在写入的分段） */
        if ((!cur->entry.is_locked) && (cur != mgr->tail)) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}


/*****************************************************************************
 * 释放队列全部节点
 *****************************************************************************/
static void queue_free_all(file_mgr_t *mgr)
{
    video_node_t *cur;
    video_node_t *next;

    if (NULL == mgr) {
        return;
    }

    cur = mgr->head;
    while (NULL != cur) {
        next = cur->next;
        free(cur);
        cur = next;
    }

    mgr->head = NULL;
    mgr->tail = NULL;
    mgr->entry_count = 0;
    mgr->current_used = 0;
}


/*****************************************************************************
 * 按 timestamp 升序排序队列
 *****************************************************************************/
static void queue_sort(file_mgr_t *mgr)
{
    video_node_t *a, *b;

    if ((NULL == mgr) || (mgr->entry_count < 2)) {
        return;
    }

    for (a = mgr->head; NULL != a; a = a->next) {
        for (b = a->next; NULL != b; b = b->next) {
            /* 时间戳相同时用文件名作第二排序键，保证排序稳定 */
            bool need_swap = (a->entry.timestamp > b->entry.timestamp) ||
                             ((a->entry.timestamp == b->entry.timestamp) &&
                              (strcmp(a->entry.filepath, b->entry.filepath) > 0));
            if (need_swap) {
                /* 交换 entry 数据，不动节点指针 */
                video_entry_t tmp = a->entry;
                a->entry = b->entry;
                b->entry = tmp;
            }
        }
    }
}


/*****************************************************************************
 * 判断文件名是否为紧急录像（_E.mp4 结尾）
 *****************************************************************************/
static bool name_is_emergency(const char *name)
{
    const char *dot;

    dot = strrchr(name, '.');
    if (NULL == dot) {
        return false;
    }
    /* ".mp4" 前两个字符为 "_E" */
    return (dot - name >= 2) && ('_' == dot[-2]) && ('E' == dot[-1]);
}


/*****************************************************************************
 * 锁定队列节点：重命名为 _E 后缀并更新队列条目（需持有 mutex）
 *****************************************************************************/
static int node_lock(file_mgr_t *mgr, video_node_t *node)
{
    char new_path[FILE_PATH_MAX];
    const char *dot;
    size_t base_len;

    if ((NULL == mgr) || (NULL == node)) {
        return -1;
    }

    /* 已锁定视为成功（幂等） */
    if (node->entry.is_locked) {
        return 0;
    }

    /* 构造新名：xxx.mp4 → xxx_E.mp4 */
    dot = strrchr(node->entry.filepath, '.');
    if (NULL == dot) {
        return -1;
    }
    base_len = (size_t)(dot - node->entry.filepath);
    snprintf(new_path, sizeof(new_path), "%.*s%s.mp4",
             (int)base_len, node->entry.filepath,
             FILE_EMERGENCY_SUFFIX);

    /* rename 持久化锁定：重启后扫描文件名仍可恢复状态 */
    if (0 != rename(node->entry.filepath, new_path)) {
        fprintf(stderr, "[file_mgr] 锁定失败: %s\n", node->entry.filepath);
        return -1;
    }

    strncpy(node->entry.filepath, new_path, sizeof(node->entry.filepath) - 1);
    node->entry.is_locked = true;
    return 0;
}


/* =========================================================================
 * 内部辅助：锁外扫描目录，收集 .mp4 文件元数据到动态数组
 * 注意事项：不触碰队列，可在任意线程调用；调用者负责 free 返回数组
 * ========================================================================= */
static int scan_dir_entries(const char *path, video_entry_t **out_entries,
                            int *out_count)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    video_entry_t *entries = NULL;
    int capacity = 0;
    int count = 0;

    dir = opendir(path);
    if (NULL == dir) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *dot = strrchr(entry->d_name, '.');
        if ((NULL != dot) && (0 == strcmp(dot, ".mp4"))) {
            struct stat file_stat;
            char fullpath[FILE_PATH_MAX];
            video_entry_t ve;

            snprintf(fullpath, sizeof(fullpath), "%s/%s",
                     path, entry->d_name);
            /* 文件可能正被删除/切换，stat 失败则跳过 */
            if (0 == stat(fullpath, &file_stat)) {
                /* 容量不足时翻倍扩容 */
                if (count >= capacity) {
                    int new_cap = (0 == capacity) ?
                                  SCAN_INIT_CAPACITY : capacity * 2;
                    video_entry_t *tmp = (video_entry_t *)realloc(
                        entries, sizeof(video_entry_t) * (size_t)new_cap);
                    if (NULL == tmp) {
                        free(entries);
                        closedir(dir);
                        return -1;
                    }
                    entries = tmp;
                    capacity = new_cap;
                }

                memset(&ve, 0, sizeof(ve));
                strncpy(ve.filepath, fullpath, sizeof(ve.filepath) - 1);
                ve.size      = (uint64_t)file_stat.st_size;
                ve.timestamp = file_stat.st_mtime;
                ve.is_locked = name_is_emergency(entry->d_name);
                entries[count++] = ve;
            }
        }
    }
    closedir(dir);

    *out_entries = entries;
    *out_count = count;
    return 0;
}


/*****************************************************************************
 * 用扫描结果重建队列（需持有 mutex，纯内存操作无 I/O）
 *****************************************************************************/
static void queue_rebuild(file_mgr_t *mgr, video_entry_t *entries, int count)
{
    int i;

    queue_free_all(mgr);
    for (i = 0; i < count; i++) {
        queue_push_tail(mgr, &entries[i]);
    }
    queue_sort(mgr);
}


/* =========================================================================
 * 对外接口（全部线程安全）
 * ========================================================================= */

/*****************************************************************************
 * 函数名称：file_mgr_init
 * 功能描述：初始化文件管理器，创建目录，加载已有文件到内存队列
 * 输入参数：@mgr      - 管理器指针
 *           @path     - 录像存储目录
 *           @max_size - 最大可用空间（字节）
 * 返回值：  成功返回0，失败返回-1
 * 注意事项：启动时调用一次，会自动恢复上次的紧急文件锁定状态
 *****************************************************************************/
int file_mgr_init(file_mgr_t *mgr, const char *path, uint64_t max_size)
{
    struct stat st;
    video_entry_t *entries = NULL;
    int count = 0;

    if ((NULL == mgr) || (NULL == path)) {
        return -1;
    }

    /* 确保目录存在 */
    if (0 != stat(path, &st)) {
        if (0 != mkdir(path, 0755)) {
            return -1;
        }
    }

    /* 初始化所有字段 */
    memset(mgr, 0, sizeof(file_mgr_t));
    pthread_mutex_init(&mgr->mutex, NULL);
    strncpy(mgr->storage_path, path, sizeof(mgr->storage_path) - 1);
    mgr->max_total_size = max_size;

    /* 锁外扫描 + 锁内重建队列；失败时销毁锁保持资源配对 */
    if (0 != scan_dir_entries(path, &entries, &count)) {
        pthread_mutex_destroy(&mgr->mutex);
        memset(mgr, 0, sizeof(file_mgr_t));
        return -1;
    }
    pthread_mutex_lock(&mgr->mutex);
    queue_rebuild(mgr, entries, count);
    pthread_mutex_unlock(&mgr->mutex);
    free(entries);

    return 0;
}


/*****************************************************************************
 * 函数名称：file_mgr_evict
 * 功能描述：从队头逐个删除未锁定文件，直到腾出足够空间
 * 输入参数：@mgr         - 管理器指针
 *           @size_needed - 需要腾出的空间（字节）
 * 返回值：  成功返回0，失败返回-1（只剩锁定文件，腾不够）
 * 注意事项：跳过 is_locked=true 的文件和正在写入的最新分段
 *****************************************************************************/
int file_mgr_evict(file_mgr_t *mgr, uint64_t size_needed)
{
    uint64_t freed = 0;
    int      ret   = -1;

    if (NULL == mgr) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);

    while (freed < size_needed) {
        video_node_t *node = queue_find_evictable(mgr);
        if (NULL == node) {
            break;  /* 全部锁定/只剩正在写的分段 */
        }

        /* 删除磁盘文件（单文件 remove 耗时短，低频调用，可接受锁内执行） */
        if (0 != remove(node->entry.filepath)) {
            pthread_mutex_unlock(&mgr->mutex);
            return -1;
        }

        freed += node->entry.size;
        queue_remove(mgr, node);
    }

    if (freed >= size_needed) {
        ret = 0;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return ret;
}


/*****************************************************************************
 * 函数名称：file_mgr_lock_file
 * 功能描述：锁定指定文件——重命名为 _E 后缀（持久化，重启后仍锁定）
 * 输入参数：@mgr      - 管理器指针
 *           @filepath - 文件完整路径
 * 返回值：  成功返回0，未找到或失败返回-1
 * 注意事项：锁定通过文件名持久化，file_mgr_check 重建队列不会丢失
 *****************************************************************************/
int file_mgr_lock_file(file_mgr_t *mgr, const char *filepath)
{
    video_node_t *cur;
    int           ret = -1;

    if ((NULL == mgr) || (NULL == filepath)) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);

    cur = mgr->head;
    while (NULL != cur) {
        if (0 == strcmp(cur->entry.filepath, filepath)) {
            ret = node_lock(mgr, cur);
            break;
        }
        cur = cur->next;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return ret;
}


/*****************************************************************************
 * 函数名称：file_mgr_lock_latest
 * 功能描述：原子地"取最新分段并锁定"（单次持锁，无 TOCTOU 窗口）
 * 输入参数：@mgr       - 管理器指针
 *           @path      - 输出：锁定后的文件路径
 *           @path_size - 输出缓冲大小
 * 返回值：  成功返回0（已锁定文件返回路径也视为成功），失败返回-1
 * 注意事项：供 AI 紧急锁定联动使用——队列为空（启动初期尚未
 *           巡检到分段）时返回失败，调用方下帧重试即可
 *****************************************************************************/
int file_mgr_lock_latest(file_mgr_t *mgr, char *path, size_t path_size)
{
    int ret = -1;

    if ((NULL == mgr) || (NULL == path) || (0 == path_size)) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);

    if (NULL != mgr->tail) {
        /* 原子完成：锁定 + 取路径（两步在锁内，队列不会中途变化） */
        if (0 == node_lock(mgr, mgr->tail)) {
            strncpy(path, mgr->tail->entry.filepath, path_size - 1);
            path[path_size - 1] = '\0';
            ret = 0;
        }
    }

    pthread_mutex_unlock(&mgr->mutex);
    return ret;
}


/*****************************************************************************
 * 函数名称：file_mgr_get_count
 * 功能描述：查询当前队列中的文件总数
 * 输入参数：@mgr - 管理器指针
 * 返回值：  文件数量（≥0），mgr 为空返回 -1
 *****************************************************************************/
int file_mgr_get_count(file_mgr_t *mgr)
{
    int count;

    if (NULL == mgr) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);
    count = (int)mgr->entry_count;
    pthread_mutex_unlock(&mgr->mutex);

    return count;
}


/*****************************************************************************
 * 函数名称：file_mgr_get_used
 * 功能描述：查询当前已用空间（字节）
 * 输入参数：@mgr - 管理器指针
 * 返回值：  已用字节数，mgr 为空返回 0
 *****************************************************************************/
uint64_t file_mgr_get_used(file_mgr_t *mgr)
{
    uint64_t used;

    if (NULL == mgr) {
        return 0;
    }

    pthread_mutex_lock(&mgr->mutex);
    used = mgr->current_used;
    pthread_mutex_unlock(&mgr->mutex);

    return used;
}


/*****************************************************************************
 * 函数名称：file_mgr_get_latest
 * 功能描述：获取队列中最新录像文件的完整路径（正在写入的分段）
 * 输入参数：@mgr       - 管理器指针
 *           @path      - 输出路径缓冲
 *           @path_size - 输出缓冲大小（字节）
 * 返回值：  成功返回0，队列为空或参数无效返回-1
 * 注意事项：仅供查询；需要"取路径并锁定"请用 file_mgr_lock_latest
 *****************************************************************************/
int file_mgr_get_latest(file_mgr_t *mgr, char *path, size_t path_size)
{
    int ret = -1;

    if ((NULL == mgr) || (NULL == path) || (0 == path_size)) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);

    if (NULL != mgr->tail) {
        strncpy(path, mgr->tail->entry.filepath, path_size - 1);
        path[path_size - 1] = '\0';
        ret = 0;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return ret;
}


/*****************************************************************************
 * 函数名称：file_mgr_get_list
 * 功能描述：获取录像文件列表快照（最新在前，供 UI 文件库展示）
 * 输入参数：@mgr - 管理器指针
 *           @out - 输出数组（调用方分配，video_entry_t[max]）
 *           @max - 输出数组容量
 * 返回值：  拷贝的文件数（0 表示队列为空），失败返回 -1
 * 注意事项：锁内快照拷贝，调用方拿到的是独立数据，无并发风险
 *****************************************************************************/
int file_mgr_get_list(file_mgr_t *mgr, video_entry_t *out, int max)
{
    video_node_t *cur;
    int count = 0;

    if ((NULL == mgr) || (NULL == out) || (0 >= max)) {
        return -1;
    }

    pthread_mutex_lock(&mgr->mutex);

    /* 从 tail（最新）向 head（最旧）遍历，最新文件排在前面 */
    cur = mgr->tail;
    while ((NULL != cur) && (count < max)) {
        out[count++] = cur->entry;
        cur = cur->prev;
    }

    pthread_mutex_unlock(&mgr->mutex);
    return count;
}


/*****************************************************************************
 * 函数名称：file_mgr_check
 * 功能描述：巡检——锁外重新扫描目录，锁内重建队列，超限自动删除
 * 输入参数：@mgr - 管理器指针
 * 返回值：  成功0，失败-1
 * 注意事项：供主循环周期性调用（如每秒一次），配合 splitmuxsink
 *           分段录像使用；扫描失败时保留旧队列不丢元数据
 *****************************************************************************/
int file_mgr_check(file_mgr_t *mgr)
{
    video_entry_t *entries = NULL;
    uint64_t need = 0;
    int count = 0;

    if (NULL == mgr) {
        return -1;
    }

    /* 锁外扫描目录：磁盘 I/O 不阻塞 AI 锁定与 UI 查询 */
    if (0 != scan_dir_entries(mgr->storage_path, &entries, &count)) {
        fprintf(stderr, "[file_mgr] 扫描目录失败（SD 卡异常？）\n");
        return -1;
    }

    /* 锁内重建队列（纯内存操作，持锁时间极短） */
    pthread_mutex_lock(&mgr->mutex);
    queue_rebuild(mgr, entries, count);
    if (mgr->current_used > mgr->max_total_size) {
        need = mgr->current_used - mgr->max_total_size;
    }
    pthread_mutex_unlock(&mgr->mutex);
    free(entries);

    /* 超限删除（evict 内部自行加锁） */
    if (0 < need) {
        return file_mgr_evict(mgr, need);
    }

    return 0;
}


/*****************************************************************************
 * 函数名称：file_mgr_deinit
 * 功能描述：释放队列中所有节点内存，销毁锁，清零管理器
 * 输入参数：@mgr - 管理器指针
 * 注意事项：调用后 mgr 不可再用，除非重新 init
 *****************************************************************************/
void file_mgr_deinit(file_mgr_t *mgr)
{
    if (NULL == mgr) {
        return;
    }

    pthread_mutex_lock(&mgr->mutex);
    queue_free_all(mgr);
    pthread_mutex_unlock(&mgr->mutex);

    pthread_mutex_destroy(&mgr->mutex);
    memset(mgr, 0, sizeof(file_mgr_t));
}
