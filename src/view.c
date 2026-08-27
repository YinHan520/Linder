








#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dsstore.h"
#include "dsstore_parse.h"
#include "dsstore_write.h"
#include "view.h"
#include "settings.h"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <math.h>
#include <locale.h>

/* ---- i18n: 默认跟系统 locale 走，非中文（zh*）一律英文。----
 * tr(中文, english)：中文 locale 返回中文，否则返回英文。 */
static int g_zh = -1;
static int lang_is_zh(void) {
    if (g_zh >= 0) return g_zh;
    /* 设置面板强制语言优先 */
    int force = settings_lang_force();
    if (force >= 0) {
        g_zh = force;
        return g_zh;
    }
    const char *l = NULL;
    l = g_getenv("LC_ALL");
    if (!l || !l[0]) l = g_getenv("LC_MESSAGES");
    if (!l || !l[0]) l = g_getenv("LANG");
    if (l && (g_ascii_strncasecmp(l, "zh", 2) == 0 ||
              g_ascii_strncasecmp(l, "zh_", 3) == 0))
        g_zh = 1;
    else
        g_zh = 0;
    return g_zh;
}
#define tr(zh, en) (lang_is_zh() ? (zh) : (en))


typedef struct {
    GtkWidget *window;
    GtkWidget *header;      /* GtkHeaderBar */
    GtkWidget *titlebar;    
    GtkWidget *title_label; 
    
    gboolean dragging;
    int drag_start_x, drag_start_y;
    int win_start_x, win_start_y;
    gboolean resizing;
    gboolean maximized;
    int pre_max_x, pre_max_y, pre_max_w, pre_max_h;
    GtkWidget *back_btn;
    GtkWidget *fwd_btn;
    GtkWidget *view_switch; 
    GtkWidget *breadcrumb;  
    GtkWidget *crumb_entry; 
    GtkWidget *icon_btn;    
    GtkWidget *list_btn;    
    GtkWidget *sidebar;     
    GtkWidget *overlay;     
    GtkWidget *bg;          
    GtkWidget *stack;       
    GtkWidget *iconview;    
    GtkWidget *listview;    
    GtkWidget *statusbar;   
    GtkWidget *zoom_scale;  
    int icon_size;           /* 图标/缩略图当前尺寸，随缩放滑块变 */
    gboolean rubber_active;
    gdouble rubber_x0, rubber_y0, rubber_x1, rubber_y1;

    
    GtkWidget *miller_box;    
    GtkWidget *miller_scroll; 
    GPtrArray  *miller_cols;  

    GtkListStore *icon_store; 
    GtkListStore *list_store; 
    DsStoreData data;       
    char *dir;              
    GPtrArray *clip_list;             
    int   clip_cut;         
    GPtrArray *hist;        
    int   hist_pos;         

    gboolean bg_dark;                  /* 背景是否偏暗：true=白字，false=深字 */
    double   bg_known_luma;            /* 已算出的背景亮度，<0 表示还没算 */
    GtkCssProvider *bg_css_provider;   /* 根据背景明暗动态生成的文字色 CSS */
} ViewState;

static ViewState g;


static const char *current_bg_path(void) {
    return (g.data.bg_pict_path && g.data.bg_pict_path[0])
        ? g.data.bg_pict_path : NULL;
}

/* 取一个背景图的平均亮度（0~255），失败返回 -1 */
static double compute_bg_luma(const GdkPixbuf *pix) {
    if (!pix) return -1.0;
    int n = gdk_pixbuf_get_n_channels(pix);
    int rs = gdk_pixbuf_get_rowstride(pix);
    int w = gdk_pixbuf_get_width(pix);
    int h = gdk_pixbuf_get_height(pix);
    const guchar *p = gdk_pixbuf_get_pixels(pix);
    if (!p || w <= 0 || h <= 0) return -1.0;
    /* 采样最多约 64k 像素，避免大图算太慢 */
    int step_x = (w > 256) ? (w / 256) : 1;
    int step_y = (h > 256) ? (h / 256) : 1;
    long cnt = 0;
    double sum = 0.0;
    for (int y = 0; y < h; y += step_y) {
        const guchar *row = p + (gsize)y * rs;
        for (int x = 0; x < w; x += step_x) {
            const guchar *px = row + x * n;
            double r = px[0], g_ = px[1], b = px[2];
            /* 感知亮度加权（人眼对绿更敏感） */
            sum += 0.299 * r + 0.587 * g_ + 0.114 * b;
            cnt++;
        }
    }
    return cnt ? (sum / cnt) : -1.0;
}

/* 根据背景明暗，动态生成一套控制文件名文字颜色的 CSS */
static void apply_bg_text_css(void) {
    const char *fg = g.bg_dark ? "rgba(255,255,255,0.94)" : "#1d1d1f";
    char css[1024];
    snprintf(css, sizeof(css),
        "iconview { color: %s; }"
        "iconview:selected { color: %s; }"
        "treeview { color: %s; }"
        "treeview:selected { color: %s; }"
        "treeview cell { color: %s; }"
        "treeview:selected cell { color: %s; }",
        fg, fg, fg, fg, fg, fg);

    if (!g.bg_css_provider) {
        g.bg_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(g.bg_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(g.bg_css_provider, css, -1, NULL);
}

static gboolean on_draw_bg(GtkWidget *widget, cairo_t *cr, gpointer unused) {
    (void)unused;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    if (w <= 0 || h <= 0) return FALSE;

    /* 默认底色（浅灰，模拟 mac 文件夹空背景） */
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
    cairo_paint(cr);

    /* 背景图铺满（cover）。bg_pict_path 即解析出的图片真实路径 */
    const char *bgp = current_bg_path();
    if (bgp) {
        GError *err = NULL;
        GdkPixbuf *pix = gdk_pixbuf_new_from_file(bgp, &err);
        if (pix) {
            /* 根据背景图平均亮度动态切换文件名文字颜色（暗背景用白字） */
            double luma = compute_bg_luma(pix);
            double known = (g.bg_known_luma < 0) ? -1.0 : g.bg_known_luma;
            if (luma >= 0 && (known < 0 || fabs(luma - known) > 8.0)) {
                g.bg_dark = (luma < 128.0);
                g.bg_known_luma = luma;
                apply_bg_text_css();
                if (g.stack) gtk_widget_queue_draw(g.stack);
            }
            int pw = gdk_pixbuf_get_width(pix);
            int ph = gdk_pixbuf_get_height(pix);
            double scale = fmax((double)w / pw, (double)h / ph);
            int dw = (int)(pw * scale);
            int dh = (int)(ph * scale);
            GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pix, dw, dh, GDK_INTERP_BILINEAR);
            if (scaled) {
                gdk_cairo_set_source_pixbuf(cr, scaled, (w - dw) / 2, (h - dh) / 2);
                cairo_paint(cr);
                g_object_unref(scaled);
            }
            g_object_unref(pix);
        } else if (err) {
            g_printerr("Linder: failed to load background %s: %s\n", bgp, err->message);
            g_error_free(err);
        }
    } else {
        /* 无背景图：回深色文字 */
        if (g.bg_dark || g.bg_known_luma >= 0) {
            g.bg_dark = FALSE;
            g.bg_known_luma = -1.0;
            apply_bg_text_css();
            if (g.stack) gtk_widget_queue_draw(g.stack);
        }
    }

    /* 配色叠加（半透明色调，模拟 mac 背景色，叠加在底色上） */
    if (g.data.bg_type == DSBG_COLOR) {
        double r = g.data.bg_r / 65535.0;
        double gg = g.data.bg_g / 65535.0;
        double b = g.data.bg_b / 65535.0;
        /* 纯色背景用完全不透明覆盖，否则叠加后几乎不可见（看起来像“没反应”） */
        cairo_set_source_rgba(cr, r, gg, b, 1.0);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_fill(cr);

        /* 无图时，根据纯色明暗决定文字色 */
        if (!bgp) {
            double luma = 0.299 * r + 0.587 * gg + 0.114 * b;
            gboolean dark = (luma < 0.5);
            if (dark != g.bg_dark) {
                g.bg_dark = dark;
                g.bg_known_luma = luma * 255.0;
                apply_bg_text_css();
                if (g.stack) gtk_widget_queue_draw(g.stack);
            }
        }
    }

    return FALSE;
}


static void reload_dsstore(void) {
    dsdata_free(&g.data);
    dsdata_init(&g.data);
    char ds[4096];
    snprintf(ds, sizeof(ds), "%s/.DS_Store", g.dir);
    dsstore_parse_file(ds, &g.data);
}

/* write background (color or picture) into .DS_Store, then reload+redraw */
static void write_bg_meta(int is_picture, uint16_t r, uint16_t gg, uint16_t b,
                          const char *pict_path) {
    char ds[4096];
    snprintf(ds, sizeof(ds), "%s/.DS_Store", g.dir);
    DsStoreWrite w;
    memset(&w, 0, sizeof(w));
    w.has_bg = 1;
    w.bg_is_picture = is_picture;
    w.bg_r = r; w.bg_g = gg; w.bg_b = b;
    w.bg_pict_path = pict_path;
    int wr = dsstore_write_file(ds, &w);
    if (wr != 0)
        g_printerr("Linder: failed to write .DS_Store %s\n", ds);
    reload_dsstore();
    if (g.overlay) gtk_widget_queue_draw(g.overlay); else gtk_widget_queue_draw(g.bg);
}

static void do_set_bg_image(void) {
    GtkWidget *fc = gtk_file_chooser_dialog_new(tr("选择背景图片", "Choose Background Image"), GTK_WINDOW(g.window),
        GTK_FILE_CHOOSER_ACTION_OPEN, tr("取消", "Cancel"), GTK_RESPONSE_CANCEL,
        tr("打开", "Open"), GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, tr("图片", "Images"));
    gtk_file_filter_add_pixbuf_formats(f);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f);
    if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        if (path) { write_bg_meta(1, 0, 0, 0, path); g_free(path); }
    }
    gtk_widget_destroy(fc);
}

static void do_set_bg_color(void) {
    GtkWidget *cc = gtk_color_chooser_dialog_new(tr("选择背景颜色", "Choose Background Color"), GTK_WINDOW(g.window));
    gint resp = gtk_dialog_run(GTK_DIALOG(cc));
    if (resp == GTK_RESPONSE_OK) {
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cc), &c);
        write_bg_meta(0, (uint16_t)(c.red * 65535.0),
                         (uint16_t)(c.green * 65535.0),
                         (uint16_t)(c.blue * 65535.0), NULL);
    }
    gtk_widget_destroy(cc);
}

static void do_clear_bg(void) {
    char ds[4096];
    snprintf(ds, sizeof(ds), "%s/.DS_Store", g.dir);
    DsStoreWrite w;
    memset(&w, 0, sizeof(w));
    w.has_bg = 0; /* overwrite without background */
    dsstore_write_file(ds, &w);
    reload_dsstore();
    if (g.overlay) gtk_widget_queue_draw(g.overlay); else gtk_widget_queue_draw(g.bg);
}


static void human_size(off_t bytes, char *out, size_t outlen) {
    double v = (double)bytes;
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, outlen, "%lld B", (long long)bytes);
    else snprintf(out, outlen, "%.1f %s", v, units[i]);
}


static double disk_free_gb(void) {
    struct statvfs st;
    if (statvfs(g.dir, &st) != 0) return 0.0;
    return (double)st.f_bavail * (double)st.f_frsize / (1024.0 * 1024.0 * 1024.0);
}


static int is_image_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    const char *ext = dot + 1;
    return (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
            strcasecmp(ext, "png") == 0 || strcasecmp(ext, "gif") == 0 ||
            strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
            strcasecmp(ext, "tiff") == 0 || strcasecmp(ext, "tif") == 0);
}

/* 图片文件生成缩略图：缩到 size 内、保持比例、居中在透明画布上 */
static GdkPixbuf *load_image_thumb(const char *full, int size) {
    GError *err = NULL;
    GdkPixbuf *src = gdk_pixbuf_new_from_file_at_scale(full, size, size, TRUE, &err);
    if (!src) {
        g_clear_error(&err);
        return NULL;
    }
    int w = gdk_pixbuf_get_width(src);
    int h = gdk_pixbuf_get_height(src);

    /* 透明画布 size×size，缩略图居中 */
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    double r = 4.0;
    int x = (size - w) / 2;
    int y = (size - h) / 2;

    /* 圆角裁剪 */
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_clip(cr);

    gdk_cairo_set_source_pixbuf(cr, src, x, y);
    cairo_paint(cr);

    GdkPixbuf *out = gdk_pixbuf_get_from_surface(surf, 0, 0, size, size);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    g_object_unref(src);
    return out;
}

static const char *entry_icon(const char *name, int isdir) {
    if (isdir) return "folder";
    const char *dot = strrchr(name, '.');
    if (!dot) return "text-x-generic";
    const char *ext = dot + 1;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
        strcasecmp(ext, "svg") == 0)
        return "image-x-generic";
    if (strcasecmp(ext, "txt") == 0)
        return "text-x-generic";
    if (strcasecmp(ext, "md") == 0)
        return "text-x-markdown";
    if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 ||
        strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "m4a") == 0 ||
        strcasecmp(ext, "ogg") == 0)
        return "audio-x-generic";
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mkv") == 0 ||
        strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mov") == 0)
        return "video-x-generic";
    if (strcasecmp(ext, "c") == 0 || strcasecmp(ext, "h") == 0 ||
        strcasecmp(ext, "cpp") == 0 || strcasecmp(ext, "py") == 0 ||
        strcasecmp(ext, "js") == 0 || strcasecmp(ext, "sh") == 0)
        return "text-x-source";
    if (strcasecmp(ext, "docx") == 0 || strcasecmp(ext, "doc") == 0 ||
        strcasecmp(ext, "odt") == 0 || strcasecmp(ext, "rtf") == 0)
        return "x-office-document";
    if (strcasecmp(ext, "xlsx") == 0 || strcasecmp(ext, "xls") == 0 ||
        strcasecmp(ext, "ods") == 0 || strcasecmp(ext, "csv") == 0)
        return "x-office-spreadsheet";
    if (strcasecmp(ext, "pptx") == 0 || strcasecmp(ext, "ppt") == 0 ||
        strcasecmp(ext, "odp") == 0)
        return "x-office-presentation";
    if (strcasecmp(ext, "pdf") == 0)
        return "application-pdf";
    if (strcasecmp(ext, "zip") == 0 || strcasecmp(ext, "tar") == 0 ||
        strcasecmp(ext, "gz") == 0 || strcasecmp(ext, "7z") == 0 ||
        strcasecmp(ext, "rar") == 0)
        return "package-x-generic";
    return "text-x-generic";
}


