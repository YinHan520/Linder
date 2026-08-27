#ifndef APPLY_H
#define APPLY_H

/* 应用自定义图标到目录（调用 gio set metadata::custom-icon） */
int apply_icon(const char *dir, const char *icon);

/* 清除自定义图标 */
int clear_icon(const char *dir);

#endif /* APPLY_H */
