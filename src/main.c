/* strdup 是 POSIX 函数，C11 下需显式声明 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dsstore.h"
#include "dsstore_parse.h"
#include "dsstore_write.h"
#include "apply.h"
#include "view.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>   /* setenv/strdup */
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <gtk/gtk.h>   /* 带出 g_get_home_dir/gtk_init/gtk_main 声明，避免隐式声明导致指针截断崩溃 */

#define LINDER_VERSION "0.7.9-alpha"

static void usage(const char *prog) {
    printf("Linder %s — Linux 文件管理器（兼容 mac .DS_Store）\n\n", LINDER_VERSION);
    printf("用法：\n");
    printf("  %s                          打开文件管理器（默认家目录）\n", prog);
    printf("  %s <目录>                    打开指定目录到文件管理器\n", prog);
    printf("  %s set                       打开设置面板\n", prog);
    printf("  %s view <目录>               打开文件夹视图窗口（背景铺满）\n", prog);
    printf("  %s set-icon <目录> <图标>   设置自定义图标\n", prog);
    printf("  %s set-bg <目录> <背景图>   设置文件夹背景图\n", prog);
    printf("  %s set-color <目录> <颜色>  设置背景配色\n", prog);
    printf("  %s clear-icon <目录>         清除自定义图标\n", prog);
    printf("  %s parse <.DS_Store>        解析 mac 的 .DS_Store（双向兼容）\n", prog);
    printf("  %s write <输出路径>         生成标准 .DS_Store（测试）\n", prog);
    printf("  %s write-bg <路径> <图>     生成带图片背景的 .DS_Store\n", prog);
    printf("  %s show <目录>               显示 .DS_Store 文件\n", prog);
    printf("  %s hide <目录>               隐藏 .DS_Store 文件\n", prog);
    printf("  %s poop on|off               排放 .DS_Store 开关（浏览自动留 .DS_Store）\n", prog);
    printf("  %s poop-strong               强力排放（不可逆，所有可写目录塞 .DS_Store）\n", prog);
    printf("  %s info <目录>               读取并打印元数据\n", prog);
    printf("  %s help | -h | --help        显示帮助\n", prog);
    printf("  %s --version |-v             显示版本号\n", prog);
    printf("\n说明：\n");
    printf("  元数据文件名为 .DS_Store（标准名，跟 mac 一致）。\n");
}

/* parse 子命令：解析 mac .DS_Store 二进制，展示自定义信息 */
static int cmd_parse(const char *path) {
    DsStoreData d;
    dsdata_init(&d);
    int r = dsstore_parse_file(path, &d);
    if (r != 0) {
        fprintf(stderr, "Linder: 解析失败（不是合法的 .DS_Store 文件）\n");
        dsdata_free(&d);
        return 1;
    }

    printf("文件：%s\n", path);
    printf("视图：%s\n", d.view ? d.view : "(未设置)");
    if (d.icon_size >= 0)
        printf("图标大小：%ld\n", d.icon_size);
    else
        printf("图标大小：(未设置)\n");
    if (d.has_icon_pos)
        printf("图标位置：(x=%d, y=%d)\n", d.icon_x, d.icon_y);
    else
        printf("图标位置：(未设置)\n");

    switch (d.bg_type) {
    case DSBG_COLOR:
        printf("背景：纯色 R=%u G=%u B=%u（0-65535）\n",
               d.bg_r, d.bg_g, d.bg_b);
        break;
    case DSBG_PICTURE:
        printf("背景：图片（pict blob，%zu 字节）\n", d.bg_pict_len);
        if (d.bg_pict_path && d.bg_pict_path[0])
            printf("背景图片路径：%s\n", d.bg_pict_path);
        break;
    default:
        printf("背景：(未设置)\n");
        break;
    }

    dsdata_free(&d);
    return 0;
}

/* poop 子命令：拉屎开关 on/off（输指令时弹 [y/N] 确认） */
static int cmd_poop(const char *arg) {
    if (arg && (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0)) {
        int on = strcmp(arg, "on") == 0;
        printf("确定要%s「排放 .DS_Store」吗？[y/N] ", on ? "开启" : "关闭");
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof(line), stdin)) {
            printf("已取消。\n");
            return 0;
        }
        if (line[0] != 'y' && line[0] != 'Y') {
            printf("已取消。\n");
            return 0;
        }
        if (dsstore_set_poop(on ? 1 : 0) != 0) { fprintf(stderr, "Linder: 设置失败\n"); return 1; }
        printf("已%s「排放 .DS_Store」：%s\n", on ? "开启" : "关闭",
               on ? "浏览文件夹会自动留下 .DS_Store" : "不再自动生成 .DS_Store");
        return 0;
    }
    printf("当前排放 .DS_Store：%s\n", dsstore_poop_enabled() ? "开" : "关");
    return 0;
}

