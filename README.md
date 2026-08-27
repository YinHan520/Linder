# Linder

> 一个像 macOS Finder 的 Linux 文件管理器（GTK3 原生）。侧边栏 + 工具栏 + 图标网格/列表视图 + 双击导航，**每个文件夹独立的背景图**，并原生读写标准 `.DS_Store`，与 macOS Finder **双向兼容**。

## 特性

- 🖼️ **文件夹背景图**——每个文件夹可以设独立背景（纯色或图片），存进标准的 `.DS_Store`，mac 打开同一个文件夹能看到同样的背景
- 🔁 **`.DS_Store` 双向兼容**——读 mac 写的 `.DS_Store`，写的 `.DS_Store` mac 也认（`BKGD`/`pBBk`/`icvp`/`icvo` 全解析）
- 🪟 **mac 风格界面**——自绘红黄绿标题栏、侧边栏、Miller 分栏、路径栏在底部、状态栏
- 🌐 **跨机自适应**——目录走 XDG 正源（桌面/文档/下载/图片各自随系统 locale），不硬编码任何路径
- 📦 **一键安装**——`npm i -g linder` 后直接敲 `linder`

## 安装

```bash
npm install -g linder
```

安装完成后，命令行输入 `linder` 即可打开文件管理器。

> 需要 GTK3 运行库（`libgtk-3-0`）。Ubuntu/Debian: `sudo apt install libgtk-3-0`。

## 使用

```
linder                打开文件管理器（默认家目录）
linder <目录>          打开指定目录
linder set            打开设置面板
linder poop on/off    开关「排放 .DS_Store」（带 [y/N] 确认）
linder help           帮助
```

## 从源码构建

```bash
git clone https://github.com/YinHan520/Linder.git
cd Linder
make
sudo make install
```

## License

MIT