static const char *entry_type(const char *name, int isdir) {
    if (isdir) return tr("文件夹", "Folder");
    const char *dot = strrchr(name, '.');
    if (!dot) return tr("文件", "File");
    const char *ext = dot + 1;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0 ||
        strcasecmp(ext, "png") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0)
        return tr("图片", "Image");
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "md") == 0)
        return tr("文本文件", "Text File");
    if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 ||
        strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "m4a") == 0)
        return tr("音频", "Audio");
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mkv") == 0 ||
        strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mov") == 0)
        return tr("视频", "Video");
    if (strcasecmp(ext, "c") == 0 || strcasecmp(ext, "h") == 0)
        return tr("C 源文件", "C Source");
    if (strcasecmp(ext, "docx") == 0 || strcasecmp(ext, "doc") == 0 ||
        strcasecmp(ext, "odt") == 0)
        return tr("Word 文档", "Word Document");
    if (strcasecmp(ext, "xlsx") == 0 || strcasecmp(ext, "xls") == 0 ||
        strcasecmp(ext, "ods") == 0 || strcasecmp(ext, "csv") == 0)
        return tr("Excel 表格", "Excel Sheet");
    if (strcasecmp(ext, "pptx") == 0 || strcasecmp(ext, "ppt") == 0 ||
        strcasecmp(ext, "odp") == 0)
        return tr("PowerPoint 演示文稿", "PowerPoint");
    if (strcasecmp(ext, "rtf") == 0)
        return tr("富文本", "Rich Text");
    if (strcasecmp(ext, "pdf") == 0)
        return tr("PDF 文档", "PDF Document");
    if (strcasecmp(ext, "zip") == 0 || strcasecmp(ext, "tar") == 0 ||
        strcasecmp(ext, "gz") == 0 || strcasecmp(ext, "7z") == 0 ||
        strcasecmp(ext, "rar") == 0)
        return tr("压缩文件", "Archive");
    return tr("文件", "File");
}


static int entry_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    
    int da = (sa[0] == '/'); 
    int db = (sb[0] == '/');
    if (da != db) return da ? -1 : 1;
    return strcasecmp(sa, sb);
}


static void on_list_selection_changed(GtkTreeSelection *sel, gpointer u) {
    (void)u;
    int cnt = gtk_tree_selection_count_selected_rows(sel);
    int total = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(g.list_store), NULL);
    char status[128];
    if (cnt > 0)
        snprintf(status, sizeof(status), tr("选择了 %d 项（共 %d 项），%.2f GB 可用", "%d of %d selected, %.2f GB free"), cnt, total, disk_free_gb());
    else
        snprintf(status, sizeof(status), tr("%d 个项目，%.2f GB 可用", "%d items, %.2f GB free"), total, disk_free_gb());
    gtk_label_set_text(GTK_LABEL(g.statusbar), status);
}


static void on_icon_selection_changed(GtkIconView *iv, gpointer u) {
    (void)u;
    GList *paths = gtk_icon_view_get_selected_items(iv);
    int cnt = g_list_length(paths);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    int total = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(g.icon_store), NULL);
    char status[128];
    if (cnt > 0)
        snprintf(status, sizeof(status), tr("选择了 %d 项（共 %d 项），%.2f GB 可用", "%d of %d selected, %.2f GB free"), cnt, total, disk_free_gb());
    else
        snprintf(status, sizeof(status), tr("%d 个项目，%.2f GB 可用", "%d items, %.2f GB free"), total, disk_free_gb());
    gtk_label_set_text(GTK_LABEL(g.statusbar), status);
}


static void miller_rebuild(void);  


/* 通用染色：只染图标的不透明区域（保留原有 alpha 形状/圆角），
 * 用 CAIRO_OPERATOR_ATOP 叠加目标色。 */
static GdkPixbuf *tint_color(GdkPixbuf *src, double r, double g, double b) {
    int w = gdk_pixbuf_get_width(src);
    int h = gdk_pixbuf_get_height(src);
    if (w < 1 || h < 1) return src;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);

    gdk_cairo_set_source_pixbuf(cr, src, 0, 0);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_ATOP);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);

    GdkPixbuf *tinted = gdk_pixbuf_get_from_surface(surf, 0, 0, w, h);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    g_object_unref(src);
    return tinted;
}

/* 染 mac 蓝 #76D2FB（文件夹图标用） */
static GdkPixbuf *tint_mac_blue(GdkPixbuf *src) {
    return tint_color(src, 118.0 / 255.0, 210.0 / 255.0, 251.0 / 255.0);
}

/* 染 mac 侧边栏粉 #E45A8E */
static GdkPixbuf *tint_pink(GdkPixbuf *src) {
    return tint_color(src, 228.0 / 255.0, 90.0 / 255.0, 142.0 / 255.0);
}

static GdkPixbuf *load_folder_pixbuf(int size) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GdkPixbuf *icon = gtk_icon_theme_load_icon(theme, "folder", size,
                                    GTK_ICON_LOOKUP_GENERIC_FALLBACK, NULL);
    if (!icon) return NULL;
    return tint_mac_blue(icon);
}

static void refresh_views(void) {
    gtk_list_store_clear(g.icon_store);
    gtk_list_store_clear(g.list_store);

    DIR *d = opendir(g.dir);
    if (!d) return;

    
    struct dirent *de;
    char **names = malloc(sizeof(char*) * 4096);
    int n = 0;
    while ((de = readdir(d)) != NULL && n < 4096) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (strcmp(de->d_name, ".DS_Store") == 0) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", g.dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        
        if (S_ISDIR(st.st_mode)) {
            names[n] = malloc(strlen(de->d_name) + 2);
            names[n][0] = '/';
            strcpy(names[n] + 1, de->d_name);
        } else {
            names[n] = strdup(de->d_name);
        }
        n++;
    }
    closedir(d);

    qsort(names, n, sizeof(char*), entry_cmp);

    GtkIconTheme *theme = gtk_icon_theme_get_default();
    for (int i = 0; i < n; i++) {
        int isdir = (names[i][0] == '/');
        const char *name = names[i] + (isdir ? 1 : 0);

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", g.dir, name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        const char *icon = entry_icon(name, isdir);
        int isize = g.icon_size > 0 ? g.icon_size : 48;
        GdkPixbuf *pix = NULL;
        if (isdir) {
            pix = load_folder_pixbuf(isize);
        } else if (is_image_file(name)) {
            pix = load_image_thumb(full, isize);
        }
        if (!pix)
            pix = gtk_icon_theme_load_icon(theme, icon, isize,
                                           GTK_ICON_LOOKUP_GENERIC_FALLBACK, NULL);
        if (!pix)
            pix = gtk_icon_theme_load_icon(theme, "text-x-generic", isize, 0, NULL);

        
        GtkTreeIter it;
        gtk_list_store_append(g.icon_store, &it);
        gtk_list_store_set(g.icon_store, &it,
                           0, pix, 1, name, -1);
        if (pix) g_object_unref(pix);

        
        GdkPixbuf *pix2 = NULL;
        if (isdir) {
            pix2 = load_folder_pixbuf(24);
        } else if (is_image_file(name)) {
            pix2 = load_image_thumb(full, 24);
        }
        if (!pix2)
            pix2 = gtk_icon_theme_load_icon(theme, icon, 24,
                                            GTK_ICON_LOOKUP_GENERIC_FALLBACK, NULL);
        if (!pix2) pix2 = gtk_icon_theme_load_icon(theme, "text-x-generic", 24, 0, NULL);
        char sz[64];
        if (isdir) snprintf(sz, sizeof(sz), tr("文件夹", "Folder"));
        else human_size(st.st_size, sz, sizeof(sz));
        char mtime[64];
        struct tm tmv;
        localtime_r(&st.st_mtime, &tmv);
        strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M", &tmv);

        GtkTreeIter it2;
        gtk_list_store_append(g.list_store, &it2);
        gtk_list_store_set(g.list_store, &it2,
                           0, pix2, 1, name, 2, sz,
                           3, entry_type(name, isdir), 4, mtime, -1);
        if (pix2) g_object_unref(pix2);
    }

    for (int i = 0; i < n; i++) free(names[i]);
    free(names);

    char status[128];
    snprintf(status, sizeof(status), tr("%d 个项目，%.2f GB 可用", "%d items, %.2f GB free"), n, disk_free_gb());
    gtk_label_set_text(GTK_LABEL(g.statusbar), status);

    if (g.overlay) gtk_widget_queue_draw(g.overlay); else gtk_widget_queue_draw(g.bg);

    
    if (g.miller_box)
        miller_rebuild();
}


static void navigate_to(const char *path); 


static void on_crumb_clicked(GtkButton *b, gpointer u) {
    (void)b;
    const char *path = u;
    if (path) navigate_to(path);
}


static void crumb_data_free(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void refresh_breadcrumb(void) {
    if (!g.breadcrumb) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(g.breadcrumb));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    
    char *path = g_strdup(g.dir);
    char *saved = path;
    
    if (strcmp(path, "/") == 0) {
        GtkWidget *root = gtk_button_new_with_label("/");
        gtk_button_set_relief(GTK_BUTTON(root), GTK_RELIEF_NONE);
        g_signal_connect_data(root, "clicked", G_CALLBACK(on_crumb_clicked),
                              g_strdup("/"), crumb_data_free, 0);
        gtk_box_pack_start(GTK_BOX(g.breadcrumb), root, FALSE, FALSE, 0);
        g_free(saved);
        gtk_widget_show_all(g.breadcrumb);
        return;
    }

    
    char *parts[256];
    char *acc[256]; 
    int n = 0;
    char *p = path;
    if (*p == '/') p++; 
    parts[n] = "/";
    acc[n] = g_strdup("/");
    n++;
    char *tok = strtok(p, "/");
    while (tok && n < 255) {
        char prev[4096];
        snprintf(prev, sizeof(prev), "%s%s%s", acc[n-1],
                 strcmp(acc[n-1], "/") == 0 ? "" : "/", tok);
        acc[n] = g_strdup(prev);
        parts[n] = g_strdup(tok);
        tok = strtok(NULL, "/");
        n++;
    }

    for (int i = 0; i < n; i++) {
        GtkWidget *btn = gtk_button_new_with_label(parts[i]);
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_crumb_clicked),
                              g_strdup(acc[i]), crumb_data_free, 0);
        gtk_box_pack_start(GTK_BOX(g.breadcrumb), btn, FALSE, FALSE, 0);
        
        if (i < n - 1) {
            GtkWidget *sep = gtk_label_new("›");
            gtk_box_pack_start(GTK_BOX(g.breadcrumb), sep, FALSE, FALSE, 0);
        }
    }

    for (int i = 0; i < n; i++) g_free(acc[i]);
    for (int i = 1; i < n; i++) g_free(parts[i]);
    g_free(saved);
    gtk_widget_show_all(g.breadcrumb);
}



/* ---- recently-visited directories (persisted) ---- */
static char *recent_file_path(void) {
    const char *cfg = g_get_user_config_dir();
    return g_strdup_printf("%s/linder/recent_dirs.txt", cfg);
}

/* record a visited dir at the front of the recent list (dedup, max 20) */
static void record_recent_dir(const char *path) {
    char *fp = recent_file_path();
    char *dir = g_path_get_dirname(fp);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    FILE *in = fopen(fp, "r");
    if (in) {
        char line[4096];
        while (fgets(line, sizeof(line), in)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] && strcmp(line, path) != 0)
                g_ptr_array_add(arr, g_strdup(line));
        }
        fclose(in);
    }
    g_ptr_array_insert(arr, 0, g_strdup(path));
    if (arr->len > 20)
        g_ptr_array_set_size(arr, 20);

    FILE *out = fopen(fp, "w");
    if (out) {
        for (guint i = 0; i < arr->len; i++)
            fprintf(out, "%s\n", (char*)g_ptr_array_index(arr, i));
        fclose(out);
    }
    g_ptr_array_free(arr, TRUE);
    g_free(fp);
}

/* ---- XDG user dirs: read ~/.config/user-dirs.dirs as source of truth ----
 * Falls back to g_get_user_special_dir() when the config file/key is absent,
 * so the sidebar adapts to each machine's own localized dirs (~/Desktop on
 * English systems, ~/桌面 on Chinese, etc.). Returns newly-allocated or NULL. */
static char *xdg_user_dir(const char *key) {
    const char *cfg = g_get_user_config_dir();
    char *path = g_strdup_printf("%s/user-dirs.dirs", cfg);
    FILE *fp = fopen(path, "r");
    g_free(path);
    if (fp) {
        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            /* lines look like: XDG_DESKTOP_DIR="$HOME/桌面" */
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            if (strcmp(line, key) != 0) { *eq = '='; continue; }
            char *val = eq + 1;
            val[strcspn(val, "\r\n")] = '\0';
            if (val[0] == '"') {
                val++;
                char *endq = strrchr(val, '"');
                if (endq) *endq = '\0';
            }
            fclose(fp);
            if (!val[0]) return NULL;
            if (g_str_has_prefix(val, "$HOME"))
                return g_strconcat(g_get_home_dir(), val + strlen("$HOME"), NULL);
            if (val[0] == '~')
                return g_strconcat(g_get_home_dir(), val + 1, NULL);
            return g_strdup(val);
        }
        fclose(fp);
    }

    /* fallback: GLib's own special-dir mapping (handles most desktops) */
    GUserDirectory gd;
    if (strcmp(key, "XDG_DESKTOP_DIR") == 0)        gd = G_USER_DIRECTORY_DESKTOP;
    else if (strcmp(key, "XDG_DOCUMENTS_DIR") == 0) gd = G_USER_DIRECTORY_DOCUMENTS;
    else if (strcmp(key, "XDG_DOWNLOAD_DIR") == 0)  gd = G_USER_DIRECTORY_DOWNLOAD;
    else if (strcmp(key, "XDG_MUSIC_DIR") == 0)     gd = G_USER_DIRECTORY_MUSIC;
    else if (strcmp(key, "XDG_PICTURES_DIR") == 0)  gd = G_USER_DIRECTORY_PICTURES;
    else if (strcmp(key, "XDG_VIDEOS_DIR") == 0)    gd = G_USER_DIRECTORY_VIDEOS;
    else                                             return NULL;
    const char *r = g_get_user_special_dir(gd);
    return (r && r[0]) ? g_strdup(r) : NULL;
}

static void hist_push(const char *path) {
    if (!g.hist) return;
    
    while (g.hist->len > (guint)(g.hist_pos + 1))
        g_ptr_array_remove_index(g.hist, g.hist->len - 1);
    g_ptr_array_add(g.hist, g_strdup(path));
    g.hist_pos = g.hist->len - 1;
    
    gtk_widget_set_sensitive(g.back_btn, g.hist_pos > 0);
    gtk_widget_set_sensitive(g.fwd_btn, g.hist_pos < (int)g.hist->len - 1);
}

static void navigate_to(const char *path) {
    char real[4096];
    if (!realpath(path, real)) return;

    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    char *dir_copy = strdup(real);
    if (!dir_copy) return;
    free(g.dir);
    g.dir = dir_copy;

    gtk_window_set_title(GTK_WINDOW(g.window), real);
    
    const char *base = strrchr(real, '/');
    if (g.title_label)
        gtk_label_set_text(GTK_LABEL(g.title_label), base && base[1] ? base + 1 : real);
    refresh_breadcrumb();

    reload_dsstore();
    refresh_views();
    record_recent_dir(real);

    /* 「排放 .DS_Store」：开时，浏览文件夹就自动写一份 .DS_Store（像 mac） */
    if (dsstore_poop_enabled()) {
        DsStoreMeta meta;
        meta_init(&meta);
        meta.view = strdup(VIEW_GRID);
        meta.icon_size = g.icon_size > 0 ? g.icon_size : 64;
        dsstore_write(real, &meta, 1);
        meta_free(&meta);
    }
}


