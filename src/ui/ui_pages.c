/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：ui_pages.c
 * 文件功能：UI 子页面实现 —— 录像文件库、设置页
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "thumb_pipeline.h"
#include "settings.h"
#include "ui_pages.h"

/* 主题颜色（简约风格：深灰黑 + 白灰文字，彩色仅 REC 红） */
#define UI_COLOR_BG     0x0E0E12 /* 页面背景（近黑） */
#define UI_COLOR_CARD   0x1C1C1E /* 卡片底色（深灰） */
#define UI_COLOR_TEXT   0xFFFFFF /* 主文本（白） */
#define UI_COLOR_TEXT2  0x8A8A8E /* 次文本（灰） */

/* 文件库单页最大显示条目
 * 注意：板端预编译 LVGL 的 LV_MEM_SIZE 仅 48KB，
 * 对象数量必须严格控制，条目过多会内存耗尽导致页面空白 */
#define UI_LIBRARY_MAX_ITEMS 8

/* 页面顶栏高度 */
#define UI_PAGE_TOP_BAR_H 56

/* 右侧导航栏占宽（与 ui_main.c UI_NAV_BAR_W 一致） */
#define UI_PAGE_NAV_W 160

/* ================= 共享样式（48KB 内存约束下必须共享） ================= */
static lv_style_t s_style_card;    /* 深灰卡片：底色+圆角+描边 */
static lv_style_t s_style_trans;   /* 透明容器：无背景无边框 */
static lv_style_t s_style_text_w;  /* 白字 + 中文字体 */
static lv_style_t s_style_text_g;  /* 灰字 + 中文字体 */
static lv_style_t s_style_topbar;  /* 页面顶栏 */
static bool s_styles_ready = false;

/* 文件库页面状态 */
static lv_obj_t* s_lib_list;        /* 列表容器（滚动） */
static lv_obj_t* s_lib_title;       /* 标题（含文件数/页码） */
static ui_worker_t* s_lib_ui;       /* 数据源 */
static int s_lib_last_count = -1;   /* 上次渲染的文件总数 */
static int s_lib_page = 0;          /* 当前页码（0 起） */
static int s_lib_page_max = 1;      /* 总页数（重建时更新） */
static int s_lib_last_page = -1;    /* 上次渲染的页码 */

/*****************************************************************************
 * 函数名称：ui_pages_styles_init
 * 功能描述：初始化共享样式（一次定义，全部对象引用）
 * 输入参数：@font - 中文字体
 * 注意事项：LVGL 预编译库 LV_MEM_SIZE 48KB，样式必须共享，
 *           严禁每个对象独立 lv_obj_set_style_*（内存会耗尽）
 *****************************************************************************/
static void ui_pages_styles_init(const lv_font_t* font)
{
    if (s_styles_ready) {
        return;
    }

    /* 卡片：深灰底 + 圆角 12 + 白 10% 描边 */
    lv_style_init(&s_style_card);
    lv_style_set_bg_color(&s_style_card, lv_color_hex(UI_COLOR_CARD));
    lv_style_set_radius(&s_style_card, 12);
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_border_color(&s_style_card, lv_color_white());
    lv_style_set_border_opa(&s_style_card, LV_OPA_10);

    /* 透明容器 */
    lv_style_init(&s_style_trans);
    lv_style_set_bg_opa(&s_style_trans, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_trans, 0);

    /* 白字 + 字体 */
    lv_style_init(&s_style_text_w);
    lv_style_set_text_color(&s_style_text_w, lv_color_hex(UI_COLOR_TEXT));
    lv_style_set_text_font(&s_style_text_w, font);

    /* 灰字 + 字体 */
    lv_style_init(&s_style_text_g);
    lv_style_set_text_color(&s_style_text_g, lv_color_hex(UI_COLOR_TEXT2));
    lv_style_set_text_font(&s_style_text_g, font);

    /* 顶栏 */
    lv_style_init(&s_style_topbar);
    lv_style_set_bg_color(&s_style_topbar, lv_color_hex(UI_COLOR_CARD));
    lv_style_set_bg_opa(&s_style_topbar, LV_OPA_90);
    lv_style_set_border_width(&s_style_topbar, 0);

    s_styles_ready = true;
}