/* ---------- 强力排放 ---------- */

static long g_strong_done = 0;
static long g_strong_skipped = 0;

/* 判断挂载点是否是可移动设备（/media/*、/run/media/* 或 fstype 是 vfat/exfat/ntfs/fuse） */
static int is_removable_mount(const char *dev, const char *mnt, const char *fstype) {
    (void)dev;
    if (strncmp(mnt, "/media/", 7) == 0) return 1;
    if (strncmp(mnt, "/run/media/", 11) == 0) return 1;
    if (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "exfat") == 0 ||
        strcmp(fstype, "ntfs") == 0 || strcmp(fstype, "ntfs3") == 0 ||
        strcmp(fstype, "fuseblk") == 0) return 1;
    return 0;
}

/* 给单个可写目录塞一个空 .DS_Store（标准 mac 隐藏名） */
static int strong_deposit_one(const char *dir) {
    DsStoreMeta m;
    meta_init(&m);
    int r = dsstore_write(dir, &m, 0); /* visible=0 → 隐藏 .DS_Store（标准 mac 名） */
    meta_free(&m);
    return r;
}

/* 递归遍历：有权限（能写）的目录就塞，无权限（EACCES）跳过；不跨越挂载点之外的符号链接 */
static void strong_walk(const char *dir, int depth) {
    if (depth > 64) return;
    DIR *d = opendir(dir);
    if (!d) return; /* 打不开（无权限/不存在）→ 跳过 */

    /* 当前目录若能写，塞一个 .DS_Store */
    if (strong_deposit_one(dir) == 0) {
        g_strong_done++;
    } else {
        g_strong_skipped++;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
        /* 拼子路径 */
        char sub[4096];
        snprintf(sub, sizeof(sub), "%s/%s", dir, de->d_name);

        struct stat st;
        if (lstat(sub, &st) != 0) continue;
        /* 只递归真目录，跳过符号链接（避免环） */
        if (S_ISDIR(st.st_mode)) {
            strong_walk(sub, depth + 1);
        }
    }
    closedir(d);
}

/* 扫 /proc/mounts，对所有可移动设备挂载点递归塞 */
static void strong_deposit_removable(void) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char dev[256], mnt[256], fstype[64];
        if (sscanf(line, "%255s %255s %63s", dev, mnt, fstype) != 3) continue;
        if (!is_removable_mount(dev, mnt, fstype)) continue;
        strong_walk(mnt, 0);
    }
    fclose(fp);
}

/* 执行强力排放（供命令行 poop-strong 和设置面板共用）。
 * 递归整个文件系统 + 可移动设备，塞标准 mac 二进制 .DS_Store，
 * 有权限就塞、无权限跳过。结果通过 *done/*skipped 返回。 */
void linder_strong_poop_run(long *done, long *skipped) {
    g_strong_done = 0;
    g_strong_skipped = 0;

    /* 1) 可移动设备（U 盘/移动硬盘）优先塞 */
    strong_deposit_removable();

    /* 2) 递归整个文件系统（从 / 开始） */
    strong_walk("/", 0);

    if (done)   *done   = g_strong_done;
    if (skipped)*skipped= g_strong_skipped;
}

/* poop-strong 子命令：弹「不可逆」确认后，递归塞满所有有权限目录 + 可移动设备 */
static int cmd_poop_strong(void) {
    fprintf(stderr, "警告：强力排放会让所有可写的文件夹都生成 .DS_Store，"
                    "此操作【不可逆】。确定继续吗？[y/N] ");
    fflush(stderr);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        fprintf(stderr, "已取消。\n");
        return 0;
    }
    if (line[0] != 'y' && line[0] != 'Y') {
        fprintf(stderr, "已取消。\n");
        return 0;
    }

    fprintf(stderr, "开始强力排放（递归整个文件系统，无权限自动跳过）...\n");

    long done = 0, skipped = 0;
    linder_strong_poop_run(&done, &skipped);

    fprintf(stderr, "强力排放完成：成功 %ld 个文件夹，跳过 %ld 个（无权限/不可写）。\n",
            done, skipped);
    return 0;
}

