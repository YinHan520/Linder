/* Linder 设置面板 + 持久化配置。
 * 存 ~/.config/linder/settings（key=value 纯文本）。
 * 设置项：lang(auto|zh|en)、view(icon|list)、poop(on|off)
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "dsstore.h"

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static char *cfg_dir_path(void) {
    const char *base = g_get_user_config_dir();
    return g_strdup_printf("%s/linder", base);
}

static char *cfg_file_path(void) {
    char *dir = cfg_dir_path();
    char *p = g_strdup_printf("%s/settings", dir);
    g_free(dir);
    return p;
}

/* 读取一个 key 的值（需 g_free），找不到返回 NULL */
static char *cfg_get(const char *key) {
    char *path = cfg_file_path();
    FILE *fp = fopen(path, "r");
    g_free(path);
    if (!fp) return NULL;

    char *result = NULL;
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            result = g_strdup(line + klen + 1);
            break;
        }
    }
    fclose(fp);
    return result;
}

/* 写一个 key=value（保留其它 key，读到内存重写；原子写） */
static void cfg_set(const char *key, const char *value) {
    char *path = cfg_file_path();
    char *dir = cfg_dir_path();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_free(dir); g_free(path);
        g_printerr("Linder: 创建配置目录失败\n");
        return;
    }
    g_free(dir);

    /* 读现有全部行到一个 GPtrArray（同时去掉重复 key，只保留最新） */
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            g_ptr_array_add(lines, g_strdup(line));
        }
        fclose(fp);
    }

    /* 原子写：先写 settings.tmp，成功后 rename 覆盖 */
    char *tmppath = g_strdup_printf("%s.tmp", path);
    FILE *out = fopen(tmppath, "w");
    if (!out) {
        g_ptr_array_free(lines, TRUE);
        g_free(tmppath); g_free(path);
        return;
    }

    gboolean written = FALSE;
    size_t klen = strlen(key);
    for (guint i = 0; i < lines->len; i++) {
        const char *l = g_ptr_array_index(lines, i);
        if (strncmp(l, key, klen) == 0 && l[klen] == '=') {
            if (!written) {
                if (fprintf(out, "%s=%s\n", key, value) < 0) { /* 写失败 */ }
                written = TRUE;
            }
            /* 已是重复 key：跳过（只保留一条） */
        } else {
            if (fprintf(out, "%s\n", l) < 0) { /* 写失败 */ }
        }
    }
    if (!written)
        if (fprintf(out, "%s=%s\n", key, value) < 0) { /* 写失败 */ }

    int ok = (fclose(out) == 0);
    if (ok) {
        if (g_rename(tmppath, path) != 0) ok = 0;
    }
    if (!ok) g_unlink(tmppath);

    g_ptr_array_free(lines, TRUE);
    g_free(tmppath);
    g_free(path);
}

int settings_lang_force(void) {
    char *v = cfg_get("lang");
    if (!v) return -1;
    int r;
    if (strcmp(v, "zh") == 0) r = 1;
    else if (strcmp(v, "en") == 0) r = 0;
    else r = -1;
    g_free(v);
    return r;
}

/* 显示隐藏文件开关（showhidden=on 返回 1，缺省/其它值返回 0） */
int show_hidden_enabled(void) {
    char *v = cfg_get("showhidden");
    int on = (v && strcmp(v, "on") == 0) ? 1 : 0;
    g_free(v);
    return on;
}

/* 设置显示隐藏文件开关（命令行与设置面板共用同一入口） */
void settings_set_show_hidden(int on) {
    cfg_set("showhidden", on ? "on" : "off");
}

const char *settings_default_view(void) {
    /* 每次实时读，避免缓存导致切换视图后不生效 */
    char *v = cfg_get("view");
    static const char *icon = "icon";
    static const char *list = "list";
    if (v && strcmp(v, "list") == 0) {
        g_free(v);
        return list;
    }
    g_free(v);
    return icon;
}

/* ---- 设置面板 GUI ---- */

static GtkWidget *g_win;

/* 语言下拉变更 */
static void on_lang_changed(GtkComboBox *cb, gpointer u) {
    (void)u;
    int idx = gtk_combo_box_get_active(cb);
    const char *val = (idx == 0) ? "auto" : (idx == 1 ? "zh" : "en");
    cfg_set("lang", val);
}

/* 视图下拉变更 */
static void on_view_changed(GtkComboBox *cb, gpointer u) {
    (void)u;
    int idx = gtk_combo_box_get_active(cb);
    cfg_set("view", idx == 1 ? "list" : "icon");
}