/*****************************************************************************
 * 函数名称：ui_page_content_create
 * 功能描述：创建页面内容容器（右侧导航栏让位，全高左侧区域）
 * 输入参数：@parent - 页面 screen
 * 返回值：  内容容器
 *****************************************************************************/
static lv_obj_t* ui_page_content_create(lv_obj_t* parent)
{
    lv_obj_t* content = lv_obj_create(parent);

    /* 注意：lv_pct() 返回特殊编码值不能参与算术运算（会变成负尺寸），
     * 需要"百分比减固定值"时必须用固定像素（屏幕宽 1280 - 导航 160） */
    lv_obj_set_size(content, LV_HOR_RES - UI_PAGE_NAV_W, LV_VER_RES);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_style(content, &s_style_trans, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    return content;
}

/*****************************************************************************
 * 函数名称：ui_page_title_bar
 * 功能描述：创建页面顶栏（标题文字，中文字体渲染）
 * 输入参数：@parent - 父对象
 *           @title  - 标题文字
 * 返回值：  顶栏容器
 *****************************************************************************/
static lv_obj_t* ui_page_title_bar(lv_obj_t* parent, const char* title)
{
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_t* label;

    lv_obj_set_size(bar, lv_pct(100), UI_PAGE_TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_style(bar, &s_style_topbar, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(bar);
    lv_obj_center(label);
    lv_obj_add_style(label, &s_style_text_w, 0);
    lv_label_set_text(label, title);

    return bar;
}

/*****************************************************************************
 * 函数名称：bmp_load_to_canvas
 * 功能描述：把 24 位 BMP 解码到 canvas 缓冲（BGR24 → BGRA 展开）
 * 输入参数：@buf      - canvas 缓冲（THUMB_PIPELINE_W*H 个 lv_color_t）
 *           @bmp_path - BMP 文件路径
 * 返回值：  成功返回 true，失败 false
 * 注意事项：我们生成的 BMP 为负 height（像素自上而下），逐行读即可
 *****************************************************************************/
static bool bmp_load_to_canvas(lv_color_t* buf, const char* bmp_path)
{
    FILE* fp = NULL;
    uint8_t header[54];
    uint8_t* row_buf = NULL;
    int32_t width;
    int32_t height;
    uint32_t stride;
    int y;
    bool ret = false;

    fp = fopen(bmp_path, "rb");
    if (NULL == fp) {
        return false;
    }

    /* 读 54 字节头并解析宽高（小端） */
    if (54 != fread(header, 1, 54, fp)) {
        goto done;
    }
    width  = (int32_t)(header[18] | (header[19] << 8) |
                       (header[20] << 16) | (header[21] << 24));
    height = (int32_t)(header[22] | (header[23] << 8) |
                       (header[24] << 16) | (header[25] << 24));
    if (0 > height) {
        height = -height; /* 自顶向下存储 */
    }
    if ((THUMB_PIPELINE_W != width) || (THUMB_H != height)) {
        goto done;
    }

    /* 每行像素：BMP 行 4 字节对齐的 BGR24 */
    stride = ((uint32_t)width * 3 + 3) & ~3U;
    row_buf = (uint8_t*)malloc(stride);
    if (NULL == row_buf) {
        goto done;
    }

    /* 逐行展开 BGR → BGRA */
    for (y = 0; y < height; y++) {
        lv_color_t* dst = buf + y * width;
        int x;

        if (stride != fread(row_buf, 1, stride, fp)) {
            goto done;
        }
        for (x = 0; x < width; x++) {
            dst[x].ch.blue  = row_buf[x * 3 + 0];
            dst[x].ch.green = row_buf[x * 3 + 1];
            dst[x].ch.red   = row_buf[x * 3 + 2];
            dst[x].ch.alpha = 0xFF;
        }
    }
    ret = true;

done:
    free(row_buf);
    fclose(fp);
    return ret;
}

/*****************************************************************************
 * 函数名称：ui_library_add_row
 * 功能描述：向列表容器添加一行录像文件卡片（缩略图 + 文件名 + 元信息）
 * 输入参数：@parent - 列表容器
 *           @entry  - 文件元数据
 * 注意事项：行对象 user_data 存 video_path 副本（完成队列匹配用），
 *           由 ui_library_rebuild 清理时释放
 *****************************************************************************/
static void ui_library_add_row(lv_obj_t* parent, const video_entry_t* entry)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_t* thumb;
    lv_obj_t* label;
    lv_color_t* thumb_buf;
    char* path_copy;
    char name_buf[64];
    char info_buf[96];
    struct tm* tm_info;

    /* 行卡片（共享样式），高度加大容纳缩略图 */
    lv_obj_set_size(row, lv_pct(100), 120);
    lv_obj_add_style(row, &s_style_card, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 缩略图 canvas（160×90 左侧，生成前显示占位灰） */
    thumb = lv_canvas_create(row);
    thumb_buf = (lv_color_t*)malloc(sizeof(lv_color_t) *
                                    THUMB_PIPELINE_W * THUMB_H);
    if (NULL != thumb_buf) {
        memset(thumb_buf, 0x20, sizeof(lv_color_t) *
               THUMB_PIPELINE_W * THUMB_H);
        lv_canvas_set_buffer(thumb, thumb_buf, THUMB_PIPELINE_W,
                             THUMB_H, LV_IMG_CF_TRUE_COLOR);

        /* BMP 已生成则同步加载：程序重启后完成队列是空的，
         * 但磁盘上的 BMP 保留，幂等跳过请求会让 UI 永远
         * 看不到已生成的缩略图 */
        {
            char bmp_path[FILE_PATH_MAX];

            thumb_pipeline_bmp_path(entry->filepath, bmp_path);
            if (bmp_load_to_canvas(thumb_buf, bmp_path)) {
                lv_obj_invalidate(thumb);
            }
        }
    }
    lv_obj_align(thumb, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_radius(thumb, 8, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);

    /* 文件名（白字，只显示文件名不显示路径） */
    {
        const char* slash = strrchr(entry->filepath, '/');

        strncpy(name_buf,
                (NULL == slash) ? entry->filepath : slash + 1,
                sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
    }
    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 200, -16);
    lv_obj_add_style(label, &s_style_text_w, 0);
    lv_label_set_text(label, name_buf);

    /* 时间 + 时长（mm:ss）+ 大小（灰字） */
    tm_info = localtime(&entry->timestamp);
    strftime(info_buf, 32, "%m-%d %H:%M", tm_info);
    snprintf(info_buf + strlen(info_buf), sizeof(info_buf) - strlen(info_buf),
             "  %02u:%02u  %llu MB",
             entry->duration_sec / 60, entry->duration_sec % 60,
             (unsigned long long)(entry->size / 1024 / 1024));
    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 200, 16);
    lv_obj_add_style(label, &s_style_text_g, 0);
    lv_label_set_text(label, info_buf);

    /* 锁定标记（右上：锁定白字、普通灰字，亮度区分） */
    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -20, 0);
    if (entry->is_locked) {
        lv_obj_add_style(label, &s_style_text_w, 0);
        lv_label_set_text(label, "● 已锁定");
    } else {
        lv_obj_add_style(label, &s_style_text_g, 0);
        lv_label_set_text(label, "普通");
    }

    /* 行对象记忆录像路径（完成队列匹配缩略图用） */
    path_copy = (char*)malloc(strlen(entry->filepath) + 1);
    if (NULL != path_copy) {
        strcpy(path_copy, entry->filepath);
    }
    lv_obj_set_user_data(row, path_copy);
}

/*****************************************************************************
 * 函数名称：thumb_consume_tick
 * 功能描述：500ms 消费缩略图完成队列，解码 BMP 上屏
 * 输入参数：@timer - LVGL 定时器
 * 注意事项：canvas 缓冲由 lv_obj 持有，rebuild 时随对象销毁释放
 *****************************************************************************/
static void thumb_consume_tick(lv_timer_t* timer)
{
    thumb_done_t done;
    lv_obj_t* row;

    (void)timer;

    while (thumb_pipeline_pop_done(&done)) {
        uint32_t i;
        uint32_t child_cnt = lv_obj_get_child_cnt(s_lib_list);

        /* 按 video_path 匹配行卡片 */
        row = NULL;
        for (i = 0; i < child_cnt; i++) {
            lv_obj_t* child = lv_obj_get_child(s_lib_list, i);
            const char* row_path = (const char*)lv_obj_get_user_data(child);
            if ((NULL != row_path) && (0 == strcmp(row_path, done.video_path))) {
                row = child;
                break;
            }
        }
        /* 行仍在列表时解码 BMP 上屏（重建过的列表无此行则跳过） */
        if (NULL != row) {
            if (done.ok) {
                lv_obj_t* thumb = lv_obj_get_child(row, 0);
                lv_img_dsc_t* img = lv_canvas_get_img(thumb);
                if ((NULL != img) &&
                    bmp_load_to_canvas((lv_color_t*)img->data,
                                       done.bmp_path)) {
                    lv_obj_invalidate(thumb);
                }
            }
        }
    }
}

/*****************************************************************************
 * 函数名称：ui_library_rebuild
 * 功能描述：重建文件库列表（清空后按最新优先填充，限量条目控制内存）
 *****************************************************************************/
static void ui_library_rebuild(void)
{
    video_entry_t entries[UI_LIBRARY_MAX_ITEMS];
    char title_buf[64];
    int total;
    int count;
    int i;

    if (NULL == s_lib_ui) {
        return;
    }

    /* 总文件数（分页计算基准） */
    total = file_mgr_get_count(s_lib_ui->file_mgr);
    if (0 > total) {
        return;
    }

    /* 页码越界修正：文件被清理后总页数减少，回退到最后一页 */
    s_lib_page_max = (total + UI_LIBRARY_MAX_ITEMS - 1) /
                     UI_LIBRARY_MAX_ITEMS;
    if (0 == s_lib_page_max) {
        s_lib_page_max = 1;
    }
    if (s_lib_page >= s_lib_page_max) {
        s_lib_page = s_lib_page_max - 1;
    }

    /* 文件数与页码均未变化则跳过重建 */
    if ((total == s_lib_last_count) && (s_lib_page == s_lib_last_page)) {
        return;
    }
    s_lib_last_count = total;
    s_lib_last_page = s_lib_page;

    /* skip_tail：正在录的分段未封口（无 moov），不显示在文件库 */
    count = file_mgr_get_list(s_lib_ui->file_mgr, entries,
                              UI_LIBRARY_MAX_ITEMS,
                              s_lib_page * UI_LIBRARY_MAX_ITEMS, true);
    if (0 > count) {
        return;
    }

    /* 释放旧行的动态内存（仅行卡片：以路径副本 user_data 识别，
     * 占位 label 无 user_data 直接跳过——lv_canvas_get_img 对
     * 非 canvas 对象会触发 LVGL 断言） */
    for (i = 0; i < (int)lv_obj_get_child_cnt(s_lib_list); i++) {
        lv_obj_t* child = lv_obj_get_child(s_lib_list, i);
        char* path = (char*)lv_obj_get_user_data(child);

        if (NULL != path) {
            lv_obj_t* thumb = lv_obj_get_child(child, 0);
            if ((NULL != thumb) &&
                lv_obj_has_class(thumb, &lv_canvas_class)) {
                lv_img_dsc_t* img = lv_canvas_get_img(thumb);
                if (NULL != img) {
                    free((void*)img->data);
                }
            }
            free(path);
        }
    }
    lv_obj_clean(s_lib_list);

    /* 重建当前页列表 + 逐行请求缩略图（幂等：已生成的自动跳过） */
    for (i = 0; i < count; i++) {
        ui_library_add_row(s_lib_list, &entries[i]);
        thumb_pipeline_request(entries[i].filepath);
    }

    snprintf(title_buf, sizeof(title_buf), "录像文件库 (%d) %d/%d",
             total, s_lib_page + 1, s_lib_page_max);
    lv_label_set_text(s_lib_title, title_buf);
}

/*****************************************************************************
 * 函数名称：lib_page_btn_cb
 * 功能描述：翻页按钮回调（上页/下页），改页码后重建列表
 * 输入参数：@e - 事件（user_data 存偏移量 +1 下页 / -1 上页）
 *****************************************************************************/
static void lib_page_btn_cb(lv_event_t* e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);

    s_lib_page += delta;
    if (0 > s_lib_page) {
        s_lib_page = 0;
    }
    if (s_lib_page >= s_lib_page_max) {
        s_lib_page = s_lib_page_max - 1;
    }
    ui_library_rebuild();
}

/*****************************************************************************
 * 函数名称：ui_library_refresh_tick
 * 功能描述：每秒检查文件数变化，变化时重建列表
 * 输入参数：@timer - LVGL 定时器
 *****************************************************************************/
static void ui_library_refresh_tick(lv_timer_t* timer)
{
    (void)timer;
    ui_library_rebuild();
}

/*****************************************************************************
 * 函数名称：ui_page_library_create
 * 功能描述：创建录像文件库页面
 * 输入参数：@ui - UI 上下文
 * 返回值：  页面 screen 对象
 *****************************************************************************/
lv_obj_t* ui_page_library_create(ui_worker_t* ui)
{
    lv_obj_t* page = lv_obj_create(NULL);
    lv_obj_t* content;
    lv_obj_t* bar;
    lv_obj_t* label;

    s_lib_ui = ui;
    ui_pages_styles_init(ui->font);

    /* 页面背景 */
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_BG), 0);

    /* 内容区（右侧导航栏让位） */
    content = ui_page_content_create(page);

    /* 顶栏（标题 label 保存供刷新文件数/页码） */
    bar = ui_page_title_bar(content, "录像文件库");
    s_lib_title = lv_obj_get_child(bar, 0);

    /* 翻页按钮（顶栏左右两端，88×56 达触摸规范；
     * 符号用默认字体渲染——freetype 字体没有符号字形会乱码） */
    {
        lv_obj_t* btn_prev = lv_btn_create(bar);
        lv_obj_t* btn_next = lv_btn_create(bar);
        lv_obj_t* lbl_prev = lv_label_create(btn_prev);
        lv_obj_t* lbl_next = lv_label_create(btn_next);

        lv_obj_set_size(btn_prev, 88, UI_PAGE_TOP_BAR_H);
        lv_obj_set_size(btn_next, 88, UI_PAGE_TOP_BAR_H);
        lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_align(btn_next, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_style(btn_prev, &s_style_trans, 0);
        lv_obj_add_style(btn_next, &s_style_trans, 0);

        lv_obj_center(lbl_prev);
        lv_obj_center(lbl_next);
        lv_label_set_text(lbl_prev, LV_SYMBOL_LEFT);
        lv_label_set_text(lbl_next, LV_SYMBOL_RIGHT);

        lv_obj_add_event_cb(btn_prev, lib_page_btn_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)-1);
        lv_obj_add_event_cb(btn_next, lib_page_btn_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)1);
    }

    /* 滚动列表容器（顶栏下方） */
    s_lib_list = lv_obj_create(content);
    lv_obj_set_size(s_lib_list, lv_pct(100), lv_pct(100));
    lv_obj_align(s_lib_list, LV_ALIGN_TOP_MID, 0, UI_PAGE_TOP_BAR_H);
    lv_obj_add_style(s_lib_list, &s_style_trans, 0);
    lv_obj_set_style_pad_all(s_lib_list, 16, 0);
    lv_obj_set_style_pad_row(s_lib_list, 8, 0);
    lv_obj_set_flex_flow(s_lib_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lib_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* 空列表提示 */
    label = lv_label_create(s_lib_list);
    lv_obj_add_style(label, &s_style_text_g, 0);
    lv_label_set_text(label, "加载中...");

    /* 立即重建一次 + 每秒检测变化 + 500ms 消费缩略图完成队列 */
    s_lib_last_count = -1;
    ui_library_rebuild();
    lv_timer_create(ui_library_refresh_tick, 1000, NULL);
    lv_timer_create(thumb_consume_tick, 500, NULL);

    /* 右侧导航栏（全局组件） */
    ui_nav_bar_create(page, ui);

    return page;
}