static void navigate_push(const char *path) {
    char real[4096];
    if (!realpath(path, real)) return;
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    hist_push(real);
    navigate_to(real);
}


static void open_entry(const char *full) {
    struct stat st;
    if (stat(full, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        navigate_push(full);
        return;
    }

    /* identify type by extension (not content), then use system default app,
     * or show an "open with" chooser when no default exists (Windows-style). */
    const char *base = strrchr(full, '/');
    if (!base) base = full;
    else base++;

    gchar *ctype = g_content_type_guess(base, NULL, 0, NULL);
    if (ctype && g_content_type_is_unknown(ctype)) {
        g_free(ctype);
        ctype = NULL;
    }

    GAppInfo *app = ctype ? g_app_info_get_default_for_type(ctype, FALSE) : NULL;
    gchar *uri = g_filename_to_uri(full, NULL, NULL);

    if (app) {
        GList *l = NULL;
        l = g_list_append(l, uri);
        g_app_info_launch_uris(app, l, NULL, NULL);
        g_list_free(l);
        g_object_unref(app);
    } else if (uri) {
        /* no default handler: show GTK "open with" chooser */
        GtkWidget *dlg = gtk_app_chooser_dialog_new(GTK_WINDOW(g.window),
            GTK_DIALOG_MODAL, g_file_new_for_uri(uri));
        gtk_widget_show_all(dlg);
        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
            GAppInfo *chosen = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dlg));
            if (chosen) {
                GList *l = g_list_append(NULL, uri);
                g_app_info_launch_uris(chosen, l, NULL, NULL);
                g_list_free(l);
                g_object_unref(chosen);
            }
        }
        gtk_widget_destroy(dlg);
    }

    g_free(uri);
    g_free(ctype);
}

static void on_back_clicked(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    if (g.hist && g.hist_pos > 0) {
        g.hist_pos--;
        char *p = g_ptr_array_index(g.hist, g.hist_pos);
        navigate_to(p);
    } else {
        
        char *parent = strdup(g.dir);
        char *slash = strrchr(parent, '/');
        if (slash) {
            if (slash == parent) slash[1] = 0;
            else *slash = 0;
        }
        navigate_to(parent[0] ? parent : "/");
        free(parent);
    }
}

static void on_forward_clicked(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    if (g.hist && g.hist_pos < (int)g.hist->len - 1) {
        g.hist_pos++;
        char *p = g_ptr_array_index(g.hist, g.hist_pos);
        navigate_to(p);
        gtk_widget_set_sensitive(g.back_btn, g.hist_pos > 0);
        gtk_widget_set_sensitive(g.fwd_btn, g.hist_pos < (int)g.hist->len - 1);
    }
}

static void on_iconview_activated(GtkIconView *iv, GtkTreePath *path, gpointer u) {
    (void)iv; (void)u;
    GtkTreeModel *model = GTK_TREE_MODEL(g.icon_store);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(model, &it, path)) return;
    char *name = NULL;
    gtk_tree_model_get(model, &it, 1, &name, -1);
    if (!name) return;

    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", g.dir, name);
    g_free(name);

    open_entry(full);
}


static void on_listview_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                      GtkTreeViewColumn *col, gpointer u) {
    (void)tv; (void)col; (void)u;
    GtkTreeModel *model = GTK_TREE_MODEL(g.list_store);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(model, &it, path)) return;
    char *name = NULL;
    gtk_tree_model_get(model, &it, 1, &name, -1);
    if (!name) return;

    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", g.dir, name);
    g_free(name);

    open_entry(full);
}


static void on_switch_icon(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_stack_set_visible_child_name(GTK_STACK(g.stack), "icon");
}
static void on_switch_list(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_stack_set_visible_child_name(GTK_STACK(g.stack), "list");
}
G_GNUC_UNUSED static void on_switch_miller(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_stack_set_visible_child_name(GTK_STACK(g.stack), "miller");
}


static void on_zoom_changed(GtkRange *range, gpointer u) {
    (void)u;
    int w = (int)gtk_range_get_value(GTK_RANGE(range));
    g.icon_size = w;
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(g.iconview), w);
    refresh_views();
}




static char *selected_full;
static GtkWidget *build_context_menu(void);


typedef struct {
    GtkWidget   *view;   /* GtkTreeView */
    GtkListStore *store; 
    char        *path;   
} MillerCol;

static void miller_add_column(const char *path);  


static void on_miller_row_activated(GtkTreeView *tv, GtkTreePath *tp,
                                    GtkTreeViewColumn *col, gpointer u) {
    (void)col;
    MillerCol *mc = u;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(mc->store), &it, tp)) return;
    char *name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(mc->store), &it, 1, &name, -1);
    if (!name) return;

    char *full = g_strdup_printf("%s/%s", mc->path, name);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        miller_add_column(full);  
    } else {
        open_entry(full);          
    }
    g_free(full);
    g_free(name);
    (void)tv;
}


static gboolean on_miller_button_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)w;
    if (ev->button != 3) return FALSE;
    MillerCol *mc = u;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(mc->view));
    GtkTreePath *tp = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(mc->view),
                                      (int)ev->x, (int)ev->y,
                                      &tp, NULL, NULL, NULL)) {
        GtkTreeIter it;
        char *name = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(mc->store), &it, tp))
            gtk_tree_model_get(GTK_TREE_MODEL(mc->store), &it, 1, &name, -1);
        g_free(selected_full);
        selected_full = name ? g_strdup_printf("%s/%s", mc->path, name) : NULL;
        g_free(name);
        gtk_tree_selection_select_path(sel, tp);
        gtk_tree_path_free(tp);
    } else {
        g_free(selected_full);
        selected_full = NULL;
        gtk_tree_selection_unselect_all(sel);
    }
    GtkWidget *menu = build_context_menu();
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
    return TRUE;
}


static void miller_add_column(const char *path) {
    
    int keep = g.miller_cols ? (int)g.miller_cols->len : 0;
    for (int i = 0; i < keep; i++) {
        MillerCol *mc = g_ptr_array_index(g.miller_cols, i);
        if (strcmp(mc->path, path) == 0) {
            keep = i + 1;  
            break;
        }
    }
    
    if (g.miller_cols) {
        while ((int)g.miller_cols->len > keep) {
            MillerCol *mc = g_ptr_array_index(g.miller_cols, g.miller_cols->len - 1);
            gtk_widget_destroy(mc->view);
            g_object_unref(mc->store);
            g_free(mc->path);
            g_free(mc);
            g_ptr_array_remove_index(g.miller_cols, g.miller_cols->len - 1);
        }
    }

    
    MillerCol *mc = g_new0(MillerCol, 1);
    mc->path = g_strdup(path);
    mc->store = gtk_list_store_new(2, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    mc->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(mc->store));

    GtkCellRenderer *rp = gtk_cell_renderer_pixbuf_new();
    GtkTreeViewColumn *c0 = gtk_tree_view_column_new_with_attributes(
        "", rp, "pixbuf", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(mc->view), c0);
    GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c1 = gtk_tree_view_column_new_with_attributes(
        "", r1, "text", 1, NULL);
    gtk_tree_view_column_set_expand(c1, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(mc->view), c1);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(mc->view), FALSE);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(mc->view));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);

    g_signal_connect(mc->view, "row-activated", G_CALLBACK(on_miller_row_activated), mc);
    g_signal_connect(mc->view, "button-press-event", G_CALLBACK(on_miller_button_press), mc);

    
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        
        char **names = NULL;
        int cnt = 0, cap = 0;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strcmp(de->d_name, ".DS_Store") == 0) continue;
            if (cnt + 1 > cap) { cap = cap ? cap*2 : 64; names = realloc(names, sizeof(char*)*cap); }
            names[cnt++] = strdup(de->d_name);
        }
        closedir(d);
        
        for (int i = 1; i < cnt; i++) {
            char *key = names[i];
            int j = i - 1;
            while (j >= 0) {
                char *pj = names[j];
                char fj[4100], fi[4100];
                struct stat sj, si;
                snprintf(fj, sizeof(fj), "%s/%s", path, pj);
                snprintf(fi, sizeof(fi), "%s/%s", path, key);
                int dj = (stat(fj, &sj)==0 && S_ISDIR(sj.st_mode)) ? 0 : 1;
                int di = (stat(fi, &si)==0 && S_ISDIR(si.st_mode)) ? 0 : 1;
                if (dj > di || (dj == di && strcasecmp(pj, key) > 0)) {
                    names[j+1] = names[j];
                    j--;
                } else break;
            }
            names[j+1] = key;
        }
        for (int i = 0; i < cnt; i++) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", path, names[i]);
            struct stat st;
            if (stat(full, &st) != 0) { free(names[i]); continue; }
            int isdir = S_ISDIR(st.st_mode);
            const char *icon = isdir ? "folder" : "text-x-generic";
            GdkPixbuf *pix = isdir
                ? load_folder_pixbuf(16)
                : gtk_icon_theme_load_icon(theme, icon, 16,
                                           GTK_ICON_LOOKUP_GENERIC_FALLBACK, NULL);
            if (!pix) pix = gtk_icon_theme_load_icon(theme, "text-x-generic", 16, 0, NULL);
            GtkTreeIter it;
            gtk_list_store_append(mc->store, &it);
            gtk_list_store_set(mc->store, &it, 0, pix, 1, names[i], -1);
            if (pix) g_object_unref(pix);
            free(names[i]);
        }
        free(names);
    }

    
    GtkWidget *col_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(col_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(col_scroll, 240, -1);
    gtk_container_add(GTK_CONTAINER(col_scroll), mc->view);
    gtk_box_pack_start(GTK_BOX(g.miller_box), col_scroll, FALSE, FALSE, 0);
    gtk_widget_show_all(col_scroll);

    if (!g.miller_cols) g.miller_cols = g_ptr_array_new();
    g_ptr_array_add(g.miller_cols, mc);
}


static void miller_rebuild(void) {
    if (!g.miller_box) return;
    if (g.miller_cols) {
        while (g.miller_cols->len > 0) {
            MillerCol *mc = g_ptr_array_index(g.miller_cols, g.miller_cols->len - 1);
            gtk_widget_destroy(mc->view);
            g_object_unref(mc->store);
            g_free(mc->path);
            g_free(mc);
            g_ptr_array_remove_index(g.miller_cols, g.miller_cols->len - 1);
        }
    }
    miller_add_column(g.dir);
}






static char *make_full(const char *name) {
    if (!name) return NULL;
    return g_strdup_printf("%s/%s", g.dir, name);
}

static GPtrArray *get_selected_names(void);


static void do_delete(const char *full) {
    if (!full) return;
    GPtrArray *names = get_selected_names();
    for (guint i = 0; i < names->len; i++) {
        char *f = make_full(g_ptr_array_index(names, i));
        if (!f) continue;
        GFile *gf = g_file_new_for_path(f);
        GError *err = NULL;
        if (!g_file_trash(gf, NULL, &err)) {
            g_clear_error(&err);
            g_file_delete(gf, NULL, NULL);
        }
        g_object_unref(gf);
        g_free(f);
    }
    g_ptr_array_free(names, TRUE);
    refresh_views();
}


static void clip_list_clear(void) {
    if (g.clip_list) { g_ptr_array_free(g.clip_list, TRUE); g.clip_list = NULL; }
}

static void do_copy(const char *full, int cut) {
    (void)full;
    clip_list_clear();
    g.clip_list = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *names = get_selected_names();
    for (guint i = 0; i < names->len; i++) {
        char *f = make_full(g_ptr_array_index(names, i));
        if (f) g_ptr_array_add(g.clip_list, f);
    }
    g_ptr_array_free(names, TRUE);
    g.clip_cut = cut;
}


static void do_paste(void) {
    if (!g.clip_list || g.clip_list->len == 0) return;
    GError *err = NULL;
    for (guint i = 0; i < g.clip_list->len; i++) {
        const char *src = g_ptr_array_index(g.clip_list, i);
        char *name = g_path_get_basename(src);
        char *dst = g_strdup_printf("%s/%s", g.dir, name);
        GFile *sf = g_file_new_for_path(src);
        GFile *df = g_file_new_for_path(dst);
        if (g.clip_cut)
            g_file_move(sf, df, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err);
        else
            g_file_copy(sf, df, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err);
        g_object_unref(sf);
        g_object_unref(df);
        g_free(name);
        g_free(dst);
        if (err) break;
    }
    if (err) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g.window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            tr("操作失败%s", "Operation failed%s"), err->message);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_error_free(err);
    }
    if (g.clip_cut) clip_list_clear();
    refresh_views();
}


static void do_rename(const char *full) {
    if (!full) return;
    char *name = g_path_get_basename(full);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(tr("重命名", "Rename"), GTK_WINDOW(g.window),
        GTK_DIALOG_MODAL, tr("取消", "Cancel"), GTK_RESPONSE_CANCEL, tr("确定", "OK"), GTK_RESPONSE_OK, NULL);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), name);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_widget_set_size_request(entry, 300, -1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       entry, TRUE, TRUE, 8);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *newname = gtk_entry_get_text(GTK_ENTRY(entry));
        if (newname && newname[0] && strcmp(newname, name) != 0) {
            GFile *f = g_file_new_for_path(full);
            GError *err = NULL;
            g_file_set_display_name(f, newname, NULL, &err);
            if (err) {
                GtkWidget *ed = gtk_message_dialog_new(GTK_WINDOW(g.window),
                    GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                    tr("重命名失败：%s", "Rename failed: %s"), err->message);
                gtk_dialog_run(GTK_DIALOG(ed));
                gtk_widget_destroy(ed);
                g_error_free(err);
            }
            g_object_unref(f);
            refresh_views();
        }
    }
    gtk_widget_destroy(dlg);
    g_free(name);
}


/* 在终端中打开：右键文件夹 → 在该文件夹路径开终端；右键空白 → 在当前目录开终端。
 * 用 g_spawn_async 后台拉起 gnome-terminal，不阻塞 Linder。 */
static void do_open_terminal(void) {
    const char *target = g.dir;
    if (selected_full) {
        struct stat st;
        if (stat(selected_full, &st) == 0 && S_ISDIR(st.st_mode))
            target = selected_full;
    }

    gchar *wd_opt = g_strdup_printf("--working-directory=%s", target);
    gchar *argv[] = { (gchar*)"gnome-terminal", wd_opt, NULL };
    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, NULL, &err);
    if (!ok) {
        if (err) {
            g_warning("open terminal failed: %s", err->message);
            g_error_free(err);
        }
    }
    g_free(wd_opt);
}

static void do_new_folder(void) {
    char *dst = g_strdup_printf("%s/%s", g.dir, tr("新建文件夹", "New Folder"));
    GFile *f = g_file_new_for_path(dst);
    GError *err = NULL;
    if (!g_file_make_directory(f, NULL, &err)) {
        if (err) { g_error_free(err); err = NULL; }
        
        for (int i = 2; i < 100; i++) {
            g_free(dst);
            dst = g_strdup_printf("%s/%s %d", g.dir, tr("新建文件夹", "New Folder"), i);
            g_object_unref(f);
            f = g_file_new_for_path(dst);
            if (g_file_make_directory(f, NULL, &err)) break;
            if (err) { g_error_free(err); err = NULL; }
        }
    }
    g_object_unref(f);
    g_free(dst);
    refresh_views();
}

