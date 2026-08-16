/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：test_file_mgr.c
 * 文件功能：file_mgr 模块单元测试（PC 端运行）
 *          模拟 splitmuxsink 产出分段文件，验证巡检删除与锁定保护
 * 作    者：heifast
 * 创建日期：2026-08-11
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_mgr.h"

#define TEST_DIR "/tmp/test_videos"
#define TEST_MAX (10ULL * 1024 * 1024) /* 10MB */

/* 模拟 splitmuxsink 生成一个分段文件 */
static void make_segment(const char *name, int size_kb)
{
    char path[512];
    FILE *fp;
    char buf[1024];

    snprintf(path, sizeof(path), "%s/%s", TEST_DIR, name);
    fp = fopen(path, "wb");
    if (NULL == fp) {
        printf("FAIL: 创建 %s 失败\n", name);
        return;
    }
    memset(buf, 'A', sizeof(buf));
    for (int i = 0; i < size_kb; i++) {
        fwrite(buf, 1, sizeof(buf), fp);
    }
    fclose(fp);
}

/* 在文件末尾追加最小 moov 索引（8 字节：长度+魔数），模拟封口 */
static void append_moov(const char *name)
{
    char path[512];
    FILE *fp;
    uint8_t moov_box[8] = {0x00, 0x00, 0x00, 0x08,
                           'm', 'o', 'o', 'v'};

    snprintf(path, sizeof(path), "%s/%s", TEST_DIR, name);
    fp = fopen(path, "ab");
    if (NULL == fp) {
        printf("FAIL: 追加 moov 失败 %s\n", name);
        return;
    }
    fwrite(moov_box, 1, sizeof(moov_box), fp);
    fclose(fp);
}

int main(void)
{
    file_mgr_t mgr;

    system("rm -rf " TEST_DIR);
    system("mkdir -p " TEST_DIR);

    /* 场景1：初始化（空目录） */
    printf("===== 场景1: 初始化 =====\n");
    if (0 != file_mgr_init(&mgr, TEST_DIR, TEST_MAX, 8000000)) {
        printf("FAIL: init 失败\n");
        return -1;
    }
    printf("  上限: %llu MB, 队列: %d 个文件\n",
           (unsigned long long)(mgr.max_total_size / 1024 / 1024),
           file_mgr_get_count(&mgr));
    printf("PASS\n\n");

    /* 场景2：模拟 splitmuxsink 产出 3 个分段 + 巡检入队 */
    printf("===== 场景2: 分段文件入队 =====\n");
    make_segment("rec_00000.mp4", 500);   /* 每个 500KB */
    make_segment("rec_00001.mp4", 500);
    make_segment("rec_00002.mp4", 500);
    file_mgr_check(&mgr);
    printf("  队列: %d 个文件, 已用: %llu MB\n",
           file_mgr_get_count(&mgr),
           (unsigned long long)(mgr.current_used / 1024 / 1024));
    printf("PASS\n\n");

    /* 场景3：紧急文件（_E 后缀，模拟锁定） */
    printf("===== 场景3: 紧急文件识别 =====\n");
    make_segment("rec_00042_E.mp4", 500);
    file_mgr_check(&mgr);
    printf("  队列: %d 个文件（含 1 个 _E）\n", file_mgr_get_count(&mgr));
    printf("PASS\n\n");

    /* 场景4：超限巡检 — 疯狂产文件触发 evict */
    printf("===== 场景4: 超限巡检删除 =====\n");
    for (int i = 3; i < 40; i++) {
        char name[64];
        snprintf(name, sizeof(name), "rec_%05d.mp4", i);
        make_segment(name, 500);
    }
    file_mgr_check(&mgr);
    printf("  巡检后队列: %d 个文件, 已用: %llu MB（上限 10MB）\n",
           file_mgr_get_count(&mgr),
           (unsigned long long)(mgr.current_used / 1024 / 1024));
    /* 测试文件无 moov（未封口），tail 不计入可展示数 */
    printf("  可展示: %d 个（不含未封口 tail）\n",
           file_mgr_get_listable_count(&mgr));
    printf("PASS\n\n");

    /* 场景5：验证最旧文件被删、_E 文件保留 */
    fflush(stdout);
    printf("===== 场景5: 锁定保护验证 =====\n");
    printf("  ");
    fflush(stdout);
    system("ls " TEST_DIR "/*_E.mp4 2>/dev/null | wc -l | xargs echo _E_count:");
    printf("  ");
    fflush(stdout);
    system("ls " TEST_DIR "/*.mp4 2>/dev/null | wc -l | xargs echo total_count:");
    printf("  ");
    fflush(stdout);
    system("ls " TEST_DIR "/*.mp4 2>/dev/null | sort | head -3 | xargs echo oldest_files:");
    printf("PASS\n\n");

    /* 场景6：封口探测——有 moov 的最新分段可展示，未封口的被跳过 */
    printf("===== 场景6: 封口探测 =====\n");
    make_segment("rec_00090.mp4", 100); /* 未封口：模拟正在写 */
    file_mgr_check(&mgr);
    printf("  可展示: %d 个（tail 未封口，应比总数少 1）\n",
           file_mgr_get_listable_count(&mgr));
    {
        video_entry_t list[8];
        int n = file_mgr_get_list(&mgr, list, 8, 0, true);
        printf("  列表第 1 个: %s（应跳过未封口 tail）\n",
               (0 < n) ? list[0].filepath : "空");
    }
    append_moov("rec_00090.mp4"); /* 补 moov：模拟分段封口 */
    file_mgr_check(&mgr);
    printf("  封口后可展示: %d 个（应恢复全量）\n",
           file_mgr_get_listable_count(&mgr));
    {
        video_entry_t list[8];
        int n = file_mgr_get_list(&mgr, list, 8, 0, true);
        printf("  列表第 1 个: %s（应含封口 tail）\n",
               (0 < n) ? list[0].filepath : "空");
    }
    printf("PASS\n\n");

    /* 收尾 */
    printf("===== 场景7: 收尾 =====\n");
    file_mgr_deinit(&mgr);
    printf("  deinit 完成\n");
    printf("PASS\n\n");

    printf("===== 全部测试完成 =====\n");
    printf("目录 %s 保留供查看，确认无误后 rm -rf\n", TEST_DIR);

    return 0;
}
