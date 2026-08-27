/* strdup 是 POSIX 函数，C11 下需显式声明 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dsstore.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* 把图标路径转成 file:// URI（gio 需要 URI 形式） */
static char *to_file_uri(const char *path) {
    if (strncmp(path, "file://", 7) == 0) {
        return g_strdup(path); /* 已经是 URI（统一用 g_malloc，配合 g_free） */
    }
    /* 先转绝对路径，再用 GLib 正确构造 URI（处理相对路径/特殊字符） */
    char *abs = g_canonicalize_filename(path, NULL);
    if (!abs) abs = g_strdup(path);
    char *uri = g_filename_to_uri(abs, NULL, NULL);
    g_free(abs);
    return uri; /* 可能为 NULL（转义失败） */
}

/* 跑一个外部命令（用 execvp，避免 shell 注入），返回退出码（<0 失败） */
static int run_argv(char **argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* 子进程：静默执行 */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* 应用自定义图标到目录（调用 gio set metadata::custom-icon） */
int apply_icon(const char *dir, const char *icon) {
    if (!dir || !icon) return -1;

    char *icon_uri = to_file_uri(icon);
    if (!icon_uri) return -1;

    char *argv[] = {
        (char*)"gio", (char*)"set", (char*)dir,
        (char*)"metadata::custom-icon", icon_uri, NULL
    };
    int exitcode = run_argv(argv);
    g_free(icon_uri);
    return exitcode == 0 ? 0 : -1;
}

/* 清除自定义图标（设置空值） */
int clear_icon(const char *dir) {
    if (!dir) return -1;
    char *argv[] = {
        (char*)"gio", (char*)"set", (char*)dir,
        (char*)"metadata::custom-icon", (char*)"", NULL
    };
    int exitcode = run_argv(argv);
    return exitcode == 0 ? 0 : -1;
}