/* write 子命令：生成标准 .DS_Store（先用纯色背景+视图+图标大小+位置验证骨架） */
static int cmd_write(const char *path) {
    DsStoreWrite w;
    memset(&w, 0, sizeof(w));
    w.has_bg = 1;
    w.bg_is_picture = 0;
    w.bg_r = 0x1234;   /* 纯色（测试值，16位） */
    w.bg_g = 0x5678;
    w.bg_b = 0x9abc;
    w.has_view = 1;
    w.view_is_icon = 1;
    w.has_icon_size = 1;
    w.icon_size = 64;
    w.has_icon_pos = 1;
    w.icon_x = 100;
    w.icon_y = 200;

    int r = dsstore_write_file(path, &w);
    if (r != 0) {
        fprintf(stderr, "Linder: 写入失败\n");
        return 1;
    }
    printf("已生成：%s\n", path);
    return 0;
}

/* write-bg 子命令：生成带【图片背景】的 .DS_Store（验证 pict/book 写入） */
static int cmd_write_bg(const char *path, const char *pict) {
    DsStoreWrite w;
    memset(&w, 0, sizeof(w));
    w.has_bg = 1;
    w.bg_is_picture = 1;
    w.bg_pict_path = pict;
    w.has_view = 1;
    w.view_is_icon = 1;
    w.has_icon_size = 1;
    w.icon_size = 64;

    int r = dsstore_write_file(path, &w);
    if (r != 0) {
        fprintf(stderr, "Linder: 写入失败\n");
        return 1;
    }
    printf("已生成（图片背景）：%s\n", path);
    return 0;
}

static int cmd_set_icon(const char *dir, const char *icon) {
    DsStoreMeta meta;
    meta_init(&meta);

    if (dsstore_read(dir, &meta) != 0) {
        fprintf(stderr, "Linder: 读取现有配置失败\n");
        meta_free(&meta);
        return 1;
    }
    if (meta.icon) free(meta.icon);
    char *tmp = strdup(icon);
    if (!tmp) { meta_free(&meta); return 1; }
    meta.icon = tmp;

    if (dsstore_write(dir, &meta, 1) != 0) {
        fprintf(stderr, "Linder: 写入失败\n");
        meta_free(&meta);
        return 1;
    }

    if (apply_icon(dir, icon) != 0) {
        fprintf(stderr, "Linder: 应用图标到 Nautilus 失败（但 ds_store 文件已写入）\n");
    } else {
        printf("已设置图标：%s → %s\n", dir, icon);
    }

    meta_free(&meta);
    return 0;
}

static int cmd_set_bg(const char *dir, const char *bg) {
    DsStoreMeta meta;
    meta_init(&meta);
    if (dsstore_read(dir, &meta) != 0) {
        fprintf(stderr, "Linder: 读取现有配置失败\n");
        meta_free(&meta);
        return 1;
    }
    if (meta.background) free(meta.background);
    char *tmp = strdup(bg);
    if (!tmp) { meta_free(&meta); return 1; }
    meta.background = tmp;

    if (dsstore_write(dir, &meta, 1) != 0) {
        fprintf(stderr, "Linder: 写入失败\n");
        meta_free(&meta);
        return 1;
    }
    printf("已设置背景图：%s → %s\n", dir, bg);
    meta_free(&meta);
    return 0;
}

static int cmd_set_color(const char *dir, const char *color) {
    DsStoreMeta meta;
    meta_init(&meta);
    if (dsstore_read(dir, &meta) != 0) {
        fprintf(stderr, "Linder: 读取现有配置失败\n");
        meta_free(&meta);
        return 1;
    }
    if (meta.color) free(meta.color);
    char *tmp = strdup(color);
    if (!tmp) { meta_free(&meta); return 1; }
    meta.color = tmp;

    if (dsstore_write(dir, &meta, 1) != 0) {
        fprintf(stderr, "Linder: 写入失败\n");
        meta_free(&meta);
        return 1;
    }
    printf("已设置配色：%s → %s\n", dir, color);
    meta_free(&meta);
    return 0;
}

