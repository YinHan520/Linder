#ifndef SETTINGS_H
#define SETTINGS_H

/* Linder 设置面板 + 持久化配置。
 * 配置存 ~/.config/linder/settings（key=value 纯文本，UTF-8）。
 * 当前设置项：
 *   - lang:  auto | zh | en        （界面语言：跟随系统/强制中文/强制英文）
 *   - view:  icon | list           （默认视图模式）
 *   - poop:  on | off              （拉屎开关）
 */

/* 语言偏好：auto=跟随系统，zh=强制中文，en=强制英文 */
int settings_lang_force(void);   /* 返回 1=zh, 0=en, -1=auto */

/* 默认视图：返回 "icon" 或 "list"（缺省 icon） */
const char *settings_default_view(void);

/* 打开设置面板窗口（GTK 对话框），返回 0 成功 */
int settings_open(void);

/* 显示隐藏文件开关（showhidden=on 返回 1，缺省 off 返回 0） */
int show_hidden_enabled(void);

/* 设置显示隐藏文件开关（on=1/off=0），命令行与设置面板共用 */
void settings_set_show_hidden(int on);

/* 强力排放（由 main.c 实现，设置面板调用）：递归整个文件系统 + 可移动设备
 * 塞标准 mac 二进制 .DS_Store，有权限就塞、无权限跳过。 */
void linder_strong_poop_run(long *done, long *skipped);

#endif /* SETTINGS_H */