/* create a new empty file/folder with a given display name and extension.
 * action string format: "new:<ext>" e.g. "new:.txt", "new:.docx", or "new:folder".
 * auto-increments a numeric suffix on name collision (Windows-style). */
static void do_new_file(const char *action) {
    const char *ext = action + 4; /* skip "new:" */

    if (strcmp(ext, "folder") == 0) {
        do_new_folder();
        return;
    }

    int isdir = (strcmp(ext, "") == 0);
    (void)isdir;

    char *dst = g_strdup_printf("%s/%s%s", g.dir, tr("未命名", "Untitled"), ext);
    /* try to create; on EEXIST increment suffix */
    GError *err = NULL;
    GFile *f = g_file_new_for_path(dst);
    GFileOutputStream *out = g_file_create(f, G_FILE_CREATE_NONE, NULL, &err);
    if (!out && err) {
        g_clear_error(&err);
        for (int i = 2; i < 1000; i++) {
            g_free(dst);
            dst = g_strdup_printf("%s/%s %d%s", g.dir, tr("未命名", "Untitled"), i, ext);
            g_object_unref(f);
            f = g_file_new_for_path(dst);
            out = g_file_create(f, G_FILE_CREATE_NONE, NULL, &err);
            if (out) break;
            if (err) g_clear_error(&err);
        }
    }
    if (out) {
        g_output_stream_close(G_OUTPUT_STREAM(out), NULL, NULL);
        g_object_unref(out);
    }
    if (err) g_clear_error(&err);
    g_object_unref(f);
    g_free(dst);
    refresh_views();
}



static void on_menu_action(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    const char *action = user_data;
    if (strcmp(action, "open") == 0) {
        open_entry(selected_full);
    } else if (strcmp(action, "copy") == 0) {
        do_copy(selected_full, 0);
    } else if (strcmp(action, "cut") == 0) {
        do_copy(selected_full, 1);
    } else if (strcmp(action, "paste") == 0) {
        do_paste();
    } else if (strcmp(action, "rename") == 0) {
        do_rename(selected_full);
    } else if (strcmp(action, "delete") == 0) {
        do_delete(selected_full);
    } else if (strcmp(action, "newfolder") == 0) {
        do_new_folder();
    } else if (strncmp(action, "new:", 4) == 0) {
        do_new_file(action);
    } else if (strcmp(action, "refresh") == 0) {
        refresh_views();
    } else if (strcmp(action, "bg:image") == 0) {
        do_set_bg_image();
    } else if (strcmp(action, "bg:color") == 0) {
        do_set_bg_color();
    } else if (strcmp(action, "bg:clear") == 0) {
        do_clear_bg();
    } else if (strcmp(action, "terminal") == 0) {
        do_open_terminal();
    }
}


static GtkWidget *build_context_menu(void) {
    GtkWidget *menu = gtk_menu_new();
    gboolean has_sel = (selected_full != NULL);

    struct { const char *label; const char *action; gboolean sens; } items[] = {
        {tr("打开", "Open"),                 "open",      has_sel},
        {tr("剪切", "Cut"),                  "cut",       has_sel},
        {tr("复制", "Copy"),                 "copy",      has_sel},
        {tr("粘贴", "Paste"),                "paste",     g.clip_list != NULL},
        {tr("重命名", "Rename"),             "rename",    has_sel},
        {tr("删除", "Delete"),               "delete",    has_sel},
        {NULL, NULL, FALSE},  
        {tr("刷新", "Refresh"),              "refresh",   TRUE},
        {tr("在终端中打开", "Open in Terminal"), "terminal",  TRUE},
    };
    int n = sizeof(items)/sizeof(items[0]);
    for (int i = 0; i < n; i++) {
        if (items[i].label == NULL) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                                  gtk_separator_menu_item_new());
            continue;
        }
        GtkWidget *mi = gtk_menu_item_new_with_label(items[i].label);
        gtk_widget_set_sensitive(mi, items[i].sens);
        g_signal_connect(mi, "activate", G_CALLBACK(on_menu_action),
                         (gpointer)items[i].action);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    /* Windows-style "New" submenu (mac Finder lacks this; we add it as a plus) */
    struct { const char *label; const char *action; } news[] = {
        {tr("文件夹", "Folder"),                 "new:folder"},
        {tr("文本文档", "Text Document"),        "new:.txt"},
        {tr("Word 文档", "Word Document"),       "new:.docx"},
        {tr("Excel 表格", "Excel Sheet"),        "new:.xlsx"},
        {tr("PowerPoint 演示文稿", "PowerPoint"),"new:.pptx"},
        {tr("Markdown 文档", "Markdown Document"),"new:.md"},
        {tr("富文本", "Rich Text"),              "new:.rtf"},
    };

    GtkWidget *new_item = gtk_menu_item_new_with_label(tr("新建", "New"));
    GtkWidget *new_menu = gtk_menu_new();
    int nn = sizeof(news)/sizeof(news[0]);
    for (int i = 0; i < nn; i++) {
        GtkWidget *ni = gtk_menu_item_new_with_label(news[i].label);
        g_signal_connect(ni, "activate", G_CALLBACK(on_menu_action),
                         (gpointer)news[i].action);
        gtk_menu_shell_append(GTK_MENU_SHELL(new_menu), ni);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(new_item), new_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_item);

    /* macOS Finder-style "set background" submenu */
    struct { const char *label; const char *action; } bgs[] = {
        {tr("设置背景图片…", "Set Background Image…"), "bg:image"},
        {tr("设置背景颜色…", "Set Background Color…"), "bg:color"},
        {tr("清除背景", "Clear Background"),           "bg:clear"},
    };
    GtkWidget *bg_item = gtk_menu_item_new_with_label(tr("设置背景", "Set Background"));
    GtkWidget *bg_menu = gtk_menu_new();
    int nb = sizeof(bgs)/sizeof(bgs[0]);
    for (int i = 0; i < nb; i++) {
        GtkWidget *bi = gtk_menu_item_new_with_label(bgs[i].label);
        g_signal_connect(bi, "activate", G_CALLBACK(on_menu_action),
                         (gpointer)bgs[i].action);
        gtk_menu_shell_append(GTK_MENU_SHELL(bg_menu), bi);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(bg_item), bg_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), bg_item);

    gtk_widget_show_all(menu);
    return menu;
}


static char *current_selected_name(void) {
    
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g.listview));
    GtkTreeModel *model = NULL;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        gchar *n = NULL;
        gtk_tree_model_get(model, &it, 1, &n, -1);
        return n; 
    }
    
    GList *paths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(g.iconview));
    if (paths) {
        GtkTreePath *p = paths->data;
        gchar *n = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g.icon_store), &it, p))
            gtk_tree_model_get(GTK_TREE_MODEL(g.icon_store), &it, 1, &n, -1);
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        return n;
    }
    return NULL;
}

/* collect all currently selected file names (list view + icon view) */
static GPtrArray *get_selected_names(void) {
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

    /* list view: all selected rows */
    GtkTreeSelection *lsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g.listview));
    GtkTreeModel *lmodel = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(lsel, &lmodel);
    for (GList *l = rows; l; l = l->next) {
        GtkTreeIter it;
        gchar *n = NULL;
        if (gtk_tree_model_get_iter(lmodel, &it, l->data))
            gtk_tree_model_get(lmodel, &it, 1, &n, -1);
        if (n) g_ptr_array_add(names, n);
    }
    if (rows) g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

    /* icon view: all selected items */
    GList *ipaths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(g.iconview));
    for (GList *l = ipaths; l; l = l->next) {
        GtkTreeIter it;
        gchar *n = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g.icon_store), &it, l->data))
            gtk_tree_model_get(GTK_TREE_MODEL(g.icon_store), &it, 1, &n, -1);
        if (n) g_ptr_array_add(names, n);
    }
    if (ipaths) g_list_free_full(ipaths, (GDestroyNotify)gtk_tree_path_free);

    return names;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer u) {
    (void)w; (void)u;
    guint key = ev->keyval;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;

    
    if (ctrl && key == GDK_KEY_a) {
        gtk_icon_view_select_all(GTK_ICON_VIEW(g.iconview));
        gtk_tree_selection_select_all(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(g.listview)));
        return TRUE;
    }

    
    if (ctrl && key == GDK_KEY_c) {
        char *name = current_selected_name();
        if (name) { char *f = make_full(name); do_copy(f, 0); g_free(f); g_free(name); }
        return TRUE;
    }
    if (ctrl && key == GDK_KEY_x) {
        char *name = current_selected_name();
        if (name) { char *f = make_full(name); do_copy(f, 1); g_free(f); g_free(name); }
        return TRUE;
    }
    if (ctrl && key == GDK_KEY_v) {
        do_paste();
        return TRUE;
    }

    
    if (key == GDK_KEY_Delete) {
        char *name = current_selected_name();
        if (name) { char *f = make_full(name); do_delete(f); g_free(f); g_free(name); }
        return TRUE;
    }

    
    if (key == GDK_KEY_F2) {
        char *name = current_selected_name();
        if (name) { char *f = make_full(name); do_rename(f); g_free(f); g_free(name); }
        return TRUE;
    }

    
    if (key == GDK_KEY_BackSpace) {
        char *parent = strdup(g.dir);
        char *slash = strrchr(parent, '/');
        if (slash) {
            if (slash == parent) slash[1] = 0;
            else *slash = 0;
        }
        navigate_push(parent[0] ? parent : "/");
        free(parent);
        return TRUE;
    }

    
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
        char *name = current_selected_name();
        if (name) {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", g.dir, name);
            open_entry(full);
            g_free(name);
        }
        return TRUE;
    }

    return FALSE;
}


static GtkTargetEntry dnd_targets[] = {
    { (char*)"text/uri-list", 0, 0 }
};

/* ==================== DND Manual Multi-Drag State & Preview ==================== */

typedef struct {
    gboolean is_down;
    gint     press_x;
    gint     press_y;
    gboolean on_selection;
    GtkTreePath *clicked_path;
} ViewDragState;

static ViewDragState g_drag_state = {0};

static void cairo_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static GdkPixbuf *get_first_selected_pixbuf(GtkWidget *w) {
    GdkPixbuf *pb = NULL;
    if (GTK_IS_ICON_VIEW(w)) {
        GList *ipaths = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(w));
        if (ipaths) {
            GtkTreeIter it;
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g.icon_store), &it, (GtkTreePath*)ipaths->data))
                gtk_tree_model_get(GTK_TREE_MODEL(g.icon_store), &it, 0, &pb, -1);
            g_list_free_full(ipaths, (GDestroyNotify)gtk_tree_path_free);
        }
    } else {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
        GtkTreeModel *model = NULL;
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
        if (rows) {
            GtkTreeIter it;
            if (gtk_tree_model_get_iter(model, &it, (GtkTreePath*)rows->data))
                gtk_tree_model_get(model, &it, 0, &pb, -1);
            g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
        }
    }
    return pb;
}

static void on_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer u) {
    (void)u;
    GPtrArray *names = get_selected_names();
    guint count = names ? names->len : 1;
    if (names) g_ptr_array_free(names, TRUE);

    GdkPixbuf *base_pb = get_first_selected_pixbuf(w);
    int pb_w = base_pb ? gdk_pixbuf_get_width(base_pb) : 48;
    int pb_h = base_pb ? gdk_pixbuf_get_height(base_pb) : 48;

    int total_w = pb_w + 24;
    int total_h = pb_h + 20;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, total_w, total_h);
    cairo_t *cr = cairo_create(surf);

    if (count >= 3 && base_pb) {
        cairo_save(cr);
        gdk_cairo_set_source_pixbuf(cr, base_pb, 10, 2);
        cairo_paint_with_alpha(cr, 0.45);
        cairo_restore(cr);
    }
    if (count >= 2 && base_pb) {
        cairo_save(cr);
        gdk_cairo_set_source_pixbuf(cr, base_pb, 5, 5);
        cairo_paint_with_alpha(cr, 0.70);
        cairo_restore(cr);
    }

    if (base_pb) {
        cairo_save(cr);
        gdk_cairo_set_source_pixbuf(cr, base_pb, 0, 8);
        cairo_paint(cr);
        cairo_restore(cr);
        g_object_unref(base_pb);
    }

    if (count > 1) {
        char num_str[16];
        g_snprintf(num_str, sizeof(num_str), "%u", count);

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, num_str, &ext);

        double badge_h = 18.0;
        double badge_w = ext.width + 10.0;
        if (badge_w < badge_h) badge_w = badge_h;

        double badge_x = pb_w + 2 - (badge_w / 2.0);
        double badge_y = 2.0;

        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.25);
        cairo_rounded_rect(cr, badge_x, badge_y + 1, badge_w, badge_h, badge_h / 2.0);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 1.0, 0.231, 0.188);
        cairo_rounded_rect(cr, badge_x, badge_y, badge_w, badge_h, badge_h / 2.0);
        cairo_fill_preserve(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        double text_x = badge_x + (badge_w - ext.width) / 2.0 - ext.x_bearing;
        double text_y = badge_y + (badge_h - ext.height) / 2.0 - ext.y_bearing;
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, num_str);
    }

    cairo_destroy(cr);
    cairo_surface_set_device_offset(surf, -pb_w / 2, -pb_h / 2);
    gtk_drag_set_icon_surface(ctx, surf);
    cairo_surface_destroy(surf);
}

static gboolean on_view_dnd_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)u;
    if (ev->button != GDK_BUTTON_PRIMARY) return FALSE;

    if (g_drag_state.clicked_path) {
        gtk_tree_path_free(g_drag_state.clicked_path);
        g_drag_state.clicked_path = NULL;
    }

    g_drag_state.is_down = TRUE;
    g_drag_state.press_x = (gint)ev->x;
    g_drag_state.press_y = (gint)ev->y;
    g_drag_state.on_selection = FALSE;

    GtkTreePath *path = NULL;
    if (GTK_IS_ICON_VIEW(w)) {
        path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), (gint)ev->x, (gint)ev->y);
        if (path) {
            if (gtk_icon_view_path_is_selected(GTK_ICON_VIEW(w), path))
                g_drag_state.on_selection = TRUE;
            else
                g_drag_state.clicked_path = gtk_tree_path_copy(path);
            gtk_tree_path_free(path);
        }
    } else {
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (gint)ev->x, (gint)ev->y,
                                          &path, NULL, NULL, NULL)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
            if (gtk_tree_selection_path_is_selected(sel, path))
                g_drag_state.on_selection = TRUE;
            else
                g_drag_state.clicked_path = gtk_tree_path_copy(path);
            gtk_tree_path_free(path);
        }
    }

    return FALSE; /* crucial: do NOT stop propagation */
}

static gboolean on_view_dnd_release(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)w; (void)u;
    if (ev->button == GDK_BUTTON_PRIMARY) {
        g_drag_state.is_down = FALSE;
        if (g_drag_state.clicked_path) {
            gtk_tree_path_free(g_drag_state.clicked_path);
            g_drag_state.clicked_path = NULL;
        }
    }
    return FALSE;
}