/*****************************************************************************
 * 函数名称：ui_page_library_refresh
 * 功能描述：刷新文件库列表（外部触发，页面切换进入时调用）
 *****************************************************************************/
void ui_page_library_refresh(void)
{
    s_lib_last_count = -1;
    ui_library_rebuild();
}

/*****************************************************************************
 * 函数名称：ui_settings_add_item
 * 功能描述：向设置页添加一行信息条目（键值对）
 * 输入参数：@parent - 分组卡片
 *           @key    - 条目名
 *           @value  - 条目值
 *****************************************************************************/
static void ui_settings_add_item(lv_obj_t* parent, const char* key,
                                 const char* value)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_t* label;

    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_add_style(row, &s_style_trans, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_style(label, &s_style_text_w, 0);
    lv_label_set_text(label, key);

    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_style(label, &s_style_text_g, 0);
    lv_label_set_text(label, value);
}

/*****************************************************************************
 * 函数名称：ui_settings_group_create
 * 功能描述：创建设置页分组卡片（标题 + 条目容器）
 * 输入参数：@parent - 内容容器
 *           @title  - 分组标题
 * 返回值：  分组卡片
 *****************************************************************************/
static lv_obj_t* ui_settings_group_create(lv_obj_t* parent, const char* title)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_t* label;

    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_style(card, &s_style_card, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(card);
    lv_obj_add_style(label, &s_style_text_g, 0);
    lv_label_set_text(label, title);

    return card;
}

