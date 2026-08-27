#!/bin/bash
# Linder 安装脚本：装二进制 + .desktop 文件 + 关联 inode/directory（双击文件夹用 Linder 打开）
# 可逆，见 uninstall.sh
# 用法：
#   交互式 sudo：  ./install.sh
#   非交互（传密码）: SUDO_PASS=你的密码 ./install.sh
set -e

PREFIX="${PREFIX:-/usr/local}"
BIN="$PREFIX/bin/linder"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DESKTOP="Linder.desktop"

# 封装 sudo：有 SUDO_PASS 走 -S 非交互
mysudo() {
    if [ -n "$SUDO_PASS" ]; then
        echo "$SUDO_PASS" | sudo -S "$@"
    else
        sudo "$@"
    fi
}

echo "==> 1/3 安装二进制到 $BIN"
mysudo install -m 0755 "$(dirname "$0")/linder" "$BIN"
# 兼容旧大写名，软链接兜底
mysudo ln -sf linder "$PREFIX/bin/Linder"

# bash 自动补全（列出子命令）
BASH_COMP_BIN="${XDG_DATA_HOME:-$HOME/.local/share}/bash-completion/completions/linder"
mkdir -p "$(dirname "$BASH_COMP_BIN")"
cat > "$BASH_COMP_BIN" <<'EOF'
_linder_complete() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local cmds="set view set-icon set-bg set-color clear-icon parse write write-bg show hide poop info help"
    COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
}
complete -F _linder_complete linder Linder
EOF

echo "==> 2/3 安装 .desktop 文件到 $APPS_DIR"
mkdir -p "$APPS_DIR"
# 替换 Exec 里的占位路径为实际安装路径
sed "s|Exec=linder|Exec=$BIN|" "$(dirname "$0")/$DESKTOP" > "$APPS_DIR/$DESKTOP"
chmod +x "$APPS_DIR/$DESKTOP"

echo "==> 3/5 关联 inode/directory"
xdg-mime default "$DESKTOP" inode/directory 2>/dev/null || \
    xdg-mime default "$DESKTOP" inode/directory

echo "==> 4/5 安装应用图标到 hicolor 主题（跨机器通用）"
SRC_ICON_DIR="$(dirname "$0")/icons"
ICON_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
for s in 512 256 128 64 48 32; do
    mkdir -p "$ICON_DIR/${s}x${s}/apps"
    if [ -f "$SRC_ICON_DIR/linder_${s}.png" ]; then
        cp "$SRC_ICON_DIR/linder_${s}.png" "$ICON_DIR/${s}x${s}/apps/linder.png"
    fi
done
# 也尝试装到系统级（需要 sudo，失败就跳过，用户级已够用）
if [ -n "$SUDO_PASS" ] || mysudo -n true 2>/dev/null; then
    for s in 512 256 128 64 48 32; do
        mysudo mkdir -p "/usr/share/icons/hicolor/${s}x${s}/apps"
        mysudo cp "$SRC_ICON_DIR/linder_${s}.png" "/usr/share/icons/hicolor/${s}x${s}/apps/linder.png" 2>/dev/null || true
    done
    mysudo gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true
fi

echo "==> 5/5 刷新缓存"
gtk-update-icon-cache -f "$ICON_DIR" 2>/dev/null || true
update-desktop-database "$APPS_DIR" 2>/dev/null || true

# 侧边栏自绘图标（macOS 风格粉色描边，SVG 矢量），复制到固定系统路径
echo "==> 6/6 安装侧边栏图标"
SIDEBAR_SRC="$(dirname "$0")/sidebar_svg"
SIDEBAR_DST="$PREFIX/share/linder/icons"
if [ -d "$SIDEBAR_SRC" ]; then
    mysudo mkdir -p "$SIDEBAR_DST"
    mysudo cp "$SIDEBAR_SRC"/*.svg "$SIDEBAR_DST/" 2>/dev/null || true
fi

echo ""
echo "✅ Linder 安装完成。双击文件夹将用 Linder 打开。"
echo "   卸载：$(dirname "$0")/uninstall.sh"
echo "   手动打开：Linder view <目录>"
