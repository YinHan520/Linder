/* strdup 是 POSIX 函数，C11 下需显式声明 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dsstore.h"

#include <glib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 初始化元数据 */
void meta_init(DsStoreMeta *m) {
    if (!m) return;
    m->icon = NULL;
    m->view = NULL;
    m->icon_size = -1;
    m->background = NULL;
    m->color = NULL;
}

/* 释放元数据 */
void meta_free(DsStoreMeta *m) {
    if (!m) return;
    if (m->icon) { free(m->icon); m->icon = NULL; }
    if (m->view) { free(m->view); m->view = NULL; }
    if (m->background) { free(m->background); m->background = NULL; }
    if (m->color) { free(m->color); m->color = NULL; }
    m->icon_size = -1;
}

/* 复制字符串（NULL 安全） */
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "dsstore: 内存不足\n"); exit(1); }
    return p;
}

/* 返回目录内 ds_store 文件路径（带点=隐藏 或 无点=显示），由 visible 决定 */
char *dsstore_path_for(const char *dir, int visible) {
    const char *fname = visible ? DSSTORE_FILE_VISIBLE : DSSTORE_FILE_HIDDEN;
    size_t len = strlen(dir) + 1 + strlen(fname) + 1;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/%s", dir, fname);
    return path;
}

/* 探测目录内实际存在的 ds_store 文件，返回实际路径或 NULL */
char *dsstore_find(const char *dir) {
    char *v = dsstore_path_for(dir, 1); /* 无点 dsstore */
    char *h = dsstore_path_for(dir, 0); /* 有点 .dsstore */
    /* 任一分配失败都释放另一个并返回 NULL */
    if (!v) { free(h); return NULL; }
    if (!h) { free(v); return NULL; }

    struct stat st;
    if (stat(v, &st) == 0 && S_ISREG(st.st_mode)) { free(h); return v; }
    if (stat(h, &st) == 0 && S_ISREG(st.st_mode)) { free(v); return h; }

    free(v);
    free(h);
    return NULL;
}

/* 写键值行（跳过空值和未设置项），返回写入字节数（<0 表示错误） */
static int write_kv2(FILE *fp, const char *key, const char *val) {
    if (val && val[0]) return fprintf(fp, "%s=%s\n", key, val);
    return 0;
}

/* 读取目录内的 ds_store 文件（自动探测显隐两种） */
int dsstore_read(const char *dir, DsStoreMeta *out) {
    if (!dir || !out) return -1;
    meta_init(out);

    char *path = dsstore_find(dir);
    if (!path) return 0; /* 没有文件，不算错误，返回空元数据 */

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return -1;

    char magic[DSSTORE_MAGIC_LEN + 1] = {0};
    if (fread(magic, 1, DSSTORE_MAGIC_LEN, fp) != DSSTORE_MAGIC_LEN) {
        fclose(fp);
        return -1;
    }
    if (strncmp(magic, DSSTORE_MAGIC, DSSTORE_MAGIC_LEN) != 0) {
        fprintf(stderr, "dsstore: 文件魔数不匹配（不是 ds_store 文件）\n");
        fclose(fp);
        return -1;
    }

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0; /* 去掉换行 */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;

        if (strcmp(key, KEY_ICON) == 0) {
            free(out->icon); out->icon = xstrdup(val);
        } else if (strcmp(key, KEY_VIEW) == 0) {
            free(out->view); out->view = xstrdup(val);
        } else if (strcmp(key, KEY_ICON_SIZE) == 0) {
            char *endp = NULL;
            long v = strtol(val, &endp, 10);
            out->icon_size = (endp && *endp == '\0' && v >= 0) ? v : -1;
        } else if (strcmp(key, KEY_BACKGROUND) == 0) {
            free(out->background); out->background = xstrdup(val);
        } else if (strcmp(key, KEY_COLOR) == 0) {
            free(out->color); out->color = xstrdup(val);
        }
    }

    fclose(fp);
    return 0;
}

