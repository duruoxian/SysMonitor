# SysMonitor — 透明桌面系统监视器

纯 **C + X11 + FreeType** 编写的桌面悬浮系统监视器，专为低占用设计（本机实测常驻内存约 **8.7MB**，< 10MB；二进制仅 38KB）。

## 功能
- 实时显示：**CPU、内存、交换分区、所有磁盘分区（系统/C盘/D盘…自动枚举，`/boot/efi` 等引导分区跳过）、网络上下行、开机时长、时钟**，每秒刷新
- **桌面组件模式**（默认）：钉在桌面上、所有工作区可见、不进任务栏，**普通窗口可以正常覆盖它**；想改成永远置顶盖住别的窗口，把 `sysmonitor.c` 顶部 `ALWAYS_ON_TOP` 改 `1` 重新编译即可
- **背景透明**（ARGB 视觉，由合成器合成，只有一行极淡的深蓝背板保证可读性）
- **蓝色字体**（全套蓝色系配色）
- **左键按住拖动**即可随意移动，位置自动记忆，下次开机回到原位
- 右键菜单：**暂停/恢复刷新、退出**
- **开机自启动**（通过 `~/.config/autostart/sysmonitor.desktop`）
- 挂载/U盘 插拔时磁盘行数自动增减，窗口高度自适应

## 构建（Manjaro/Arch）
```bash
sudo pacman -S --needed gcc libx11 libxft fontconfig pkgconf
make
sudo make install autostart   # 安装到 /usr/local/bin 并创建自启动项
```
安装后立即运行：`sysmonitor &`（无需重启，下次开机会自动加载）。

## 使用
| 操作 | 效果 |
|---|---|
| 左键按住拖动 | 移动位置（自动保存） |
| 右键 | 菜单：暂停/恢复刷新、退出 |
| 等待 | 每秒自动刷新数据 |

## 配置（改完 `make && sudo make install` 重装生效）
都在 `sysmonitor.c` 顶部：
- `PLATE_A`：背板透明度，`0` = 纯文字全透明，`60`（默认）≈ 24% 暗色薄纱，`255` 不透明
- `FS_MAIN`：字号
- `init_colors()`：各处颜色（当前为蓝色系）

## 卸载
```bash
sudo make uninstall
```
（删除 `/usr/local/bin/sysmonitor` 和 `~/.config/autostart/sysmonitor.desktop`）

## 说明
- 显示层为 X11（Wayland 下经 XWayland 运行，KDE/GNOME 均正常）
- 若桌面无合成器（纯 TWM 等），透明会退化为深色底，其余功能不受影响
- 位置记忆文件：`~/.config/sysmonitor/pos`
