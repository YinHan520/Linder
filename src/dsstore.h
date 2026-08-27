#ifndef DSSTORE_H
#define DSSTORE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 魔数：8 字节，标识这是 ds_store 元数据文件 */
#define DSSTORE_MAGIC   "DSSTORE1"
#define DSSTORE_MAGIC_LEN 8

/* 文件名（银寒钦定 2026-08-19：改成标准 .DS_Store，跟 mac 一模一样） */
#define DSSTORE_FILE_VISIBLE ".DS_Store"   /* 标准名（跟 mac 一致，带点=隐藏） */
#define DSSTORE_FILE_HIDDEN  ".DS_Store"   /* 与 visible 相同；项目只支持标准 .DS_Store，不区分显隐 */

/* 元数据键（对齐 mac .DS_Store 语义） */
#define KEY_ICON      "icon"
#define KEY_VIEW      "view"
#define KEY_ICON_SIZE "icon_size"
#define KEY_BACKGROUND "background"  /* 背景图路径（对齐 mac 的 bwsp/BKGD+pict） */
#define KEY_COLOR      "color"       /* 配色（对齐 mac background color，支持 0-65535 或 #RRGGBB） */

/* 视图模式 */
#define VIEW_LIST "list"
#define VIEW_GRID "grid"

/* 单个目录的元数据结构 */
typedef struct {
    char *icon;       /* 自定义图标路径（可 NULL） */
    char *view;       /* 视图模式（可 NULL） */
    long  icon_size;  /* 图标大小（-1 未设置） */
    char *background; /* 文件夹背景图路径（可 NULL） */
    char *color;      /* 配色（可 NULL） */
} DsStoreMeta;

/* 初始化/清空元数据 */
void meta_init(DsStoreMeta *m);
void meta_free(DsStoreMeta *m);

/* 目录内 ds_store 文件固定名（返回新分配内存，调用者 free） */
char *dsstore_path_for(const char *dir, int visible);

/* 探测目录内实际存在的 ds_store 文件，返回实际路径或 NULL */
char *dsstore_find(const char *dir);

/* 读取目录内的 ds_store 文件（自动探测 .dsstore / dsstore 两种） */
int dsstore_read(const char *dir, DsStoreMeta *out);

/* 写入目录内的 ds_store 文件（用 show/hide 状态决定带不带点） */
int dsstore_write(const char *dir, const DsStoreMeta *meta, int visible);

/* 切换可见性：visible=1 → 显示(ds_store)，visible=0 → 隐藏(.dsstore) */
int dsstore_set_visible(const char *dir, int visible);

/* 拉屎开关：查询（默认关），设置 on=1/off=0 */
int dsstore_poop_enabled(void);
int dsstore_set_poop(int on);

/* 应用元数据到 Linux（调用 gio set metadata::custom-icon） */
int apply_icon(const char *dir, const char *icon);

#endif /* DSSTORE_H */