/* 设置开关标识（回调 user_data 区分） */
typedef enum {
    SETTINGS_SW_AI_DRAW, /* AI 检测框绘制 */
    SETTINGS_SW_AI_LOCK, /* person 紧急自动锁定 */
    SETTINGS_SW_RECORD,  /* 录像 */
    SETTINGS_SW_AUDIO,   /* 录音（占位） */
} settings_sw_id_t;

/*****************************************************************************
 * 函数名称：settings_switch_cb
 * 功能描述：设置开关值变化回调——写配置并持久化
 * 输入参数：@e - 事件（user_data 为 settings_sw_id_t 标识）
 * 注意事项：录像/分段时长的实际应用由主循环每秒巡检完成，
 *           此处只负责配置存储（解耦 UI 与编码器）
 *****************************************************************************/
static void settings_switch_cb(lv_event_t* e)
{
    lv_obj_t* sw = lv_event_get_target(e);
    settings_sw_id_t id = (settings_sw_id_t)(intptr_t)
                          lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    switch (id) {
        case SETTINGS_SW_AI_DRAW:
            settings_set_ai_draw_box(on);
            break;
        case SETTINGS_SW_AI_LOCK:
            settings_set_ai_auto_lock(on);
            break;
        case SETTINGS_SW_RECORD:
            settings_set_record_enabled(on);
            break;
        case SETTINGS_SW_AUDIO:
            settings_set_audio_enabled(on);
            break;
        default:
            break;
    }
    settings_save();
}

