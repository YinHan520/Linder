/* Finder-render：离线渲染工具（统一走 .DS_Store）
 * 渲染出「文件管理器主窗口」的静态快照：顶部 HeaderBar + 左侧侧边栏 + 主区图标网格 + 背景图铺底 + 状态栏。
 * 用于验证真实窗口的视觉效果（不需要 X display）。
 * 用法：Finder-render <目录> <输出.png>
 */
#include "dsstore.h"
#include "dsstore_parse.h"

#include <cairo.h>
#include <gtk/gtk.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+r, y+r, r, 3.14159, 3*3.14159/2);
    cairo_arc(cr, x+w-r, y+r, r, 3*3.14159/2, 0);
    cairo_arc(cr, x+w-r, y+h-r, r, 0, 3.14159/2);
    cairo_arc(cr, x+r, y+h-r, r, 3.14159/2, 3.14159);
    cairo_close_path(cr);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <目录> <输出.png>\n", argv[0]);
        return 1;
    }
    const char *dir = argv[1];
    const char *out = argv[2];

    DsStoreData data;
    dsdata_init(&data);
    char ds_path[4096];
    snprintf(ds_path, sizeof(ds_path), "%s/.DS_Store", dir);
    dsstore_parse_file(ds_path, &data);

    int W = 1000, H = 640;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(surf);

    /* 顶部 HeaderBar 高度 */
    int HEADER = 52;
    int SIDEBAR_W = 180;
    int STATUS_H = 30;

    /* ===== 1. 顶部 HeaderBar（深色） ===== */
    cairo_set_source_rgb(cr, 0.13, 0.13, 0.15);
    cairo_rectangle(cr, 0, 0, W, HEADER);
    cairo_fill(cr);

    /* 导航按钮（后退/前进/向上） */
    double btn_x = 12;
    for (int i = 0; i < 3; i++) {
        rounded_rect(cr, btn_x, 10, 32, 32, 6);
        cairo_set_source_rgb(cr, 0.22, 0.22, 0.25);
        cairo_fill(cr);
        /* 箭头符号 */
        cairo_set_source_rgb(cr, 0.85, 0.85, 0.88);
        cairo_set_font_size(cr, 16);
        cairo_move_to(cr, btn_x + 10, 34);
        const char *sym = (i == 0) ? "◀" : (i == 1 ? "▶" : "▲");
        cairo_show_text(cr, sym);
        btn_x += 40;
    }

    /* 地址栏 */
    double ax = btn_x + 8;
    rounded_rect(cr, ax, 10, W - ax - 60, 32, 6);
    cairo_set_source_rgb(cr, 0.18, 0.18, 0.20);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.92, 1.0);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, ax + 14, 32);
    cairo_show_text(cr, dir);

    /* 搜索框 */
    rounded_rect(cr, W - 52, 10, 40, 32, 6);
    cairo_set_source_rgb(cr, 0.18, 0.18, 0.20);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.75);
    cairo_set_font_size(cr, 16);
    cairo_move_to(cr, W - 44, 34);
    cairo_show_text(cr, "🔍");

    /* ===== 2. 左侧侧边栏 ===== */
    cairo_set_source_rgb(cr, 0.20, 0.20, 0.22);
    cairo_rectangle(cr, 0, HEADER, SIDEBAR_W, H - HEADER - STATUS_H);
    cairo_fill(cr);

    cairo_set_font_size(cr, 15);
    double sy = HEADER + 28;
    const char *items[] = {"🏠 主页", "⬇ 下载", "📄 文档", "🖼 图片", "🖥 此电脑", "🗑 回收站"};
    for (int i = 0; i < 6; i++) {
        cairo_set_source_rgba(cr, 1, 1, 1, (i == 0) ? 0.95 : 0.65);
        cairo_move_to(cr, 20, sy);
        cairo_show_text(cr, items[i]);
        sy += 38;
    }

    /* ===== 3. 主区：背景图铺满最底层 ===== */
    double mx = SIDEBAR_W, my = HEADER;
    double mw = W - SIDEBAR_W, mh = H - HEADER - STATUS_H;

    /* 先铺背景图（cover）——这块就是 view 窗口里 GtkOverlay 底层做的事 */
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
    cairo_rectangle(cr, mx, my, mw, mh);
    cairo_fill(cr);

    if (data.bg_pict_path && data.bg_pict_path[0]) {
        GError *err = NULL;
        GdkPixbuf *pix = gdk_pixbuf_new_from_file(data.bg_pict_path, &err);
        if (pix) {
            int pw = gdk_pixbuf_get_width(pix);
            int ph = gdk_pixbuf_get_height(pix);
            double scale = fmax(mw / (double)pw, mh / (double)ph);
            int dw = (int)(pw * scale), dh = (int)(ph * scale);
            GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pix, dw, dh, GDK_INTERP_BILINEAR);
            g_object_unref(pix);
            if (scaled) {
                gdk_cairo_set_source_pixbuf(cr, scaled, mx + (mw - dw) / 2, my + (mh - dh) / 2);
                cairo_paint(cr);
                g_object_unref(scaled);
            }
        } else if (err) {
            fprintf(stderr, "背景图加载失败 %s: %s\n", data.bg_pict_path, err->message);
            g_error_free(err);
        }
    }

    /* 纯色背景叠加 */
    if (data.bg_type == DSBG_COLOR) {
        double r = data.bg_r / 65535.0, g = data.bg_g / 65535.0, b = data.bg_b / 65535.0;
        cairo_set_source_rgba(cr, r, g, b, 0.45);
        cairo_rectangle(cr, mx, my, mw, mh);
        cairo_fill(cr);
    }

    /* ===== 4. 图标网格（浮在背景图上） ===== */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        int n = 0;
        double ix = mx + 30, iy = my + 26;
        while ((de = readdir(d)) && n < 24) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strcmp(de->d_name, ".DS_Store") == 0) continue;
            n++;
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
            struct stat st;
            int isdir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));

            /* 图标（圆角方块） */
            double icon_sz = 52;
            rounded_rect(cr, ix, iy, icon_sz, icon_sz, 8);
            cairo_set_source_rgba(cr, isdir ? 0.35 : 0.30, isdir ? 0.62 : 0.55, isdir ? 0.78 : 0.92, 0.95);
            cairo_fill(cr);
            /* 图标高光 */
            rounded_rect(cr, ix, iy, icon_sz, icon_sz * 0.4, 8);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
            cairo_fill(cr);
            /* 图标里的图形（文件夹=文件夹形状，文件=折角） */
            cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
            if (isdir) {
                rounded_rect(cr, ix + 14, iy + 18, 24, 18, 3);
                cairo_fill(cr);
            } else {
                cairo_rectangle(cr, ix + 16, iy + 18, 20, 20);
                cairo_fill(cr);
            }

            /* 文件名（白字带阴影，浮在背景图上） */
            cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
            cairo_set_font_size(cr, 12);
            cairo_move_to(cr, ix + 1, iy + icon_sz + 17);
            cairo_show_text(cr, de->d_name);
            cairo_set_source_rgba(cr, 1, 1, 1, 1.0);
            cairo_move_to(cr, ix, iy + icon_sz + 16);
            cairo_show_text(cr, de->d_name);

            /* 网格换行 */
            ix += 110;
            if (ix + 110 > mx + mw) {
                ix = mx + 30;
                iy += 96;
            }
        }
        closedir(d);
    }

    /* ===== 5. 底部状态栏 ===== */
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.17);
    cairo_rectangle(cr, 0, H - STATUS_H, W, STATUS_H);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, 12, H - 10);
    char info[1024];
    snprintf(info, sizeof(info), "就绪  |  背景图: %s",
             data.bg_pict_path ? data.bg_pict_path : "(无)");
    cairo_show_text(cr, info);

    cairo_surface_write_to_png(surf, out);
    printf("已渲染: %s\n", out);

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    dsdata_free(&data);
    return 0;
}