/* 排放 .DS_Store 开关变更：直接生效（命令行 poop 才有 [y/N] 确认）。
 * 注意：GtkSwitch 的 "state-set" 信号回调必须返回 gboolean。 */
static gboolean on_poop_changed(GtkSwitch *sw, gboolean state, gpointer u) {
    (void)sw; (void)u;
    dsstore_set_poop(state ? 1 : 0);
    cfg_set("poop", state ? "on" : "off");
    return FALSE; /* 让 GTK 默认处理继续 */
}

/* 显示隐藏文件开关变更：直接生效 */
static gboolean on_showhidden_changed(GtkSwitch *sw, gboolean state, gpointer u) {
    (void)sw; (void)u;
    cfg_set("showhidden", state ? "on" : "off");
    return FALSE; /* 让 GTK 默认处理继续 */
}

static GtkWidget *make_lang_selector(void) {
    GtkWidget *cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), "跟随系统");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), "中文");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), "英文");
    int cur = settings_lang_force();
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb), cur < 0 ? 0 : (cur > 0 ? 1 : 2));
    g_signal_connect(cb, "changed", G_CALLBACK(on_lang_changed), NULL);
    return cb;
}

static GtkWidget *make_view_selector(void) {
    GtkWidget *cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), "图标视图");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb), "列表视图");
    const char *v = settings_default_view();
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb), strcmp(v, "list") == 0 ? 1 : 0);
    g_signal_connect(cb, "changed", G_CALLBACK(on_view_changed), NULL);
    return cb;
}

static GtkWidget *make_poop_switch(void) {
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), dsstore_poop_enabled() ? TRUE : FALSE);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_poop_changed), NULL);
    return sw;
}

static GtkWidget *make_showhidden_switch(void) {
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), show_hidden_enabled() ? TRUE : FALSE);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_showhidden_changed), NULL);
    return sw;
}

/* ============ 强力排放按钮 ============ */

/* 跑完弹结果窗 */
static void show_strong_result(GtkWidget *parent, long done, long skipped) {
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "强力排放完成\n成功 %ld 个文件夹，跳过 %ld 个（无权限/不可写）。",
        done, skipped);
    gtk_window_set_title(GTK_WINDOW(dlg), "强力排放");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* 确认后真正执行强力排放 */
static void do_strong_poop(GtkWidget *parent) {
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_OK_CANCEL,
        "强力排放会让所有可写的文件夹都生成 .DS_Store，\n此操作【不可逆】。确定继续吗？");
    gtk_window_set_title(GTK_WINDOW(dlg), "确认");
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r != GTK_RESPONSE_OK) return;

    long done = 0, skipped = 0;
    linder_strong_poop_run(&done, &skipped);
    show_strong_result(parent, done, skipped);
}

/* 强力排放按钮点击 */
static void on_strong_clicked(GtkButton *b, gpointer u) {
    (void)b;
    GtkWidget *win = (GtkWidget *)u;
    do_strong_poop(win);
}

static GtkWidget *make_strong_button(GtkWidget *win) {
    GtkWidget *btn = gtk_button_new_with_label("强力排放");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_strong_clicked), win);
    return btn;
}

/* 加一行：左 label 右 widget */
static void add_row(GtkGrid *grid, int row, const char *label, GtkWidget *widget) {
    GtkWidget *lbl = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_grid_attach(grid, lbl, 0, row, 1, 1);
    gtk_grid_attach(grid, widget, 1, row, 1, 1);
}

int settings_open(void) {
    if (g_win) {
        gtk_window_present(GTK_WINDOW(g_win));
        return 0;
    }

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win), "Linder 设置");
    gtk_window_set_default_size(GTK_WINDOW(g_win), 380, 220);
    gtk_container_set_border_width(GTK_CONTAINER(g_win), 16);
    g_signal_connect(g_win, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_win);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_add(GTK_CONTAINER(g_win), grid);

    add_row(GTK_GRID(grid), 0, "界面语言：", make_lang_selector());
    add_row(GTK_GRID(grid), 1, "默认视图：", make_view_selector());
    add_row(GTK_GRID(grid), 2, "排放 .DS_Store：", make_poop_switch());
    add_row(GTK_GRID(grid), 3, "显示隐藏文件：", make_showhidden_switch());
    add_row(GTK_GRID(grid), 4, "强力模式：", make_strong_button(g_win));

    gtk_widget_show_all(g_win);
    return 0;
}
