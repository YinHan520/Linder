#ifndef DSSTORE_PARSE_H
#define DSSTORE_PARSE_H

/*
 * dsstore_parse.h —— macOS .DS_Store 二进制格式解析器（纯 C）
 *
 * 参考逆向文档：
 *   - Mozilla Wiki "DS_Store File Format"（Mark Mentovai 逆向）
 *   - 0day.work "Parsing the .DS_Store file format"
 *   - Mac-Finder-DSStore (Wim Lewis) DSStoreFormat.pod
 *
 * 格式要点：
 *   头部 36 字节 = 0x01 + magic "Bud1"(0x42756431) + offset(2×) + size + 16 保留
 *   Root block = offsets 列表 + TOC(DSDB) + free list
 *   B 树遍历 block/记录：
 *     记录 = [文件名长:4][文件名:UTF16BE][structure-id:4][structure-type:4][数据...]
 *   structure-type 关键值：
 *     "BKGD"(blob) = 文件夹背景（DefB=默认 / ClrB=纯色RGB / PctB=图片）
 *     "pict"(blob) = 背景图片（Alias 记录，解析到图片文件路径）
 *     "bwsp"(blob) = 窗口布局（bplist），不是背景图！
 *     "icvp"/"lsvp" = 图标/列表视图设置（plist）
 *     "icvo"/"icvt" = 图标视图选项/文字大小
 *     "Iloc"(blob) = 图标位置
 *     "fwi0" = Finder 窗口信息
 */

#include <stdint.h>
#include <stdio.h>

/* 背景类型 */
typedef enum {
    DSBG_NONE = 0,   /* 无背景 */
    DSBG_COLOR,      /* 纯色背景（RGB） */
    DSBG_PICTURE     /* 图片背景 */
} DsBgType;

/* 从 .DS_Store 解析出的自定义信息 */
typedef struct {
    DsBgType bg_type;      /* 背景类型 */
    uint16_t bg_r, bg_g, bg_b; /* 纯色背景（0-65535，mac 16位），仅 bg_type==COLOR 有效 */
    char *bg_pict;         /* 背景图 alias（pict blob 的 raw alias 数据），可 NULL */
    size_t bg_pict_len;
    char *bg_pict_path;    /* 背景图片的实际路径（从 book 的 0x1004 解析出），可 NULL */
    char *view;            /* 视图模式 "icon"/"list"，可 NULL */
    long   icon_size;      /* 图标大小，-1 未设置 */
    int    has_icon_pos;   /* 是否有图标位置 */
    int    icon_x, icon_y; /* 图标位置（中心点） */
} DsStoreData;

/* 初始化 */
void dsdata_init(DsStoreData *d);
/* 释放 */
void dsdata_free(DsStoreData *d);

/*
 * 解析 .DS_Store 文件。
 * path: .DS_Store 文件路径。
 * out:  输出解析结果（调用前需 dsdata_init，或由本函数自动 init）。
 * 返回 0 成功，-1 失败（文件不存在/不是合法 .DS_Store）。
 */
int dsstore_parse_file(const char *path, DsStoreData *out);

#endif /* DSSTORE_PARSE_H */