static gboolean on_view_dnd_motion(GtkWidget *w, GdkEventMotion *ev, gpointer u) {
    (void)u;
    if (!g_drag_state.is_down) return FALSE;
    if (!(ev->state & GDK_BUTTON1_MASK)) {
        g_drag_state.is_down = FALSE;
        return FALSE;
    }

    if (gtk_drag_check_threshold(w, g_drag_state.press_x, g_drag_state.press_y,
                                 (gint)ev->x, (gint)ev->y)) {
        /* Pressed on empty area (no item under pointer, nothing already
         * selected): do NOT take over. Let GTK's built-in rubber-band
         * selection handle drag-box multi-select. */
        if (!g_drag_state.on_selection && !g_drag_state.clicked_path) {
            g_drag_state.is_down = FALSE;
            return FALSE;
        }
        g_drag_state.is_down = FALSE;

        if (!g_drag_state.on_selection && g_drag_state.clicked_path) {
            if (GTK_IS_ICON_VIEW(w)) {
                gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
                gtk_icon_view_select_path(GTK_ICON_VIEW(w), g_drag_state.clicked_path);
            } else {
                GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
                gtk_tree_selection_unselect_all(sel);
                gtk_tree_selection_select_path(sel, g_drag_state.clicked_path);
            }
        }
        if (g_drag_state.clicked_path) {
            gtk_tree_path_free(g_drag_state.clicked_path);
            g_drag_state.clicked_path = NULL;
        }

        GtkTargetList *tl = gtk_target_list_new(dnd_targets, G_N_ELEMENTS(dnd_targets));
        gtk_drag_begin_with_coordinates(w, tl, GDK_ACTION_COPY | GDK_ACTION_MOVE,
                                        1, (GdkEvent*)ev, (gint)ev->x, (gint)ev->y);
        gtk_target_list_unref(tl);
        return TRUE;
    }
    return FALSE;
}



static void on_drag_data_get(GtkWidget *w, GdkDragContext *ctx,
                             GtkSelectionData *sel, guint info, guint time,
                             gpointer u) {
    (void)w; (void)ctx; (void)info; (void)time; (void)u;
    GPtrArray *names = get_selected_names();
    g_print("[dnd] drag-data-get selected count = %u\n", names ? names->len : 0);
    if (names->len == 0) {
        g_ptr_array_free(names, TRUE);
        return;
    }

    GString *urilist = g_string_new(NULL);
    for (guint i = 0; i < names->len; i++) {
        char *full = make_full(g_ptr_array_index(names, i));
        if (!full) continue;
        char *uri = g_filename_to_uri(full, NULL, NULL);
        if (uri) {
            g_string_append(urilist, uri);
            g_string_append(urilist, "\r\n");
            g_free(uri);
        }
        g_free(full);
    }
    g_ptr_array_free(names, TRUE);

    if (urilist->len > 0)
        gtk_selection_data_set(sel, gtk_selection_data_get_target(sel),
                               8, (guchar*)urilist->str, urilist->len);
    g_string_free(urilist, TRUE);
}


static void on_drag_data_received(GtkWidget *w, GdkDragContext *ctx,
                                  gint x, gint y, GtkSelectionData *sel,
                                  guint info, guint time, gpointer u) {
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)time; (void)u;
    const guchar *data = gtk_selection_data_get_data(sel);
    gint len = gtk_selection_data_get_length(sel);
    if (!data || len <= 0) return;

    char *txt = g_strndup((const char*)data, len);
    char **uris = g_uri_list_extract_uris(txt);
    g_free(txt);
    if (!uris) return;

    for (int i = 0; uris[i]; i++) {
        char *srcpath = g_filename_from_uri(uris[i], NULL, NULL);
        if (!srcpath) continue;
        char *base = g_path_get_basename(srcpath);
        char *dst = g_strdup_printf("%s/%s", g.dir, base);

        GFile *sf = g_file_new_for_path(srcpath);
        GFile *df = g_file_new_for_path(dst);
        GError *err = NULL;
        if (!g_file_move(sf, df, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &err)) {
            if (err) {
                g_printerr("Linder: move failed %s: %s\n", srcpath, err->message);
                g_error_free(err);
            }
        }
        g_object_unref(sf);
        g_object_unref(df);
        g_free(base);
        g_free(dst);
        g_free(srcpath);
    }
    g_strfreev(uris);
    refresh_views();
}


static gboolean on_rubber_draw(GtkWidget *w, cairo_t *cr, gpointer u) {
    (void)w; (void)u;
    if (!g.rubber_active) return FALSE;
    gdouble x0 = g.rubber_x0, y0 = g.rubber_y0, x1 = g.rubber_x1, y1 = g.rubber_y1;
    gdouble x = MIN(x0, x1), y = MIN(y0, y1);
    gdouble wd = fabs(x1 - x0), ht = fabs(y1 - y0);
    if (wd < 2 && ht < 2) return FALSE;
    cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.15);
    cairo_rectangle(cr, x, y, wd, ht);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 0.6);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x, y, wd, ht);
    cairo_stroke(cr);
    return FALSE;
}

static gboolean on_rubber_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)w; (void)u;
    if (ev->button != 1) return FALSE;
    /* only start rubber-band when pressing empty area (no item under pointer) */
    GtkTreePath *p = NULL;
    gtk_icon_view_get_item_at_pos(GTK_ICON_VIEW(g.iconview),
                                  (gint)ev->x, (gint)ev->y, &p, NULL);
    gboolean empty = (p == NULL);
    if (p) gtk_tree_path_free(p);
    if (!empty) return FALSE;

    g.rubber_active = TRUE;
    g.rubber_x0 = g.rubber_x1 = ev->x;
    g.rubber_y0 = g.rubber_y1 = ev->y;
    gtk_widget_queue_draw(g.iconview);
    return TRUE;
}

static gboolean on_rubber_motion(GtkWidget *w, GdkEventMotion *ev, gpointer u) {
    (void)w; (void)u;
    if (!g.rubber_active) return FALSE;
    g.rubber_x1 = ev->x;
    g.rubber_y1 = ev->y;
    gtk_widget_queue_draw(g.iconview);
    return TRUE;
}

static gboolean on_rubber_release(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)w; (void)u;
    if (!g.rubber_active) return FALSE;
    g.rubber_active = FALSE;
    gtk_widget_queue_draw(g.iconview);

    gdouble x = MIN(g.rubber_x0, g.rubber_x1), y = MIN(g.rubber_y0, g.rubber_y1);
    gdouble wd = fabs(g.rubber_x1 - g.rubber_x0), ht = fabs(g.rubber_y1 - g.rubber_y0);
    if (wd >= 4 && ht >= 4 && ev->button == 1) {
        GtkTreeModel *m = GTK_TREE_MODEL(g.icon_store);
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(g.iconview));
        GtkTreeIter it;
        gboolean ok = gtk_tree_model_get_iter_first(m, &it);
        while (ok) {
            GtkTreePath *p = gtk_tree_model_get_path(m, &it);
            GdkRectangle r;
            if (gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(g.iconview), p, NULL, &r)) {
                gboolean hit = !(r.x + r.width < x || r.x > x + wd ||
                                 r.y + r.height < y || r.y > y + ht);
                if (hit) gtk_icon_view_select_path(GTK_ICON_VIEW(g.iconview), p);
            }
            gtk_tree_path_free(p);
            ok = gtk_tree_model_iter_next(m, &it);
        }
    }
    return TRUE;
}

static gboolean on_iconview_button_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)u;
    if (ev->button != 3) return FALSE;

    
    GtkTreePath *path = gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w),
                                                       (int)ev->x, (int)ev->y);
    if (path) {
        gtk_icon_view_select_path(GTK_ICON_VIEW(w), path);
        GtkTreeIter it;
        char *name = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g.icon_store), &it, path))
            gtk_tree_model_get(GTK_TREE_MODEL(g.icon_store), &it, 1, &name, -1);
        g_free(selected_full);
        selected_full = make_full(name);
        g_free(name);
        gtk_tree_path_free(path);
    } else {
        
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(w));
        g_free(selected_full);
        selected_full = NULL;
    }

    GtkWidget *menu = build_context_menu();
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
    return TRUE;
}


static gboolean on_listview_button_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)w; (void)u;
    if (ev->button != 3) return FALSE;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y,
                                      &path, NULL, NULL, NULL)) {
        GtkTreeIter it;
        char *name = NULL;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g.list_store), &it, path))
            gtk_tree_model_get(GTK_TREE_MODEL(g.list_store), &it, 1, &name, -1);
        g_free(selected_full);
        selected_full = make_full(name);
        g_free(name);
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
        gtk_tree_path_free(path);
        
    } else {
        g_free(selected_full);
        selected_full = NULL;
        gtk_tree_selection_unselect_all(sel);
    }

    GtkWidget *menu = build_context_menu();
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
    return TRUE;
}



/* Load one of our own sidebar icons (macOS-style pink outline, SVG vector
 * rendered at requested size for crisp output on any DPI). */
static GdkPixbuf *load_sidebar_icon(const char *name, int size) {
    GdkPixbuf *pix = NULL;
    GError *err = NULL;

    /* 按优先级尝试多个候选目录，避免硬编码单一安装前缀：
     * 1. LINDER_ICON_DIR / LINDER_PREFIX 环境变量
     * 2. XDG_DATA_HOME/linder/icons
     * 3. /usr/local/share/linder/icons、/usr/share/linder/icons
     * 4. 开发树 ./sidebar_svg/
     */
    const char *candidates[6];
    int nc = 0;
    char envbuf[1024];

    const char *icd = g_getenv("LINDER_ICON_DIR");
    if (!icd || !icd[0]) icd = g_getenv("LINDER_PREFIX");
    if (icd && icd[0]) {
        snprintf(envbuf, sizeof(envbuf), "%s/share/linder/icons", icd);
        candidates[nc++] = envbuf;
    }

    const char *xdg = g_getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(envbuf + 512, 512, "%s/linder/icons", xdg);
        candidates[nc++] = envbuf + 512;
    } else {
        const char *home = g_getenv("HOME");
        if (home && home[0]) {
            snprintf(envbuf + 512, 512, "%s/.local/share/linder/icons", home);
            candidates[nc++] = envbuf + 512;
        }
    }

    candidates[nc++] = "/usr/local/share/linder/icons";
    candidates[nc++] = "/usr/share/linder/icons";

    for (int i = 0; i < nc; i++) {
        char *path = g_strdup_printf("%s/%s.svg", candidates[i], name);
        pix = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, &err);
        g_free(path);
        if (pix) {
            if (err) g_error_free(err);
            return pix;
        }
        if (err) { g_error_free(err); err = NULL; }
    }

    /* fall back to dev tree */
    char *devpath = g_strdup_printf("%s/sidebar_svg/%s.svg",
                                    g_get_current_dir(), name);
    pix = gdk_pixbuf_new_from_file_at_scale(devpath, size, size, TRUE, &err);
    g_free(devpath);
    if (err) { g_error_free(err); g_clear_object(&pix); }
    return pix;
}

static void sidebar_add(GtkListBox *box, const char *label, const char *icon,
                        const char *target, gboolean custom) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    /* Sidebar icon = our own macOS-style SVG (pink outline), vector-crisp. */
    GdkPixbuf *ip = load_sidebar_icon(icon, 32);
    GtkWidget *img;
    if (ip) {
        img = gtk_image_new_from_pixbuf(ip);
        g_object_unref(ip);
    } else {
        GdkPixbuf *fb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(),
                "folder", 16, GTK_ICON_LOOKUP_GENERIC_FALLBACK, NULL);
        if (fb) {
            fb = tint_pink(fb);
            img = gtk_image_new_from_pixbuf(fb);
            g_object_unref(fb);
        } else {
            img = gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_MENU);
        }
    }

    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
    
    GtkStyleContext *ctx = gtk_widget_get_style_context(row);
    gtk_style_context_add_class(ctx, "sidebar-row");
    gtk_widget_set_margin_start(row, 16);
    gtk_widget_set_margin_end(row, 6);
    gtk_widget_set_margin_top(row, 1);
    gtk_widget_set_margin_bottom(row, 1);
    gtk_widget_show_all(row);
    g_object_set_data_full(G_OBJECT(row), "target", g_strdup(target), g_free);
    if (custom)
        g_object_set_data(G_OBJECT(row), "custom", (gpointer)"1");
    gtk_list_box_insert(box, row, -1);
}

/* ---- user-added sidebar favorites (persisted to ~/.config/linder/favorites.txt) ---- */

static char *fav_file_path(void) {
    const char *cfg = g_get_user_config_dir();
    return g_strdup_printf("%s/linder/favorites.txt", cfg);
}

/* append a custom favorite row (used by both load and add) */
static void sidebar_add_custom_from_path(GtkListBox *box, const char *path) {
    char *name = g_path_get_basename(path);
    if (!name || !name[0]) { g_free(name); name = g_strdup(path); }
    sidebar_add(box, name, "folder", path, TRUE);
    g_free(name);
}

/* load persisted favorites into the sidebar */
static void load_favorites(GtkListBox *box) {
    char *path = fav_file_path();
    FILE *fp = fopen(path, "r");
    g_free(path);
    if (!fp) return;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        sidebar_add_custom_from_path(box, line);
    }
    fclose(fp);
}

/* write current favorite paths to the config file */
static void save_favorites(GtkListBox *box) {
    char *path = fav_file_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    FILE *fp = fopen(path, "w");
    g_free(path);
    if (!fp) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        if (!GTK_IS_BOX(w)) continue;
        const char *custom = g_object_get_data(G_OBJECT(w), "custom");
        if (!custom) continue;
        const char *target = g_object_get_data(G_OBJECT(w), "target");
        if (target && target[0])
            fprintf(fp, "%s\n", target);
    }
    g_list_free(children);
    fclose(fp);
}

static void sidebar_remove_row(GtkListBox *box, GtkWidget *row) {
    gtk_widget_destroy(row);
    save_favorites(box);
}