/* 写入目录内的 ds_store 文件，visible 决定带不带点 */
int dsstore_write(const char *dir, const DsStoreMeta *meta, int visible) {
    if (!dir || !meta) return -1;
    char *path = dsstore_path_for(dir, visible);
    if (!path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) { free(path); return -1; }

    if (fwrite(DSSTORE_MAGIC, 1, DSSTORE_MAGIC_LEN, fp) != DSSTORE_MAGIC_LEN) {
        fclose(fp); free(path); return -1;
    }
    if (fputc('\n', fp) == EOF) { fclose(fp); free(path); return -1; }

    if (write_kv2(fp, KEY_ICON, meta->icon) < 0 ||
        write_kv2(fp, KEY_VIEW, meta->view) < 0 ||
        write_kv2(fp, KEY_BACKGROUND, meta->background) < 0 ||
        write_kv2(fp, KEY_COLOR, meta->color) < 0) {
        fclose(fp); free(path); return -1;
    }
    if (meta->icon_size >= 0) {
        if (fprintf(fp, "%s=%ld\n", KEY_ICON_SIZE, meta->icon_size) < 0) {
            fclose(fp); free(path); return -1;
        }
    }

    if (fclose(fp) != 0) { free(path); return -1; }
    free(path);
    return 0;
}

/* 切换可见性：visible=1 显示(无点dsstore)，visible=0 隐藏(.dsstore) */
int dsstore_set_visible(const char *dir, int visible) {
    char *target = dsstore_path_for(dir, visible);
    if (!target) return -1;

    /* 找出当前存在的文件 */
    char *cur = dsstore_find(dir);
    if (cur) {
        /* 若当前已是目标态，无需改动 */
        if (strcmp(cur, target) == 0) {
            free(cur);
            free(target);
            return 0;
        }
        if (rename(cur, target) != 0) {
            fprintf(stderr, "dsstore: 重命名失败\n");
            free(cur);
            free(target);
            return -1;
        }
        free(cur);
    } else {
        /* 没有文件，就按目标态新建一个空的（检查写入结果） */
        DsStoreMeta empty;
        meta_init(&empty);
        int rc = dsstore_write(dir, &empty, visible);
        meta_free(&empty);
        if (rc != 0) { free(target); return -1; }
    }

    free(target);
    return 0;
}

/* ================= 拉屎开关 =================
 * 「拉屎行为」：像 mac 一样，浏览/打开一个文件夹就自动留下一个 .DS_Store。
 * 默认为关（不拉屎）。开关存配置文件 ~/.config/dsstore/poop。
 */

/* 读拉屎开关配置目录，返回完整文件路径（需 free，失败返回 NULL） */
static char *poop_file(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char *p;
    if (xdg && xdg[0]) {
        size_t n = strlen(xdg) + 32;
        p = malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s/dsstore/poop", xdg);
    } else {
        if (!home) home = "/root";
        size_t n = strlen(home) + 40;
        p = malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s/.config/dsstore/poop", home);
    }
    return p;
}

/* 查询拉屎开关是否打开（默认关） */
int dsstore_poop_enabled(void) {
    char *p = poop_file();
    if (!p) return 0;
    FILE *fp = fopen(p, "r");
    free(p);
    if (!fp) return 0; /* 默认关 */
    int on = 0;
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), fp)) {
        if (strchr(buf, '1')) on = 1;
    }
    fclose(fp);
    return on;
}

/* 设置拉屎开关 on=1/off=0 */
int dsstore_set_poop(int on) {
    char *p = poop_file();
    if (!p) return -1;
    /* 确保目录存在（逐级 mkdir，不用 system() 避免命令注入） */
    char *slash = strrchr(p, '/');
    if (slash) {
        char saved = *slash;
        *slash = '\0';
        int m = g_mkdir_with_parents(p, 0755);
        *slash = saved;
        if (m != 0) { free(p); return -1; }
    }
    FILE *fp = fopen(p, "w");
    if (!fp) { free(p); return -1; }
    int r = fprintf(fp, "%d\n", on ? 1 : 0) < 0 ? -1 : 0;
    if (fclose(fp) != 0) r = -1;
    free(p);
    return r;
}