/*****************************************************************************
 * 函数名称：ui_settings_add_switch
 * 功能描述：向分组卡片添加一行开关（左标签 + 右 lv_switch）
 * 输入参数：@parent   - 分组卡片
 *           @key      - 开关名称
 *           @initial  - 初始开关状态
 *           @disabled - 禁用（占位功能用，灰色不可点）
 *           @id       - 回调标识（settings_sw_id_t）
 *****************************************************************************/
static void ui_settings_add_switch(lv_obj_t* parent, const char* key,
                                   bool initial, bool disabled,
                                   settings_sw_id_t id)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_t* label;
    lv_obj_t* sw;

    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_add_style(row, &s_style_trans, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_style(label, &s_style_text_w, 0);
    lv_label_set_text(label, key);

    sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    if (initial) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    if (disabled) {
        lv_obj_add_state(sw, LV_STATE_DISABLED);
        lv_obj_add_style(label, &s_style_text_g, 0);
    } else {
        lv_obj_add_event_cb(sw, settings_switch_cb, LV_EVENT_VALUE_CHANGED,
                            (void*)(intptr_t)id);
    }
}

/* 分段时长选项（秒） */
#define SEGMENT_CHOICE_1 60
#define SEGMENT_CHOICE_2 120
#define SEGMENT_CHOICE_3 300

