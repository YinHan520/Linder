#ifndef DSSTORE_WRITE_H
#define DSSTORE_WRITE_H

/*
 * dsstore_write.h —— macOS .DS_Store 二进制格式【写入器】（纯 C）
 *
 * 这是「双向兼容 mac」的另一半：把自定义字段（背景/视图/图标）写成标准
 * .DS_Store 二进制，让 mac Finder 能认，也让本项目的 parse 能读回。
 *
 * 格式逆向来源（与 parser 对称）：
 *   - Wim Lewis 的 DSStoreFormat.pod（权威）
 *   - Mozilla Wiki DS_Store_File_Format（Mark Mentovai）
 *   - 0day.work 逆向
 *   - 真实样本 fixture.DS_Store（sindresorhus）
 *
 * 写入采用「最小自洽布局」（每目录一个小文件，无需复杂动态分配）：
 *   block 0 = allocator 元数据（Bud1 头指向它）
 *   block 1 = B-tree header（DSDB 指向它）
 *   block 2 = 叶节点（放全部记录）
 */

#include <stdint.h>
#include <stdio.h>

/* 写入参数：一套 .DS_Store 要写的字段（未用到填 0/NULL 即跳过） */
typedef struct {
    int      has_bg;        /* 是否写背景 */
    int      bg_is_picture; /* 1=图片背景(PctB+pict) 0=纯色(ClrB) */
    /* 纯色背景 RGB（0-65535，mac 16 位）。图片背景时忽略。 */
    uint16_t bg_r, bg_g, bg_b;
    /* 图片背景：图片文件的绝对路径（用于生成 pict 的 Alias 记录）。
       若路径为空但 bg_is_picture=1，仍写 PctB（pict 记录略）。 */
    const char *bg_pict_path;

    int      has_view;      /* 是否写视图 */
    int      view_is_icon;  /* 1=图标视图(icvp) 0=列表视图(lsvp) */

    int      has_icon_size; /* 是否写图标大小 */
    uint16_t icon_size;     /* 像素，如 48 / 64 / 96 */

    int      has_icon_pos;  /* 是否写图标位置 */
    int      icon_x, icon_y;/* 图标中心点坐标 */
} DsStoreWrite;

/*
 * 写入 .DS_Store 文件。
 * path: 目标文件路径（一般 /目录/.DS_Store）
 * w:    写入参数
 * 返回 0 成功，非 0 失败。
 */
int dsstore_write_file(const char *path, const DsStoreWrite *w);

#endif /* DSSTORE_WRITE_H */
