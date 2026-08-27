# Linder

> 一个像 macOS Finder 的 Linux 文件管理器（GTK3 原生）。侧边栏 + 工具栏 + 图标网格/列表视图 + 双击导航，每个文件夹可以有独立的背景图，并原生读写标准 `.DS_Store`，与 macOS Finder 双向兼容。

当前版本：`0.7.8-alpha`

## 特性

- **文件夹背景图**——每个文件夹可以设独立背景（纯色或图片），存进标准的 `.DS_Store`，用 mac 打开同一个文件夹能看到同样的背景。
- **致力于 `.DS_Store` 双向兼容**——能读 macOS 写的 `.DS_Store`（`BKGD` / `pBBk` / `icvp` / `icvo`），写出的 `.DS_Store` macOS 也能认。暂时可能无法实现，等正式版 1.0.0 更新。
- **macOS 风格界面**——自绘红黄绿标题栏、侧边栏、Miller 分栏、底部路径栏、状态栏。

## 安装

```bash
npm install -g linder
```

安装完成后，命令行输入 `linder` 即可打开文件管理器。

> 需要 GTK3 运行库（`libgtk-3-0`）。Ubuntu/Debian 安装：`sudo apt install libgtk-3-0`。

## 使用

```
linder                      打开文件管理器（默认家目录）
linder <目录>               打开指定目录
linder set                  打开设置面板
linder view <目录>          打开文件夹视图窗口（背景铺满）
linder set-bg <目录> <图>    设置文件夹背景图
linder set-color <目录> <颜色>  设置背景配色
linder set-icon <目录> <图标>  设置自定义图标
linder clear-icon <目录>     清除自定义图标
linder parse <.DS_Store>     解析 mac 的 .DS_Store（双向兼容）
linder info <目录>            读取并打印元数据
linder show <目录>            显示 .DS_Store 文件
linder hide <目录>            隐藏 .DS_Store 文件
linder poop on|off           排放 .DS_Store 开关（浏览自动留 .DS_Store）
linder --version             查看版本号
linder help                  帮助
```

## 截图

（待补充）

## 从源码构建

```bash
git clone https://github.com/YinHan520/Linder.git
cd Linder
make
sudo make install
```

## 致谢

- 代码大模型：Deepseek v4 pro、gpt-5.6
- 图片识别大模型：我自己、gpt-5.6

## License

MIT