/*****************************************************************************
 * 函数名称：segment_btn_cb
 * 功能描述：分段时长选择回调——写配置并持久化（主循环应用生效）
 * 输入参数：@e - 事件（user_data 为秒数）
 *****************************************************************************/
static void segment_btn_cb(lv_event_t* e)
{
    uint32_t sec = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    settings_set_segment_sec(sec);
    settings_save();
}

/*****************************************************************************
 * 函数名称：ui_settings_add_segment_row
 * 功能描述：向分组卡片添加分段时长选择行（1/2/5 分钟三按钮）
 * 输入参数：@parent - 分组卡片
 *****************************************************************************/
static void ui_settings_add_segment_row(lv_obj_t* parent)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_t* label;
    lv_obj_t* btn1;
    lv_obj_t* btn2;
    lv_obj_t* btn3;

    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_add_style(row, &s_style_trans, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_style(label, &s_style_text_w, 0);
    lv_label_set_text(label, "分段时长");

    btn1 = lv_btn_create(row);
    btn2 = lv_btn_create(row);
    btn3 = lv_btn_create(row);
    lv_obj_set_size(btn1, 64, 44);
    lv_obj_set_size(btn2, 64, 44);
    lv_obj_set_size(btn3, 64, 44);
    lv_obj_align(btn3, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_align_to(btn2, btn3, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_align_to(btn1, btn2, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    label = lv_label_create(btn1);
    lv_obj_center(label);
    lv_label_set_text(label, "1分");
    label = lv_label_create(btn2);
    lv_obj_center(label);
    lv_label_set_text(label, "2分");
    label = lv_label_create(btn3);
    lv_obj_center(label);
    lv_label_set_text(label, "5分");

    lv_obj_add_event_cb(btn1, segment_btn_cb, LV_EVENT_CLICKED,
                        (void*)(uintptr_t)SEGMENT_CHOICE_1);
    lv_obj_add_event_cb(btn2, segment_btn_cb, LV_EVENT_CLICKED,
                        (void*)(uintptr_t)SEGMENT_CHOICE_2);
    lv_obj_add_event_cb(btn3, segment_btn_cb, LV_EVENT_CLICKED,
                        (void*)(uintptr_t)SEGMENT_CHOICE_3);
}

/*****************************************************************************
 * 函数名称：ui_page_settings_create
 * 功能描述：创建设置页面（分组卡片信息展示）
 * 输入参数：@ui - UI 上下文
 * 返回值：  页面 screen 对象
 *****************************************************************************/
lv_obj_t* ui_page_settings_create(ui_worker_t* ui)
{
    lv_obj_t* page = lv_obj_create(NULL);
    lv_obj_t* content;
    lv_obj_t* card;
    char buf[96];
    int y = UI_PAGE_TOP_BAR_H + 12;

    ui_pages_styles_init(ui->font);

    /* 页面背景 + 顶栏 */
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_BG), 0);
    content = ui_page_content_create(page);
    ui_page_title_bar(content, "设置");

    /* 功能开关分组 */
    card = ui_settings_group_create(content, "功能开关");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    ui_settings_add_switch(card, "AI 识别画框", settings_get_ai_draw_box(),
                           false, SETTINGS_SW_AI_DRAW);
    ui_settings_add_switch(card, "紧急自动锁定", settings_get_ai_auto_lock(),
                           false, SETTINGS_SW_AI_LOCK);
    ui_settings_add_switch(card, "录像", settings_get_record_enabled(),
                           false, SETTINGS_SW_RECORD);
    ui_settings_add_switch(card, "录音（开发中）",
                           settings_get_audio_enabled(),
                           true, SETTINGS_SW_AUDIO);

    /* 录像参数分组 */
    y += 260;
    card = ui_settings_group_create(content, "录像参数");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    ui_settings_add_item(card, "画面", "1920×1080 @30fps · 8Mbps");
    ui_settings_add_segment_row(card);

    /* 存储分组 */
    y += 170;
    card = ui_settings_group_create(content, "存储");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    snprintf(buf, sizeof(buf), "%d 个文件 / %llu MB",
             file_mgr_get_count(ui->file_mgr),
             (unsigned long long)(file_mgr_get_used(ui->file_mgr)
                                  / 1024 / 1024));
    ui_settings_add_item(card, "SD 卡", buf);

    /* 关于分组 */
    y += 100;
    card = ui_settings_group_create(content, "关于");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    ui_settings_add_item(card, "版本 / 平台", "v0.4.0 · RV1126B + IMX415");

    /* 右侧导航栏（全局组件） */
    ui_nav_bar_create(page, ui);

    return page;
}
