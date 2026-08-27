#!/bin/bash
# Linder 卸载脚本：还原 inode/directory 关联到系统默认文件管理器，删掉安装的二进制和 .desktop
# 谨慎：只影响 Linder 自己装的东西，不动系统其他配置
set -e

PREFIX="${PREFIX:-/usr/local}"
BIN="$PREFIX/bin/Linder"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DESKTOP="Linder.desktop"

echo "==> 1/3 还原 inode/directory 默认关联"
# 尝试还原到 GNOME Files(Nautilus)，找不到就算了
if [ -f /usr/share/applications/org.gnome.Nautilus.desktop ]; then
    xdg-mime default org.gnome.Nautilus.desktop inode/directory 2>/dev/null || true
    echo "    已还原到 Nautilus"
else
    echo "    未找到 Nautilus，请手动 xdg-mime default <你的文件管理器>.desktop inode/directory"
fi

echo "==> 2/3 删除 .desktop 文件"
rm -f "$APPS_DIR/$DESKTOP"
update-desktop-database "$APPS_DIR" 2>/dev/null || true

echo "==> 3/4 删除二进制"
sudo rm -f "$BIN"

echo "==> 4/4 删除图标"
rm -rf "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/"*x*/apps/linder.png 2>/dev/null || true
find "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" -name 'linder.png' -delete 2>/dev/null || true
sudo find /usr/share/icons/hicolor -name 'linder.png' -delete 2>/dev/null || true

echo ""
echo "✅ Linder 已卸载。双击文件夹恢复系统默认文件管理器。"