/* called when user picks a folder in the chooser */
static void on_fav_chooser_response(GtkDialog *dlg, gint resp, gpointer data) {
    GtkListBox *box = GTK_LIST_BOX(data);
    if (resp == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *fc = GTK_FILE_CHOOSER(dlg);
        char *path = gtk_file_chooser_get_filename(fc);
        if (path) {
            sidebar_add_custom_from_path(box, path);
            save_favorites(box);
            g_free(path);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

/* context-menu item: open a folder chooser to add a favorite */
static void on_sidebar_add_place(GtkMenuItem *mi, gpointer data) {
    (void)mi;
    GtkListBox *box = GTK_LIST_BOX(data);
    GtkWidget *win = gtk_window_get_transient_for(GTK_WINDOW(g.window)) ?
                     gtk_window_get_transient_for(GTK_WINDOW(g.window)) : g.window;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        tr("添加到侧边栏", "Add to Sidebar"), GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        tr("取消", "Cancel"), GTK_RESPONSE_CANCEL,
        tr("添加", "Add"), GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect(dlg, "response", G_CALLBACK(on_fav_chooser_response), box);
    gtk_widget_show_all(dlg);
}

/* context-menu item: remove the right-clicked row from the sidebar */
static void on_sidebar_remove_place(GtkMenuItem *mi, gpointer data) {
    (void)mi;
    GtkWidget *row = GTK_WIDGET(data);
    GtkListBox *box = GTK_LIST_BOX(gtk_widget_get_parent(row));
    sidebar_remove_row(box, row);
}

/* right-click on the sidebar: show add/remove context menu */
static gboolean on_sidebar_button_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)u;
    if (ev->button != 3) return FALSE; /* right mouse button */

    GtkListBox *box = GTK_LIST_BOX(w);
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(box, (gint)ev->y);

    GtkWidget *menu = gtk_menu_new();

    if (row) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(row));
        const char *custom = child ? g_object_get_data(G_OBJECT(child), "custom") : NULL;
        const char *target = child ? g_object_get_data(G_OBJECT(child), "target") : NULL;
        if (custom && target && target[0]) {
            GtkWidget *mi = gtk_menu_item_new_with_label(tr("从侧边栏移除", "Remove from Sidebar"));
            g_signal_connect(mi, "activate", G_CALLBACK(on_sidebar_remove_place), child);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        }
    } else {
        GtkWidget *mi = gtk_menu_item_new_with_label(tr("添加到侧边栏…", "Add to Sidebar…"));
        g_signal_connect(mi, "activate", G_CALLBACK(on_sidebar_add_place), box);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)ev);
    return TRUE;
}


static char *expand_path(const char *in) {
    if (in[0] == '~') {
        const char *home = g_get_home_dir();
        return g_strdup_printf("%s%s", home, in + 1);
    }
    return g_strdup(in);
}


static int is_pseudo_fs(const char *fstype) {
    const char *pseudo[] = {
        "proc", "sysfs", "devpts", "devtmpfs", "tmpfs", "cgroup", "cgroup2",
        "overlay", "squashfs", "autofs", "fusectl", "securityfs", "debugfs",
        "tracefs", "pstore", "bpf", "mqueue", "hugetlbfs", "configfs",
        "ramfs", "binfmt_misc", "fuse.gvfs-fuse-daemon", NULL
    };
    for (int i = 0; pseudo[i]; i++)
        if (strcmp(fstype, pseudo[i]) == 0) return 1;
    return 0;
}


static void sidebar_add_devices(GtkListBox *box) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return;

    
    GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *seplbl = gtk_label_new(tr("位置", "Locations"));
    gtk_label_set_xalign(GTK_LABEL(seplbl), 0.0);
    
    GtkStyleContext *sctx = gtk_widget_get_style_context(seplbl);
    gtk_style_context_add_class(sctx, "sidebar-section");
    gtk_widget_set_margin_top(seplbl, 14);
    gtk_widget_set_margin_start(seplbl, 10);
    gtk_widget_set_margin_bottom(seplbl, 2);
    gtk_box_pack_start(GTK_BOX(sep), seplbl, FALSE, FALSE, 0);
    gtk_widget_show_all(sep);
    gtk_list_box_insert(box, sep, -1);
    GtkListBoxRow *seprow = GTK_LIST_BOX_ROW(gtk_widget_get_parent(sep));
    if (seprow) gtk_list_box_row_set_activatable(seprow, FALSE);

    char line[512];
    int shown = 0;
    while (fgets(line, sizeof(line), fp) && shown < 20) {
        char dev[256], mnt[256], fstype[64];
        if (sscanf(line, "%255s %255s %63s", dev, mnt, fstype) != 3) continue;
        if (is_pseudo_fs(fstype)) continue;
        
        if (strncmp(dev, "/dev/", 5) != 0) continue;
        
        if (strcmp(mnt, "/") == 0) continue;

        
        const char *basename = strrchr(mnt, '/');
        char label[256];
        if (basename && basename[1]) snprintf(label, sizeof(label), "%s", basename + 1);
        else snprintf(label, sizeof(label), "%s", mnt);

        sidebar_add(box, label, "drive", mnt, FALSE);
        shown++;
    }
    fclose(fp);
}



/* clicked item in the recent menu -> navigate there */
static void on_recent_menu_activate(GtkMenuItem *mi, gpointer data) {
    (void)mi;
    navigate_push((const char*)data);
}

/* clicked the "最近使用" row -> popup a submenu of recently-visited dirs */
static void on_recent_row_activated(GtkWidget *row, gpointer u) {
    (void)row; (void)u;
    char *fp = recent_file_path();
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    FILE *in = fopen(fp, "r");
    if (in) {
        char line[4096];
        while (fgets(line, sizeof(line), in)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0])
                g_ptr_array_add(arr, g_strdup(line));
        }
        fclose(in);
    }
    g_free(fp);

    GtkWidget *menu = gtk_menu_new();
    if (arr->len == 0) {
        GtkWidget *mi = gtk_menu_item_new_with_label(tr("（暂无历史）", "(No history)"));
        gtk_widget_set_sensitive(mi, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    } else {
        for (guint i = 0; i < arr->len; i++) {
            const char *p = g_ptr_array_index(arr, i);
            GtkWidget *mi = gtk_menu_item_new_with_label(p);
            g_signal_connect(mi, "activate", G_CALLBACK(on_recent_menu_activate), (gpointer)p);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        }
    }
    gtk_widget_show_all(menu);
    /* popup near the sidebar row */
    gtk_menu_popup_at_widget(GTK_MENU(menu), row, GDK_GRAVITY_EAST, GDK_GRAVITY_NORTH_WEST, NULL);
    /* keep arr alive until menu closes; simplest: leak-free via destroy callback */
    g_object_set_data_full(G_OBJECT(menu), "recent-arr", arr, (GDestroyNotify)g_ptr_array_free);
}

static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer u) {
    (void)box; (void)u;
    if (!row) return;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(row));
    if (!child) return;
    /* recent row -> popup history menu, not a direct navigate */
    if (g_object_get_data(G_OBJECT(child), "recent")) {
        on_recent_row_activated(child, NULL);
        return;
    }
    const char *target = g_object_get_data(G_OBJECT(child), "target");
    if (!target || !target[0]) return;
    char *full = expand_path(target);
    navigate_push(full);
    g_free(full);
}





static void on_tb_close(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_widget_destroy(g.window);
}

static void apply_window_shape(GtkWidget *w);
static void on_tb_minimize(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    gtk_window_iconify(GTK_WINDOW(g.window));
}

static Display *x_display(void) {
    GdkDisplay *d = gtk_widget_get_display(g.window);
    return d ? gdk_x11_display_get_xdisplay(d) : NULL;
}

static Window x_window(void) {
    GdkWindow *w = gtk_widget_get_window(g.window);
    if (!w || !GDK_IS_X11_WINDOW(w)) return 0;
    return gdk_x11_window_get_xid(w);
}

static void x_move(int x, int y) {
    Display *d = x_display();
    Window w = x_window();
    if (!d || !w) return;
    XMoveWindow(d, w, x, y);
    XFlush(d);
}

static void x_get_geom(int *x, int *y, int *ww, int *hh) {
    Display *d = x_display();
    Window w = x_window();
    if (!d || !w) {
        *x = *y = *ww = *hh = 0;
        return;
    }
    XWindowAttributes a;
    if (XGetWindowAttributes(d, w, &a)) {
        *x = a.x; *y = a.y; *ww = a.width; *hh = a.height;
    } else {
        *x = *y = *ww = *hh = 0;
    }
}

/* Get the usable work area (screen minus top bar / dock) for the
 * monitor the window currently lives on. Returns 0 on success. */
static int x_work_area(int *wx, int *wy, int *ww, int *wh) {
    GdkDisplay *d = gtk_widget_get_display(g.window);
    if (!d) return -1;
    GdkWindow *gwin = gtk_widget_get_window(g.window);
    GdkMonitor *mon = gwin ? gdk_display_get_monitor_at_window(d, gwin)
                           : gdk_display_get_primary_monitor(d);
    if (!mon) return -1;
    GdkRectangle rect;
    gdk_monitor_get_workarea(mon, &rect);
    *wx = rect.x; *wy = rect.y; *ww = rect.width; *wh = rect.height;
    return 0;
}

/* Toggle the mac-style maximize: we do NOT ask the WM (unreliable for
 * frameless windows under mutter/Xwayland). Instead we size the window
 * to the usable work area ourselves, using GDK's move_resize so GTK's
 * internal geometry cache stays in sync, then force a full re-layout. */
static void on_tb_maximize(GtkButton *b, gpointer u) {
    (void)b; (void)u;
    GtkWidget *win = g.window;
    GdkWindow *gwin = gtk_widget_get_window(win);
    GtkAllocation alloc;
    if (!gwin) return;

    if (g.maximized) {
        /* restore previous geometry + rounded corners */
        g.maximized = FALSE;
        g.resizing = FALSE;
        gdk_window_move_resize(gwin, g.pre_max_x, g.pre_max_y,
                               g.pre_max_w, g.pre_max_h);
        alloc.x = 0; alloc.y = 0;
        alloc.width = g.pre_max_w; alloc.height = g.pre_max_h;
        gtk_widget_size_allocate(win, &alloc);
        gtk_widget_queue_draw(win);
        gdk_window_process_updates(gwin, TRUE);
        apply_window_shape(win);
    } else {
        int cx, cy, cw, ch;
        x_get_geom(&cx, &cy, &cw, &ch);
        g.pre_max_x = cx; g.pre_max_y = cy;
        g.pre_max_w = cw; g.pre_max_h = ch;

        int wx, wy, ww, wh;
        if (x_work_area(&wx, &wy, &ww, &wh) != 0) return;

        g.maximized = TRUE;
        gtk_widget_shape_combine_region(win, NULL);
        gdk_window_move_resize(gwin, wx, wy, ww, wh);
        alloc.x = 0; alloc.y = 0;
        alloc.width = ww; alloc.height = wh;
        gtk_widget_size_allocate(win, &alloc);
        gtk_widget_queue_draw(win);
        gdk_window_process_updates(gwin, TRUE);
    }

    gdk_display_flush(gtk_widget_get_display(win));
}


#define RESIZE_MARGIN 10

static GdkWindowEdge get_resize_edge(int x, int y, int w, int h) {
    int left = x <= RESIZE_MARGIN;
    int right = x >= w - RESIZE_MARGIN;
    int top = y <= RESIZE_MARGIN;
    int bottom = y >= h - RESIZE_MARGIN;
    if (top && left)     return GDK_WINDOW_EDGE_NORTH_WEST;
    if (top && right)    return GDK_WINDOW_EDGE_NORTH_EAST;
    if (bottom && left)  return GDK_WINDOW_EDGE_SOUTH_WEST;
    if (bottom && right) return GDK_WINDOW_EDGE_SOUTH_EAST;
    if (left)            return GDK_WINDOW_EDGE_WEST;
    if (right)           return GDK_WINDOW_EDGE_EAST;
    if (top)             return GDK_WINDOW_EDGE_NORTH;
    if (bottom)          return GDK_WINDOW_EDGE_SOUTH;
    return (GdkWindowEdge)-1;
}

static void set_edge_cursor(GtkWidget *w, GdkWindowEdge edge) {
    GdkDisplay *d = gtk_widget_get_display(w);
    GdkWindow *gwin = gtk_widget_get_window(w);
    if (!d || !gwin) return;
    const char *name = NULL;
    switch (edge) {
        case GDK_WINDOW_EDGE_NORTH:
        case GDK_WINDOW_EDGE_SOUTH: name = "ns-resize"; break;
        case GDK_WINDOW_EDGE_EAST:
        case GDK_WINDOW_EDGE_WEST:  name = "ew-resize"; break;
        case GDK_WINDOW_EDGE_NORTH_EAST:
        case GDK_WINDOW_EDGE_SOUTH_WEST: name = "nesw-resize"; break;
        case GDK_WINDOW_EDGE_NORTH_WEST:
        case GDK_WINDOW_EDGE_SOUTH_EAST: name = "nwse-resize"; break;
        default: name = NULL; break;
    }
    if (name) {
        GdkCursor *c = gdk_cursor_new_from_name(d, name);
        if (c) {
            gdk_window_set_cursor(gwin, c);
            g_object_unref(c);
            return;
        }
    }
    gdk_window_set_cursor(gwin, NULL);
}

static GdkWindowEdge resize_hit_test(GtkWidget *w, GdkEvent *ev) {
    int ww = gtk_widget_get_allocated_width(w);
    int wh = gtk_widget_get_allocated_height(w);
    double rx = 0, ry = 0;
    if (!gdk_event_get_root_coords(ev, &rx, &ry)) return (GdkWindowEdge)-1;
    GdkWindow *gwin = gtk_widget_get_window(w);
    int ox = 0, oy = 0;
    if (gwin) gdk_window_get_origin(gwin, &ox, &oy);
    int x = (int)rx - ox;
    int y = (int)ry - oy;
    return get_resize_edge(x, y, ww, wh);
}

static gboolean on_win_motion(GtkWidget *w, GdkEventMotion *ev, gpointer u) {
    (void)u;
    if (g.resizing) return FALSE;
    GdkWindowEdge e = resize_hit_test(w, (GdkEvent*)ev);
    if (e != (GdkWindowEdge)-1) set_edge_cursor(w, e);
    else set_edge_cursor(w, (GdkWindowEdge)-1);
    return FALSE;
}

static gboolean on_win_button_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)u;
    if (ev->button != 1) return FALSE;
    GdkWindowEdge e = resize_hit_test(w, (GdkEvent*)ev);
    if (e == (GdkWindowEdge)-1) return FALSE;
    g.resizing = TRUE;
    gtk_window_begin_resize_drag(GTK_WINDOW(g.window), e,
                                 ev->button,
                                 (int)ev->x_root, (int)ev->y_root,
                                 ev->time);
    return TRUE;
}

static gboolean on_win_button_release(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)ev; (void)u;
    if (g.resizing) {
        g.resizing = FALSE;
        apply_window_shape(w);
    }
    return FALSE;
}


static gboolean on_dot_enter(GtkWidget *w, GdkEventCrossing *ev, gpointer u) {
    (void)ev;
    gboolean *hover = (gboolean*)u;
    *hover = TRUE;
    gtk_widget_queue_draw(w);
    return FALSE;
}
static gboolean on_dot_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer u) {
    (void)ev;
    gboolean *hover = (gboolean*)u;
    *hover = FALSE;
    gtk_widget_queue_draw(w);
    return FALSE;
}


typedef struct {
    double r, g, b;
    char sym[8];      
    gboolean *hover;  
} DotData;


static gboolean on_dot_draw(GtkWidget *w, cairo_t *cr, gpointer u);


typedef struct {
    gboolean left;   /* TRUE = '<' chevron, FALSE = '>' chevron */
} ChevronData;


static gboolean on_chevron_draw(GtkWidget *w, cairo_t *cr, gpointer u) {
    ChevronData *cd = (ChevronData*)u;
    /* macOS Finder back/forward chevron: standard 90-degree angle, grey
     * (#AEAEB2), round cap + round join, 1.6px stroke. Replicates SF Symbol
     * chevron.backward / chevron.forward. */
    cairo_set_source_rgb(cr, 0.68, 0.68, 0.70);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (cd->left) {
        /* chevron.left: apex points left, opened to the right */
        cairo_move_to(cr, 16.0, 7.0);
        cairo_line_to(cr, 9.5, 14.0);
        cairo_line_to(cr, 16.0, 21.0);
    } else {
        /* chevron.right: apex points right, opened to the left */
        cairo_move_to(cr, 12.0, 7.0);
        cairo_line_to(cr, 18.5, 14.0);
        cairo_line_to(cr, 12.0, 21.0);
    }
    cairo_stroke(cr);
    (void)w;
    return FALSE;
}