static int cmd_clear_icon(const char *dir) {
    DsStoreMeta meta;
    meta_init(&meta);
    if (dsstore_read(dir, &meta) != 0) {
        fprintf(stderr, "Linder: 读取现有配置失败\n");
        meta_free(&meta);
        return 1;
    }
    if (meta.icon) { free(meta.icon); meta.icon = NULL; }

    int r1 = dsstore_write(dir, &meta, 1);
    int r2 = clear_icon(dir);
    if (r1 != 0 || r2 != 0) {
        fprintf(stderr, "Linder: 清除图标失败%s%s\n",
                r1 != 0 ? "（写入 ds_store 失败）" : "",
                r2 != 0 ? "（清除 Nautilus 元数据失败）" : "");
        meta_free(&meta);
        return 1;
    }
    printf("已清除图标：%s\n", dir);

    meta_free(&meta);
    return 0;
}

static int cmd_info(const char *dir) {
    DsStoreMeta meta;
    meta_init(&meta);
    int r = dsstore_read(dir, &meta);
    if (r != 0) {
        fprintf(stderr, "Linder: 读取失败\n");
        meta_free(&meta);
        return 1;
    }

    char *p = dsstore_find(dir);
    printf("目录：%s\n", dir);
    printf("文件：%s\n", p ? p : "(无 ds_store 文件)");
    if (p) free(p);
    printf("图标：%s\n", meta.icon ? meta.icon : "(未设置)");
    printf("视图：%s\n", meta.view ? meta.view : "(未设置)");
    printf("背景图：%s\n", meta.background ? meta.background : "(未设置)");
    printf("配色：%s\n", meta.color ? meta.color : "(未设置)");
    printf("图标大小：%s\n", meta.icon_size >= 0 ? "" : "(未设置)");
    if (meta.icon_size >= 0) printf("  → %ld\n", meta.icon_size);

    meta_free(&meta);
    return 0;
}

int main(int argc, char **argv) {
    /* GTK3 Wayland 下 gtk_window_move / XMoveWindow 会被 mutter 忽略，导致
     * 自绘无边框窗口拖不动。强制走 X11（Xwayland），窗口拖动、无边框、图标
     * 才正常。必须在任何 gtk_init 之前设置。 */
    setenv("GDK_BACKEND", "x11", 1);

    /* 裸命令（不带参数）→ 打开文件管理器 GUI（默认家目录） */
    if (argc < 2) {
        const char *h = g_get_home_dir();
        if (!h) return 1;
        char *home = strdup(h);
        if (!home) return 1;
        int r = view_open(home);
        free(home);
        return r;
    }

    const char *cmd = argv[1];

    /* help / -h / --help */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    /* --version / -v */
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("Linder %s\n", LINDER_VERSION);
        return 0;
    }

    /* set → 打开设置面板 */
    if (strcmp(cmd, "set") == 0) {
        gtk_init(&argc, &argv);
        int r = settings_open();
        if (r == 0) gtk_main();
        return r;
    }

    if (strcmp(cmd, "set-icon") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_set_icon(argv[2], argv[3]);
    }
    else if (strcmp(cmd, "set-bg") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_set_bg(argv[2], argv[3]);
    }
    else if (strcmp(cmd, "set-color") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_set_color(argv[2], argv[3]);
    }
    else if (strcmp(cmd, "clear-icon") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_clear_icon(argv[2]);
    }
    else if (strcmp(cmd, "view") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return view_open(argv[2]);
    }
    else if (strcmp(cmd, "parse") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_parse(argv[2]);
    }
    else if (strcmp(cmd, "write") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_write(argv[2]);
    }
    else if (strcmp(cmd, "write-bg") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_write_bg(argv[2], argv[3]);
    }
    else if (strcmp(cmd, "show") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        int r = dsstore_set_visible(argv[2], 1);
        if (r == 0) printf("已显示：%s\n", argv[2]);
        return r;
    }
    else if (strcmp(cmd, "hide") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        int r = dsstore_set_visible(argv[2], 0);
        if (r == 0) printf("已隐藏：%s\n", argv[2]);
        return r;
    }
    else if (strcmp(cmd, "poop") == 0) {
        return cmd_poop(argc > 2 ? argv[2] : NULL);
    }
    else if (strcmp(cmd, "poop-strong") == 0) {
        return cmd_poop_strong();
    }
    else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_info(argv[2]);
    }
    else {
        usage(argv[0]);
        return 1;
    }
}
