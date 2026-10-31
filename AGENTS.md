# 全局项目规则（长期有效，所有会话必须遵守）

## 规则 1：SysMonitor（C 版）——已冻结，禁止任何改动

用户已经做好的桌面系统监视器，**任何时候都不要修改、重装、卸载、删除它**，除非用户明确说"改 C 版"：

| 项目 | 位置 |
|---|---|
| 源码 | `study/SysMonitor/`（sysmonitor.c、Makefile、README.md、sysmonitor.desktop.in） |
| 已安装程序 | `/usr/local/bin/sysmonitor`（正在运行，开机自启） |
| 自启动项 | `~/.config/autostart/sysmonitor.desktop` |
| 位置记忆 | `~/.config/sysmonitor/pos` |

- 它当前的功能：透明背景、蓝色字体、可拖动、所有磁盘（系统/C盘/D盘）、可被窗口覆盖（BELOW）、不进任务栏、开机自启、内存 ~7.5MB。
- 给 Java 版做参考时**只读不改**。
- 重新编译/安装/杀进程/改配置 全部视为违规操作。

## 规则 2：SysMonitor Java 版——全新独立项目，与 C 版互不重叠

- 用户的目标：**另外用 IntelliJ IDEA 单独写一个"纯 Java"的 SysMonitor**——在 Manjaro 的 IDEA 上开发，在 Windows 的 IDEA 上运行。
- 与 C 版**文件零重叠**，各自独立运行，不去替换、不去联动、不改 C 版的自启动。
- **不要求与 C 版逐项一致**：任务栏可以有条目、虚拟桌面跟随不强求、垫底行为可以近似实现。
- 目标纯 JDK（零第三方依赖）；数据层 Linux 用 /proc、Windows 用 MXBean + File API + 命令行采样（`System.getProperty("os.name")` 自动选择实现）。
- 开发用工作区是共享 NTFS 分区：Manjaro 的 `/run/media/root/win_D/study/` = Windows 的 `D:\study\`，两端同一份文件。
- **新项目路径（已定）**：`study/SysMonitorJava/`（Manjaro: `/run/media/root/win_D/study/SysMonitorJava/`，Windows: `D:\study\SysMonitorJava\`）。
- 代码结构：`src/sysmonitor/` 下 5 个类（SysMonitorApp / Snapshot / Disk / StatsProvider / LinuxStats / WindowsStats），纯 JDK 17+，无第三方依赖。
- 已装好 IntelliJ IDEA 社区版（`intellij-idea-community-edition 2026.2`，启动命令 `idea`，应用菜单可搜到）。
- **进度**：Java 版 v1 已完成并在 Manjaro 实测通过（透明窗口、蓝色字体、拖动、位置记忆、六行数据全对；内存 ~152MB）。Windows 侧代码路径（netstat/PowerShell 采集）等用户在 Windows 的 IDEA 里运行验证。C 版仍在桌面运行、未受影响。

## 规则 3：IntelliJ IDEA 中文界面（已配置好，勿破坏）

用户要求 IDEA 显示中文，已完成配置（2026.2 社区版，构建 262.8665.337）。**不要删除或修改以下内容**，否则 IDEA 会变回英文：

| 内容 | 位置 | 说明 |
|---|---|---|
| 中文语言包 | `~/.config/JetBrains/IdeaIC2026.2/plugins/localization-zh/lib/localization-zh.jar` | 官方语言包（Arch 包的源码构建未内置）；plugin.xml 的 until-build 已补丁为精确匹配本机构建 |
| 语言选择（真正持久化） | `~/.config/JetBrains/IdeaIC2026.2/options/ide.general.xml` 中 `LocalizationStateService` 组件的 `selectedLocale=zh-CN` | IDEA 的 "Language and Region" 机制 |
| 首次引导 | `~/.config/JetBrains/IdeaIC2026.2/early-access-registry.txt` 中 `i18n.locale=zh-CN` | 会被 IDE 消费后清空，属正常 |

- 恢复英文：把 `selectedLocale` 改为 `en`（或删除该 component）。
- IDEA 升级后语言包可能因 until-build 不匹配失效，需用新版本发行包里的 `localization-zh.jar` 替换并补丁 until-build。
- 语言包只从**用户插件目录**加载（发行目录 `/usr/share/idea/plugins` 下新增的包不会被扫描）。
