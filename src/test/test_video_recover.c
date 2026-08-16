/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：test_video_recover.c
 * 文件功能：video_recover 模块单元测试（PC 端运行）
 *          用真实断电残留文件验证恢复效果
 * 作    者：heifast
 * 创建日期：2026-08-17
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_recover.h"

int main(int argc, char *argv[])
{
    const char *path;
    int frames;

    if (2 > argc) {
        printf("用法: %s <断电残留.mp4>\n", argv[0]);
        return -1;
    }
    path = argv[1];

    printf("===== 恢复测试: %s =====\n", path);

    /* 完好文件应返回 0（跳过） */
    frames = video_recover_file(path, 30);
    if (0 > frames) {
        printf("FAIL: 恢复失败\n");
        return -1;
    }
    if (0 == frames) {
        printf("SKIP: 文件已封口，无需恢复\n");
        return 0;
    }

    printf("恢复成功: %d 帧（约 %d 秒 @30fps）\n",
           frames, frames / 30);
    printf("PASS\n");
    return 0;
}