static GtkWidget *make_chevron(gboolean left) {
    GtkWidget *arrow = gtk_drawing_area_new();
    gtk_widget_set_size_request(arrow, 28, 28);
    ChevronData *cd = g_new(ChevronData, 1);
    cd->left = left;
    g_signal_connect(arrow, "draw", G_CALLBACK(on_chevron_draw), cd);
    g_object_set_data_full(G_OBJECT(arrow), "chevron-data", cd, g_free);
    return arrow;
}


static GtkWidget *make_dot(double r, double gr, double b2, const char *sym) {
    GtkWidget *dot = gtk_drawing_area_new();
    gtk_widget_set_size_request(dot, 14, 14);
    DotData *d = g_new(DotData, 1);
    d->r = r; d->g = gr; d->b = b2;
    g_strlcpy(d->sym, sym ? sym : "", sizeof(d->sym));
    d->hover = NULL;
    g_signal_connect(dot, "draw", G_CALLBACK(on_dot_draw), d);
    
    g_object_set_data_full(G_OBJECT(dot), "dot-data", d, g_free);
    return dot;
}


static gboolean on_dot_draw(GtkWidget *w, cairo_t *cr, gpointer u) {
    DotData *d = (DotData*)u;
    gboolean hover = d->hover ? *d->hover : FALSE;
    
    cairo_set_source_rgb(cr, d->r, d->g, d->b);
    cairo_arc(cr, 7.0, 7.0, 7.0, 0, 2*G_PI);
    cairo_fill(cr);
    
    if (hover && d->sym[0] != '\0') {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 9.0);
        cairo_text_extents_t te;
        cairo_text_extents(cr, d->sym, &te);
        cairo_move_to(cr, 7.0 - te.width/2.0 - te.x_bearing, 7.0 - te.height/2.0 - te.y_bearing);
        cairo_show_text(cr, d->sym);
    }
    (void)w;
    return FALSE;
}


static gboolean on_titlebar_press(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)u;
    if (ev->button != 1) return FALSE;
    
    GdkWindowEdge e = resize_hit_test(g.window, (GdkEvent*)ev);
    if (e != (GdkWindowEdge)-1) {
        gtk_window_begin_resize_drag(GTK_WINDOW(g.window), e, ev->button,
                                     (int)ev->x_root, (int)ev->y_root, ev->time);
        return TRUE;
    }

    g.dragging = TRUE;
    g.drag_start_x = (int)ev->x_root;
    g.drag_start_y = (int)ev->y_root;
    int wx, wy, ww, wh;
    x_get_geom(&wx, &wy, &ww, &wh);
    g.win_start_x = wx;
    g.win_start_y = wy;
    (void)w;
    return TRUE;
}

static gboolean on_titlebar_motion(GtkWidget *w, GdkEventMotion *ev, gpointer u) {
    (void)w; (void)u;
    if (!g.dragging) return FALSE;
    int dx = (int)ev->x_root - g.drag_start_x;
    int dy = (int)ev->y_root - g.drag_start_y;
    x_move(g.win_start_x + dx, g.win_start_y + dy);
    return TRUE;
}

static gboolean on_titlebar_release(GtkWidget *w, GdkEventButton *ev, gpointer u) {
    (void)w; (void)ev; (void)u;
    g.dragging = FALSE;
    return FALSE;
}


static GtkWidget *build_mac_titlebar(void) {
    
    GtkWidget *ebox = gtk_event_box_new();
    gtk_widget_set_hexpand(ebox, TRUE);
    gtk_widget_add_events(ebox, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                               | GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK);
    g_signal_connect(ebox, "button-press-event", G_CALLBACK(on_titlebar_press), NULL);
    g_signal_connect(ebox, "motion-notify-event", G_CALLBACK(on_titlebar_motion), NULL);
    g_signal_connect(ebox, "button-release-event", G_CALLBACK(on_titlebar_release), NULL);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);  
    gtk_widget_set_hexpand(bar, TRUE);
    GtkStyleContext *ctx = gtk_widget_get_style_context(bar);
    gtk_style_context_add_class(ctx, "dsstore-titlebar");
    gtk_container_add(GTK_CONTAINER(ebox), bar);

    /* Red/yellow/green dots (12px, matching real Finder size) */
    static const double cols[3][3] = {
        {1.00, 0.373, 0.337},  /* #FF5F56 */
        {1.00, 0.741, 0.180},  /* #FFBD2E */
        {0.153, 0.788, 0.247}, /* #27C93F */
    };
    static const char *syms[3] = { "\xc3\x97", "\xe2\x88\x92", "+" };
    GtkWidget *dots[3];
    gboolean *hover_flags[3];
    for (int i = 0; i < 3; i++) {
        GtkWidget *btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        gtk_widget_set_size_request(btn, 14, 14);
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "traffic-dot");
        GtkWidget *dot = make_dot(cols[i][0], cols[i][1], cols[i][2], syms[i]);
        gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(btn), dot);

        hover_flags[i] = g_new0(gboolean, 1);
        g_object_set_data_full(G_OBJECT(btn), "hover", hover_flags[i], g_free);

        DotData *dd = g_object_get_data(G_OBJECT(dot), "dot-data");
        dd->hover = hover_flags[i];
        gtk_widget_add_events(btn, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_signal_connect(btn, "enter-notify-event", G_CALLBACK(on_dot_enter), hover_flags[i]);
        g_signal_connect(btn, "leave-notify-event", G_CALLBACK(on_dot_leave), hover_flags[i]);
        dots[i] = btn;
    }
    g_signal_connect(dots[0], "clicked", G_CALLBACK(on_tb_close), NULL);
    g_signal_connect(dots[1], "clicked", G_CALLBACK(on_tb_minimize), NULL);
    g_signal_connect(dots[2], "clicked", G_CALLBACK(on_tb_maximize), NULL);

    
    gtk_box_pack_start(GTK_BOX(bar), dots[0], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), dots[1], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), dots[2], FALSE, FALSE, 0);

    

    return ebox;
}


static void apply_theme(void) {
    const char *css =
        
        "window, .background { background-color: rgba(0,0,0,0); }"
        "dialog, dialog .background, GtkFileChooserDialog, GtkColorChooserDialog { background-color: #f2f2f2; }"
        ".dsstore-titlebar { background-color: rgba(242,242,242,0.92);"
        "                    min-height: 56px; padding: 0px 20px; }"
        ".dsstore-titlebar button { padding: 0px; min-width: 0px; min-height: 0px;"
        "                             border: none; box-shadow: none; background: transparent;"
        "                             outline: none; -gtk-icon-effect: none;"
        "                             -gtk-icon-shadow: none; }"
        ".traffic-dot { border: none; box-shadow: none; outline: none; background: transparent;"
        "              padding: 0px; min-width: 0px; min-height: 0px; }"
        "headerbar { background-color: rgba(0,0,0,0);"
        "            border: none; box-shadow: none; }"
        "headerbar entry { background-color: rgba(255,255,255,0.85);"
        "                 border: 1px solid rgba(0,0,0,0.08);"
        "                 border-radius: 6px; padding: 3px 8px; }"
        
        "#dot-symbol { color: rgba(0,0,0,0.55); font-size: 13px; font-weight: bold; }"
        
        "list { background-color: transparent; }"
        "list row { padding: 0px; margin: 0px; background-color: transparent; }"
        "list row { min-height: 28px; }"
        
        ".sidebar-row { padding: 1px 4px; border-radius: 6px; margin: 0px 6px; }"
        "row:hover .sidebar-row { background-color: rgba(0,0,0,0.05); }"
        "row:selected .sidebar-row { background-color: rgba(100,100,110,0.16); }"
        "row:selected .sidebar-row label { color: #1d1d1f; font-weight: 600; }"
        
        ".sidebar-section { color: #8e8e93; font-size: 11px; font-weight: 500; }"
        
        "iconview { background-color: transparent; }"
        "iconview:selected { background-color: rgba(120,120,128,0.28);"
        "                      border-radius: 6px; }"
        "iconview:hover { background-color: rgba(0,0,0,0.06); border-radius: 6px; }"
        
        "treeview { background-color: transparent; }"
        "treeview:selected { background-color: rgba(120,120,128,0.28);"
        "                     border-radius: 6px; }"
        "treeview:hover { background-color: rgba(0,0,0,0.05); }"
        "treeview header button { background-color: rgba(250,250,252,0.8); padding: 4px 6px; }"
        
        ".dsstore-toolbar { background-color: rgba(242,242,242,0.92);"
        "                   border-bottom: 1px solid rgba(0,0,0,0.08);"
        "                   padding: 4px 6px; }"
        
        ".toolbar-title { color: #1d1d1f; font-size: 13px; font-weight: 700; }"
        
        ".toolbar-btn { border: none; box-shadow: none; outline: none; background: transparent;"
        "              padding: 4px; min-width: 0px; min-height: 0px; border-radius: 6px;"
        "              -gtk-icon-effect: none; -gtk-icon-shadow: none;"
        "              color: rgba(0,0,0,0.60); }"
        ".toolbar-btn:hover { background-color: rgba(0,0,0,0.06); color: rgba(0,0,0,0.80); }"
        ".toolbar-btn:active { background-color: rgba(0,0,0,0.10); }"
        ".toolbar-btn:disabled { color: rgba(0,0,0,0.25); }"
        ".toolbar-btn image { -gtk-icon-effect: none; }"
        
        ".dsstore-statusbar { background-color: rgba(255,255,255,0.80);"
        "                     border-top: 1px solid rgba(0,0,0,0.08);"
        "                     padding: 1px 8px; }"
        ".pathbar { background-color: rgba(255,255,255,0.80);"
        "          border-top: 1px solid rgba(0,0,0,0.06);"
        "          padding: 0px 8px; }"
        ".pathbar button { color: #3a3a3c; font-size: 11px; }"
        ".pathbar button:hover { color: #000; }"
        ".statusbar-item { color: #3a3a3c; font-size: 10px; }"
        ".sidebar-panel { background-color: rgba(245,245,247,0.35); }"
        "paned > separator { background-color: rgba(0,0,0,0.06); }"
        
        "scrolledwindow { background-color: transparent; }"
        "scrolledwindow viewport { background-color: transparent; }"
        "iconview { background-color: transparent; }"
        
        "paned, box, overlay { background-color: transparent; }"
        
        /* 让文件内容区彻底透出最底层背景（GtkViewport/view/stack 全透明） */
        "scrolledwindow { background-color: transparent; }"
        "scrolledwindow viewport { background-color: transparent; }"
        "iconview { background-color: transparent; }";

    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_USER);  
    g_object_unref(prov);
}


#define CORNER_RADIUS 14


static gboolean on_window_draw(GtkWidget *w, cairo_t *cr, gpointer u) {
    (void)u;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);

    
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    
    double r = CORNER_RADIUS;
    double x = 0.5, y = 0.5;
    double w2 = width - 1, h2 = height - 1;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w2 - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w2 - r, y + h2 - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h2 - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);

    
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.96, 1.0);
    cairo_fill_preserve(cr);
    
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    return FALSE;
}


static cairo_region_t *rounded_region(int width, int height, int radius) {
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_A1, width, height);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    double r = radius;
    cairo_new_sub_path(cr);
    cairo_arc(cr, width - r, r, r, -G_PI / 2, 0);
    cairo_arc(cr, width - r, height - r, r, 0, G_PI / 2);
    cairo_arc(cr, r, height - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_region_t *region = gdk_cairo_region_create_from_surface(surf);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return region;
}




static void apply_window_shape(GtkWidget *w) {
    if (g.maximized) return;
    int width = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    if (width <= 0 || height <= 0) return;
    cairo_region_t *region = rounded_region(width, height, CORNER_RADIUS);
    gtk_widget_shape_combine_region(w, region);
    cairo_region_destroy(region);
}

static gboolean on_window_resize(GtkWidget *w, GdkEventConfigure *ev, gpointer u) {
    (void)ev; (void)u;
    if (g.maximized) return FALSE;
    /* keep rounded corners during live resize too; never drop to square */
    apply_window_shape(w);
    return FALSE;
}

/* 窗口销毁时清理全局资源，避免泄漏（多次开/关窗口时尤其重要） */
static void on_window_destroy(GtkWidget *w, gpointer u) {
    (void)w; (void)u;
    if (g.hist) { g_ptr_array_free(g.hist, TRUE); g.hist = NULL; }
    if (g.clip_list) { g_ptr_array_free(g.clip_list, TRUE); g.clip_list = NULL; }
    if (g.miller_cols) { g_ptr_array_free(g.miller_cols, TRUE); g.miller_cols = NULL; }
    g_free(g.dir); g.dir = NULL;
    dsdata_free(&g.data);
    gtk_main_quit();
}

