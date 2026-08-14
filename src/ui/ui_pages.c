/*****************************************************************************
 * Copyright (C) 2026 heifast. All rights reserved.
 *
 * 文件名称：ui_pages.c
 * 文件功能：UI 子页面实现 —— 录像文件库、设置页
 * 作    者：heifast
 * 创建日期：2026-08-14
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>

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
static lv_obj_t* s_lib_title;       /* 标题（含文件数） */
static ui_worker_t* s_lib_ui;       /* 数据源 */
static int s_lib_last_count = -1;   /* 上次渲染的文件数 */

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
 * 函数名称：ui_library_add_row
 * 功能描述：向列表容器添加一行录像文件卡片
 * 输入参数：@parent - 列表容器
 *           @entry  - 文件元数据
 *****************************************************************************/
static void ui_library_add_row(lv_obj_t* parent, const video_entry_t* entry)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_t* label;
    char name_buf[64];
    char info_buf[96];
    struct tm* tm_info;

    /* 行卡片（共享样式） */
    lv_obj_set_size(row, lv_pct(100), 96);
    lv_obj_add_style(row, &s_style_card, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 文件名（白字，左上） */
    strncpy(name_buf, entry->filepath, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, -12);
    lv_obj_add_style(label, &s_style_text_w, 0);
    lv_label_set_text(label, name_buf);

    /* 时间 + 大小（灰字，左下） */
    tm_info = localtime(&entry->timestamp);
    strftime(info_buf, 32, "%m-%d %H:%M", tm_info);
    snprintf(info_buf + strlen(info_buf), sizeof(info_buf) - strlen(info_buf),
             "  %llu MB",
             (unsigned long long)(entry->size / 1024 / 1024));
    label = lv_label_create(row);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 16);
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
}

/*****************************************************************************
 * 函数名称：ui_library_rebuild
 * 功能描述：重建文件库列表（清空后按最新优先填充，限量条目控制内存）
 *****************************************************************************/
static void ui_library_rebuild(void)
{
    video_entry_t entries[UI_LIBRARY_MAX_ITEMS];
    char title_buf[64];
    int count;
    int i;

    if (NULL == s_lib_ui) {
        return;
    }

    count = file_mgr_get_list(s_lib_ui->file_mgr, entries,
                              UI_LIBRARY_MAX_ITEMS);
    if (0 > count) {
        return;
    }

    /* 文件数未变化则跳过重建（每 5 分钟切段才变一次） */
    if (count == s_lib_last_count) {
        return;
    }
    s_lib_last_count = count;

    lv_obj_clean(s_lib_list);
    for (i = 0; i < count; i++) {
        ui_library_add_row(s_lib_list, &entries[i]);
    }

    snprintf(title_buf, sizeof(title_buf), "录像文件库 (%d)", count);
    lv_label_set_text(s_lib_title, title_buf);
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

    /* 顶栏（标题 label 保存供刷新文件数） */
    bar = ui_page_title_bar(content, "录像文件库");
    s_lib_title = lv_obj_get_child(bar, 0);

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

    /* 立即重建一次 + 每秒检测变化 */
    s_lib_last_count = -1;
    ui_library_rebuild();
    lv_timer_create(ui_library_refresh_tick, 1000, NULL);

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
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    label = lv_label_create(card);
    lv_obj_add_style(label, &s_style_text_g, 0);
    lv_label_set_text(label, title);

    return card;
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
    char buf[64];

    ui_pages_styles_init(ui->font);

    /* 页面背景 + 顶栏 */
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_COLOR_BG), 0);
    content = ui_page_content_create(page);
    ui_page_title_bar(content, "设置");

    /* 录像参数分组 */
    card = ui_settings_group_create(content, "录像参数");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, UI_PAGE_TOP_BAR_H + 16);
    ui_settings_add_item(card, "分辨率", "1920×1080 @30fps");
    ui_settings_add_item(card, "码率", "8 Mbps");
    ui_settings_add_item(card, "分段时长", "5 分钟");

    /* 存储分组 */
    card = ui_settings_group_create(content, "存储");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, UI_PAGE_TOP_BAR_H + 210);
    snprintf(buf, sizeof(buf), "%d 个文件 / %llu MB",
             file_mgr_get_count(ui->file_mgr),
             (unsigned long long)(file_mgr_get_used(ui->file_mgr)
                                  / 1024 / 1024));
    ui_settings_add_item(card, "SD 卡", buf);
    ui_settings_add_item(card, "格式化", "开发中");

    /* 关于分组 */
    card = ui_settings_group_create(content, "关于");
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, UI_PAGE_TOP_BAR_H + 390);
    ui_settings_add_item(card, "固件版本", "v0.4.0");
    ui_settings_add_item(card, "平台", "RV1126B + IMX415");

    /* 右侧导航栏（全局组件） */
    ui_nav_bar_create(page, ui);

    return page;
}