int view_open(const char *dir) {
    if (!dir) return -1;

    g.dir = strdup(dir);
    if (!g.dir) return -1;
    g.icon_size = 48;
    dsdata_init(&g.data);
    g.hist = g_ptr_array_new_with_free_func(g_free);
    g.hist_pos = -1;

    gtk_init(NULL, NULL);
    apply_theme();

    g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g.window), g.dir);
    gtk_window_set_default_size(GTK_WINDOW(g.window), 1200, 760);
    g_signal_connect(g.window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    
    gtk_window_set_decorated(GTK_WINDOW(g.window), FALSE);

    
    {
        GdkScreen *screen = gtk_widget_get_screen(g.window);
        GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
        if (visual) {
            gtk_widget_set_visual(g.window, visual);
            gtk_widget_set_app_paintable(g.window, TRUE);
        }
        g_signal_connect(g.window, "draw", G_CALLBACK(on_window_draw), NULL);
        g_signal_connect(g.window, "configure-event", G_CALLBACK(on_window_resize), NULL);
    }

    
    g.header = build_mac_titlebar();

    
    g.back_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(g.back_btn), GTK_RELIEF_NONE);
    {
        GtkWidget *a1 = make_chevron(TRUE);
        gtk_widget_set_valign(a1, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(a1, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(g.back_btn), a1);
    }
    g.fwd_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(g.fwd_btn), GTK_RELIEF_NONE);
    {
        GtkWidget *a2 = make_chevron(FALSE);
        gtk_widget_set_valign(a2, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(a2, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(g.fwd_btn), a2);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(g.back_btn), "toolbar-btn");
    gtk_style_context_add_class(gtk_widget_get_style_context(g.fwd_btn), "toolbar-btn");
    gtk_widget_set_tooltip_text(g.back_btn, tr("后退", "Back"));
    gtk_widget_set_tooltip_text(g.fwd_btn, tr("前进", "Forward"));

    
    g.breadcrumb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkStyleContext *pb_ctx = gtk_widget_get_style_context(g.breadcrumb);
    gtk_style_context_add_class(pb_ctx, "pathbar");
    gtk_widget_set_size_request(g.breadcrumb, -1, 12);

    
    g_signal_connect(g.window, "key-press-event", G_CALLBACK(on_key_press), NULL);

    g_signal_connect(g.back_btn, "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(g.fwd_btn, "clicked", G_CALLBACK(on_forward_clicked), NULL);

    
    g.icon_btn = gtk_button_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_MENU);
    g.list_btn = gtk_button_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_MENU);
    gtk_style_context_add_class(gtk_widget_get_style_context(g.icon_btn), "toolbar-btn");
    gtk_style_context_add_class(gtk_widget_get_style_context(g.list_btn), "toolbar-btn");
    gtk_widget_set_tooltip_text(g.icon_btn, tr("图标视图", "Icon View"));
    gtk_widget_set_tooltip_text(g.list_btn, tr("列表视图", "List View"));
    g_signal_connect(g.icon_btn, "clicked", G_CALLBACK(on_switch_icon), NULL);
    g_signal_connect(g.list_btn, "clicked", G_CALLBACK(on_switch_list), NULL);

    
    


    
    GtkWidget *crumb_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(crumb_row), g.back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(crumb_row), g.fwd_btn, FALSE, FALSE, 0);
    
    g.title_label = gtk_label_new("Linder");
    GtkStyleContext *ttc = gtk_widget_get_style_context(g.title_label);
    gtk_style_context_add_class(ttc, "toolbar-title");
    gtk_label_set_ellipsize(GTK_LABEL(g.title_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(crumb_row), g.title_label, TRUE, TRUE, 0);
    
    gtk_box_pack_start(GTK_BOX(crumb_row), g.icon_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(crumb_row), g.list_btn, FALSE, FALSE, 0);

    
    GtkWidget *search_btn = gtk_button_new_from_icon_name("system-search-symbolic", GTK_ICON_SIZE_MENU);
    gtk_style_context_add_class(gtk_widget_get_style_context(search_btn), "toolbar-btn");
    gtk_widget_set_tooltip_text(search_btn, tr("搜索", "Search"));
    gtk_box_pack_start(GTK_BOX(crumb_row), search_btn, FALSE, FALSE, 0);

    
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* Wrap the crumb row in an event box so the toolbar blank area
     * (title text, spacing) can drag the window, like macOS. */
    GtkWidget *crumb_ebox = gtk_event_box_new();
    gtk_widget_set_hexpand(crumb_ebox, TRUE);
    gtk_widget_set_size_request(crumb_ebox, -1, 42);
    gtk_widget_add_events(crumb_ebox,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(crumb_ebox), crumb_row);
    g_signal_connect(crumb_ebox, "button-press-event", G_CALLBACK(on_titlebar_press), NULL);
    g_signal_connect(crumb_ebox, "motion-notify-event", G_CALLBACK(on_titlebar_motion), NULL);
    g_signal_connect(crumb_ebox, "button-release-event", G_CALLBACK(on_titlebar_release), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), crumb_ebox, FALSE, FALSE, 2);

    GtkStyleContext *tb_ctx = gtk_widget_get_style_context(toolbar);
    gtk_style_context_add_class(tb_ctx, "dsstore-toolbar");

    
    GtkWidget *side_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(side_scroll, 230, -1);
    g.sidebar = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g.sidebar), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(side_scroll), g.sidebar);

    
    g.statusbar = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g.statusbar), 0.0);
    GtkStyleContext *sbctx = gtk_widget_get_style_context(g.statusbar);
    gtk_style_context_add_class(sbctx, "statusbar-item");

    
    GtkWidget *side_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkStyleContext *pctx = gtk_widget_get_style_context(side_box);
    gtk_style_context_add_class(pctx, "sidebar-panel");
    
    gtk_box_pack_start(GTK_BOX(side_box), g.header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side_box), side_scroll, TRUE, TRUE, 0);

    
    {
        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *hl = gtk_label_new(tr("个人收藏", "Favorites"));
        gtk_label_set_xalign(GTK_LABEL(hl), 0.0);
        GtkStyleContext *hctx = gtk_widget_get_style_context(hl);
        gtk_style_context_add_class(hctx, "sidebar-section");
        gtk_widget_set_margin_top(hl, 4);
        gtk_widget_set_margin_start(hl, 10);
        gtk_widget_set_margin_bottom(hl, 2);
        gtk_box_pack_start(GTK_BOX(hdr), hl, FALSE, FALSE, 0);
        gtk_widget_show_all(hdr);
        gtk_list_box_insert(GTK_LIST_BOX(g.sidebar), hdr, -1);
        GtkListBoxRow *hr = GTK_LIST_BOX_ROW(gtk_widget_get_parent(hdr));
        if (hr) gtk_list_box_row_set_activatable(hr, FALSE);
    }

    
    char *dl = xdg_user_dir("XDG_DOWNLOAD_DIR");
    char *doc = xdg_user_dir("XDG_DOCUMENTS_DIR");
    char *pic = xdg_user_dir("XDG_PICTURES_DIR");
    char *mus = xdg_user_dir("XDG_MUSIC_DIR");
    char *vid = xdg_user_dir("XDG_VIDEOS_DIR");
    char *desk = xdg_user_dir("XDG_DESKTOP_DIR");
    /* 「最近使用」特殊条目：点击弹出访问历史子菜单（不直接导航） */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GdkPixbuf *ip = load_sidebar_icon("recent", 32);
        GtkWidget *img = ip ? gtk_image_new_from_pixbuf(ip) : gtk_image_new_from_icon_name("folder", GTK_ICON_SIZE_MENU);
        if (ip) g_object_unref(ip);
        GtkWidget *lbl = gtk_label_new(tr("最近使用", "Recents"));
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
        GtkStyleContext *ctx = gtk_widget_get_style_context(row);
        gtk_style_context_add_class(ctx, "sidebar-row");
        gtk_widget_set_margin_start(row, 16);
        gtk_widget_set_margin_end(row, 6);
        gtk_widget_set_margin_top(row, 1);
        gtk_widget_set_margin_bottom(row, 1);
        gtk_widget_show_all(row);
        g_object_set_data(G_OBJECT(row), "recent", (gpointer)"1");
        gtk_list_box_insert(GTK_LIST_BOX(g.sidebar), row, -1);
    }
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("应用程序", "Applications"), "applications", "/usr/share/applications", FALSE);
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("桌面", "Desktop"), "desktop", desk && desk[0] ? desk : "~/Desktop", FALSE);
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("文稿", "Documents"), "documents", doc ? doc : "~/Documents", FALSE);
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("下载", "Downloads"), "downloads", dl ? dl : "~/Downloads", FALSE);
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("图片", "Pictures"), "pictures", pic ? pic : "~/Pictures", FALSE);
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("音乐", "Music"), "music", mus ? mus : "~/Music", FALSE);
    sidebar_add(GTK_LIST_BOX(g.sidebar), tr("影片", "Videos"), "movies", vid ? vid : "~/Videos", FALSE);

    
    sidebar_add_devices(GTK_LIST_BOX(g.sidebar));

    /* 加载用户自定义收藏（持久化） */
    load_favorites(GTK_LIST_BOX(g.sidebar));

    g_free(dl); g_free(doc); g_free(pic); g_free(mus); g_free(vid); g_free(desk);

    g_signal_connect(g.sidebar, "row-activated", G_CALLBACK(on_sidebar_row_activated), NULL);
    g_signal_connect(g.sidebar, "button-press-event", G_CALLBACK(on_sidebar_button_press), NULL);

    
    g.bg = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g.bg, TRUE);
    gtk_widget_set_vexpand(g.bg, TRUE);
    g_signal_connect(g.bg, "draw", G_CALLBACK(on_draw_bg), NULL);

    
    g.icon_store = gtk_list_store_new(2, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    g.iconview = gtk_icon_view_new_with_model(GTK_TREE_MODEL(g.icon_store));
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(g.iconview), 0);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(g.iconview), 1);
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(g.iconview), GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(g.iconview), 100);
    gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(g.iconview), 12);
    gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(g.iconview), 12);

    GtkWidget *icon_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_hexpand(icon_scroll, TRUE);
    gtk_widget_set_vexpand(icon_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(icon_scroll), g.iconview);

    g_signal_connect(g.iconview, "item-activated", G_CALLBACK(on_iconview_activated), NULL);
    g_signal_connect(g.iconview, "button-press-event", G_CALLBACK(on_iconview_button_press), NULL);
    g_signal_connect(g.iconview, "button-press-event", G_CALLBACK(on_rubber_press), NULL);
    g_signal_connect(g.iconview, "button-release-event", G_CALLBACK(on_rubber_release), NULL);
    g_signal_connect(g.iconview, "motion-notify-event", G_CALLBACK(on_rubber_motion), NULL);
    g_signal_connect(g.iconview, "selection-changed", G_CALLBACK(on_icon_selection_changed), NULL);
    g_signal_connect_after(g.iconview, "draw", G_CALLBACK(on_rubber_draw), NULL);

    gtk_icon_view_enable_model_drag_source(GTK_ICON_VIEW(g.iconview),
        GDK_BUTTON1_MASK, dnd_targets, 1, GDK_ACTION_MOVE);
    g_signal_connect(g.iconview, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    gtk_icon_view_enable_model_drag_dest(GTK_ICON_VIEW(g.iconview),
        dnd_targets, 1, GDK_ACTION_MOVE);
    g_signal_connect(g.iconview, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);

    
    g.list_store = gtk_list_store_new(5, GDK_TYPE_PIXBUF, G_TYPE_STRING,
                                      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g.listview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g.list_store));
    
    GtkTreeSelection *listsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g.listview));
    gtk_tree_selection_set_mode(listsel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(listsel, "changed", G_CALLBACK(on_list_selection_changed), NULL);

    GtkCellRenderer *rp = gtk_cell_renderer_pixbuf_new();
    GtkTreeViewColumn *c0 = gtk_tree_view_column_new_with_attributes(
        "", rp, "pixbuf", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g.listview), c0);

    GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c1 = gtk_tree_view_column_new_with_attributes(
        tr("名称", "Name"), r1, "text", 1, NULL);
    gtk_tree_view_column_set_expand(c1, TRUE);
    gtk_tree_view_column_set_resizable(c1, TRUE);
    gtk_tree_view_column_set_sort_column_id(c1, 1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g.listview), c1);

    GtkCellRenderer *r2 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c2 = gtk_tree_view_column_new_with_attributes(
        tr("大小", "Size"), r2, "text", 2, NULL);
    gtk_tree_view_column_set_sort_column_id(c2, 2);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g.listview), c2);

    GtkCellRenderer *r3 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c3 = gtk_tree_view_column_new_with_attributes(
        tr("类型", "Kind"), r3, "text", 3, NULL);
    gtk_tree_view_column_set_sort_column_id(c3, 3);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g.listview), c3);

    GtkCellRenderer *r4 = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c4 = gtk_tree_view_column_new_with_attributes(
        tr("修改日期", "Date Modified"), r4, "text", 4, NULL);
    gtk_tree_view_column_set_sort_column_id(c4, 4);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g.listview), c4);

    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_hexpand(list_scroll, TRUE);
    gtk_widget_set_vexpand(list_scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(list_scroll), g.listview);

    g_signal_connect(g.listview, "row-activated", G_CALLBACK(on_listview_row_activated), NULL);
    g_signal_connect(g.listview, "button-press-event", G_CALLBACK(on_listview_button_press), NULL);

    gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(g.listview), TRUE);
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(g.listview),
        GDK_BUTTON1_MASK, dnd_targets, 1, GDK_ACTION_MOVE);
    g_signal_connect(g.listview, "drag-data-get", G_CALLBACK(on_drag_data_get), NULL);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(g.listview),
        dnd_targets, 1, GDK_ACTION_MOVE);
    g_signal_connect(g.listview, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);

    
    g.miller_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g.miller_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g.miller_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_container_add(GTK_CONTAINER(g.miller_scroll), g.miller_box);

    
    g.stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(g.stack), icon_scroll, "icon");
    gtk_stack_add_named(GTK_STACK(g.stack), list_scroll, "list");
    gtk_stack_add_named(GTK_STACK(g.stack), g.miller_scroll, "miller");
    gtk_stack_set_visible_child_name(GTK_STACK(g.stack), "icon");

    
    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkStyleContext *srctx = gtk_widget_get_style_context(status_row);
    gtk_style_context_add_class(srctx, "dsstore-statusbar");
    
    gtk_box_pack_start(GTK_BOX(status_row), g.statusbar, FALSE, FALSE, 0);
    
    g.zoom_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 48.0, 128.0, 8.0);
    gtk_range_set_value(GTK_RANGE(g.zoom_scale), 64.0);
    gtk_scale_set_draw_value(GTK_SCALE(g.zoom_scale), FALSE);
    gtk_widget_set_size_request(g.zoom_scale, 120, -1);
    gtk_widget_set_halign(g.zoom_scale, GTK_ALIGN_END);
    g_signal_connect(g.zoom_scale, "value-changed", G_CALLBACK(on_zoom_changed), NULL);
    GtkWidget *spacer = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(status_row), spacer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(status_row), g.zoom_scale, FALSE, FALSE, 0);

    
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(right_box), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_box), g.stack, TRUE, TRUE, 0);
    /* 路径栏（面包屑）放底部、状态栏上方（与 mac Finder 一致） */
    gtk_box_pack_start(GTK_BOX(right_box), g.breadcrumb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_box), status_row, FALSE, FALSE, 0);

    
    GtkWidget *main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(main_paned), side_box, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(main_paned), right_box, TRUE, TRUE);

    
    g.overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(g.overlay), g.bg);
    gtk_overlay_add_overlay(GTK_OVERLAY(g.overlay), main_paned);
    gtk_container_add(GTK_CONTAINER(g.window), g.overlay);

    
    gtk_widget_set_events(g.window, gtk_widget_get_events(g.window) |
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
        GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK);
    gtk_widget_set_events(g.overlay, gtk_widget_get_events(g.overlay) |
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
        GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK);
    g_signal_connect(g.window, "motion-notify-event", G_CALLBACK(on_win_motion), NULL);
    g_signal_connect(g.window, "button-press-event", G_CALLBACK(on_win_button_press), NULL);
    g_signal_connect(g.window, "button-release-event", G_CALLBACK(on_win_button_release), NULL);
    g_signal_connect(g.overlay, "motion-notify-event", G_CALLBACK(on_win_motion), NULL);
    g_signal_connect(g.overlay, "button-press-event", G_CALLBACK(on_win_button_press), NULL);
    g_signal_connect(g.overlay, "button-release-event", G_CALLBACK(on_win_button_release), NULL);

    
    reload_dsstore();
    refresh_views();
    refresh_breadcrumb();

    
    hist_push(g.dir);
    gtk_widget_set_sensitive(g.back_btn, FALSE);
    gtk_widget_set_sensitive(g.fwd_btn, FALSE);

    gtk_widget_show_all(g.window);
    
    apply_window_shape(g.window);
    gtk_main();

    dsdata_free(&g.data);
    free(g.dir);
    g_ptr_array_free(g.hist, TRUE);
    g_object_unref(g.icon_store);
    g_object_unref(g.list_store);
    return 0;
}
