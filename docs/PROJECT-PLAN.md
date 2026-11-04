# 项目计划（2026-11-04 15:0x）

> 这份文档只回答两件事：**现在到哪了**、**接下来按什么顺序打**。
> 实证记录在 `docs/SESSION-HANDOFF.md`（每条都有真机日志时间戳），文件地图在 `docs/PROJECT-MAP.md`。
> 结论先行：**存储这一层已经关掉了**（显示 + 跨重启保留），剩下的主线只有 Wi-Fi 链路，以及用户看得见的三件事（语言、壁纸、息屏）。

---

## 0. 一页速览

| 层 | 状态 | 一句话 |
|---|---|---|
| GPU / 渲染 | ✅ 完成 | Mesa GLES + FD650，切换器、编辑模式、Settings 滚动、小组件都过 90 帧 |
| 输入 / 电源 / 电池 | ✅ 完成 | 触摸、电源键、音量键、电量上报 |
| 存储读取 | ✅ 完成 | 根卷报 20 GiB / 12.2 GiB 可用（`df` 实测，§13.13） |
| 存储持久化 | ✅ 完成 | `/private/var` 跨重启保留（§13.15，真机六轮，提交 `9c7b712`） |
| Settings → Wi-Fi 面板 | ✅ 完成 | 补了 configd / securityd / wifianalyticsd，面板能进（提交 `885af0d`） |
| **Wi-Fi 无线电** | 🟡 大半通 | 链路 L0（`24f4d4a`）、固件搬到 AMSS、**`en0` 起来、ath11k 返回 0**（照搬参考实现，§13.18）；扫描未拿到 |
| **系统语言 → 中文** | 🔍 取证中 | 偏好确实进到 SpringBoard（进程内实测）；卡在 bundle 认定（§13.16） |
| **默认壁纸** | ❌ 未通 | PosterBoard 从未运行，扩展每次都崩 |
| **真熄屏** | ❌ 未通 | 黑帧但面板仍通电（DCS 0x28 从未发过） |
| 音频 / BT / 相机 / 蜂窝 | ⛔ 未开始 | 不在当前阶段 |

**当前唯一主线**：Wi-Fi 的最后一层——让 iOS 真的去扫描并连上。
**路线已定**：分支 `wifi-ref-port` 上整套照搬参考实现（Linux ath11k 编进 XNU），
它已经跑到 `en0` + `ath11k returned 0`；`main` 保留我们自己那份 `alioth_pcie.c`。
**本周可交付**：扫到 SSID（或证明"扫描"被内核定时器问题挡住，答案同样明确）。

---

## 1. 11-03 关掉的三件事（都有真机证据）

### 1.1 存储显示（15:2x，提交 `3c3df45`）

- `ufs=2` 让 loader 发布块服务页，命令行自动带 `ufs_shared_pa=`，内核起 119 个 `/dev/rufs_*`。
- 关键修点不是挂载缓存，而是 **`apfs_vfs_getattr()`**：XNU 每次 `statfs(2)` 都走
  `vfs_update_vfsstat → vfs_getattr` 重新取值，写进 `mnt_vfsstatfs` 会被覆盖（这就是之前改四次没效果的真因）。
- 结果：`df /` 20,971,500 KB 总量 / 12,803,988 KB 可用（此前 2,433,812 / 0）。

### 1.2 数据卷持久化（15:5x，提交 `9c7b712`）

- 写窗口：**logdump 的 8–64 MB**（block 2048..16383，56 MB），手机自己的转储分区，不碰 Android userdata。
- 两个 28 MiB 槽轮换 + 头最后写 + CRC。实测 generation 1→4 交替；每次保存 8–10 次写请求 /
  24.5–29.5 MB，loader 逐块读回一致、0 拒绝。
- 载入实测 `5,088 directories, 1,011 files (23,607,719 bytes)`，对照全新启动只有 12 个种子文件；
  `containermanagerd` 上一轮创建的 `containers.sqlite3` 读回 `SQLite format 3`。
- 存档从 33.7 MB 裁到 24 MiB：按名字跳过任意层级的 `Caches`/`Logs`，丢掉 `DuetExpertCenter`（8 MB 遥测）。
- **默认已开启**：载荷命令行 = `ufs=2 ufs_write=1 var_store=1`，`boot/alioth-boot.img` 是新 loader。

### 1.3 调试通道（顺带，影响后面所有工作）

- **日志可以从 Android 直接读**：`adb exec-out 'su -c "dd if=/dev/block/by-name/logdump bs=1M count=2"'`，
  不必依赖 `fastboot get_staged`。`tools/logdump.py` 封装了它（含按会话截断）。
- 这条通道其实一直存在，只是没被用——前几轮"loader 失联、日志取不到"都能这样读。

---

## 2. 接下来的计划（按优先级）

打分口径：**用户可见收益 / 阻塞价值 / 风险可逆性 / 工作量**（1–5，5 最好或最省）。

| 优先级 | 项目 | 收益 | 阻塞 | 风险 | 工作量 | 为什么排这儿 |
|---|---|---|---|---|---|---|
| **P0** | 中文语言 | 5 | 3 | 4 | 2（1–3 天） | 持久化刚通，第一次能"改一次 → 重启 → 还在"；纯用户可见 |
| **P0** | Wi-Fi 链路 | 5 | 5 | 2 | 4（1–3 周） | 唯一主线，也是 Wi-Fi 开关/密码的真前提 |
| **P1** | ramfs 名字查找 bug | 3 | 4 | 4 | 2 | 新发现，可能正在悄悄吞掉设置；影响面比看起来大 |
| **P1** | 默认壁纸 | 5 | 1 | 3 | 4（1–2 周） | 观感最直接，但可能被 Metal 卡住 |
| **P2** | 真熄屏 | 3 | 1 | **1** | 5 | 屏幕已损，误写不可恢复，收益与风险不匹配 |
| **P3** | 音频 / BT / 相机 / 蜂窝 | 2 | 0 | 3 | 5 | 现阶段性价比最差 |

### P0-1 中文语言（1–3 天）

**为什么现在做**：以前"改成中文"重启就回英文，永远分不清是修复没生效还是被内存盘吞了。
现在 `/private/var` 会保留，实验第一次可判定。

**已经排除的（都有真机证据，见 §13.16）**：

| 假设 | 结论 | 证据 |
|---|---|---|
| 种子文件没写上 / 域不对 | ❌ 死 | 三处 `.GlobalPreferences.plist` 都是 `zh-Hans-CN` |
| cfprefsd 起晚了，进程读到时还是空 | ❌ 死 | SpringBoard **进程内**（uid 501）三种视图都返回 `[zh-Hans-CN, zh-Hans, en]` |
| 资源里没有中文 | ❌ 死 | `SpringBoard.loctable` 42 种语言、中英同键 438 个，`LOW_BATT_TITLE` = `电池电量不足` |
| 缺 `zh-Hans.lproj` | ❌ 不是原因 | 真机上 `SpringBoard.app` / `Preferences.app` 也只有 `en.lproj`、`en_GB.lproj` |

**剩下来的一条**：SpringBoard 是否被**判定**为"有中文"。两个子分支，一次取证就能分开：

- **H4a 只按 `.lproj` 枚举语言** → 只看到 en，回落英文。修法：`Info.plist` 加
  `CFBundleLocalizations`（不要补空 `zh_CN.lproj`，那会让字符串变成 key）。
- **H4b 会读 loctable，但读不动这张表** → 同样回落英文。修法在内核 decmpfs-4 读路径上，
  **和 `PingFang.ttc` 加载失败是同一个根因**（两者都是 type 4）。

**下一步（一轮上机）**：`lang3` 载荷已编好（`out/run-payload-lang3.bin`），上一轮在刷 system.apfs
阶段被中断，重跑即可。两个探针已经写在 `inject_sb.c` 里：

1. `CFBundleCopyLocalizationsForPreferences(42 种语言, NULL)` → `zh_CN` 还是 `en`？
2. `CFBundleCopyLocalizedString(board, "LOW_BATT_TITLE", …)` → `电池电量不足` 还是 `<no such key>`？

第 2 条同时回答"这张 loctable 读不读得动"，也就是语言与字体是不是同一件事。

**验收**：主屏 / Settings 文字变中文，**且重启后仍是中文**（这条以前无法验证）。

### P0-2 Wi-Fi（主线，剩下的最后一层）

**已通的（真机）**：

| 层 | 证据 | 在哪条路线 |
|---|---|---|
| 链路 L0 | `parf_ltssm 0x111`、端点 `17cb:1101` | 我们自己的 `alioth_pcie.c`（`24f4d4a`）与照搬路线都有 |
| 固件搬运 | BHI 512 KiB SBL → MHI M0 → BHIE 4,352,144 B AMSS（9 段）→ `execenv 0x2` | 照搬路线（§13.18） |
| 协议栈 | QMI 下 BDF/board/m3、`firmware init done`、`wlan mode 0`、`unified ready` | 照搬路线 |
| 接口 | **`en0` 建起来、`configd` 自配、`ath11k returned 0`** | 照搬路线 |

**没通的**：扫描。iOS 只在 Wi-Fi 面板打开时才让 `wifid` 扫，而"自动打开面板"的七轮尝试全部失败，
根因是**参考内核上我们的用户态定时器不可靠**（`sleep` 偶发不醒、`dispatch_after` 从不执行）。
详细七轮记录见 `docs/WIFI-REFERENCE-PORT.md` §5.3。

**三条可选的路（按性价比）**：

| 路 | 做法 | 代价 | 能证明什么 |
|---|---|---|---|
| **A** | 把 SSID/密码写进镜像 `fw/network`（参考实现的 `lx_os_network`），驱动开机自己连 | 需要你的网络凭据；**不需要任何延时/UI** | 端到端能不能上网（最有说服力） |
| **B** | 查内核的用户态定时器（我们进程 `sleep` 不醒，iOS 自己的正常） | 几轮实验 | 修好后 UI 自动化、以及其他延时逻辑全通 |
| **C** | 人在手机边时手动点一次 Settings → Wi-Fi | 一分钟 | 只证明"能扫到" |

**红线不变**：任何一步碰 `0x60000000` / `0x60100000` 前先确认链路已训练；一次只改一步；
PHY 的 ready 极性是 `bit6=1` 为**已就绪**。

### P1 ramfs 名字查找 bug（2 天，但可能很关键）

**现象**（本轮两次复现）：`var_store stamp` 创建文件后写成功、报 `29 bytes added`，
但**之后任何按名字打开都是 ENOENT**；而打包时的 `readdir` 能列到它（存档里能找到内容）。
同路径 `open(O_CREAT|O_TRUNC)` 也一样；`vol_ls` 在 job 里连 `statfs("/private/var")` 都 ENOENT。

**为什么重要**：iOS 的偏好写入普遍是"写临时文件 + rename"或"打开已有文件截断"。若新建名字
查不到，会出现"设置看着改了、实际没落盘"的静默丢失——和语言问题可能同源。

**怎么做**：读 `bsd/miscfs/ramfs` 的 `lookup` / `create` / `rename` 与 dcache 插入路径，
写一个 10 行复现（create → open → 失败），改完用同一脚本验收。

### P1 默认壁纸（1–2 周）

**现状**：PosterBoard 从未出现；真正渲染壁纸的是 AegirPoster / KaleidoscopePoster 扩展，
每启动崩 15/12 次（UniXNU 在同一台机器上的记录）。

**路线**：
1. 先诊断：PosterBoard 是否被 launchd 拉起、崩在哪（现在有 logdump 直读，成本低）。
2. 若只差进程 → 加 job + 排在 SpringBoard 之前。
3. 若被 Metal 卡死 → 走注入层静态图（`inject_sb.c` 挂钩壁纸层），**明确标注"贴图方案"**，不计入"真壁纸"。

### P2 真熄屏（研究项，单独立项）

面板走 DSI0 的 TPG 命令 FIFO 发 DCS（0x51 亮度 / 0x28 关屏 / 0x10 睡眠），UniXNU 记录该路径
只在 `secure_mode` 下可用、XNU 侧被 TEE 挡着，且他们的亮度探针只在 host 验证过。
**在拿到"可安全回退"的方案前不动手**——屏幕已有彩线，误写可能不可恢复。

### P3 明确不做（现阶段）

音频（mediaserverd + ADSP）、BT、相机、蜂窝：工作量大、收益低、不影响主线。

---

## 3. 里程碑

| 里程碑 | 判据 | 预计 |
|---|---|---|
| **M1 设置能留住** | 中文改一次 → 重启 → 仍中文；Wi-Fi 面板里的开关状态也留住 | 本周 |
| **M2 认出 endpoint** | `17cb:1101`，LTSSM L0 | ✅ 11-03（`24f4d4a`） |
| **M2.5 无线电起来** | `en0` 存在、`ath11k returned 0`、station 交给 iOS | ✅ 11-04（照搬路线，§13.18） |
| **M3 扫到 SSID** | Settings → Wi-Fi 列出真实热点（不是模拟） | 只差这一步 |
| **M4 真的联网** | 输入密码 → 关联 → DHCP → Safari 打开页面 | 视 M3 的固件 / 协议栈进展 |
| **M5 观感补齐** | 默认壁纸 + 图标布局跨重启保留 | 与 M2 并行 |

---

## 4. 每次上机的固定流程（别自己写 fastboot）

```bash
# 构建层在 WSL 的 ~/k40，仓库只放源码与产物；改完源码先 cp 过去再编（build.sh 用那边的副本）
export MSYS_NO_PATHCONV=1
wsl bash ubuntu/build-persist2.sh <tag>          # 重建场景 + 载荷（改 var_store / scene 时用）

# 上机
FB_T=600 SKIP_FIRMWARE=1 REBOOT=1 SECS=175 SHOTS=0 TAG=<tag> \
  IMG=boot/alioth-boot.img SYS=boot/system.apfs PAYLOAD=C:/Users/MrMoxing/k40-boot/run-payload-<tag>.bin \
  bash boot-one.sh

# 取日志（不依赖 get_staged，最可靠）
python tools/logdump.py log            # 拉 logdump 并按会话截断
python tools/logdump.py head 8388608   # 查存档头 RDWVAR
```

**三个必踩的坑（都吃过）**：
1. **logdump 的日志区不清零**：loader 每次从 0 写 `len` 字节，之后是上一轮残留。读日志必须按
   **第一个** `== END OF LOG ==` 截断，否则会把上一轮的输出当成这一轮（被骗过一次）。
2. **槽容量是硬门槛**：`newest()` 检查 `h->bytes > SLOT_BYTES - 4096`，改小槽会让旧存档立刻
   失效并打印 `no store yet`（`show` 却仍说 `header right`，很容易误判）。
3. **跨 shell 的 `$VAR` 会被吞**：WSL 操作一律写脚本文件 + 绝对路径，别拼一行长命令。
4. **Git Bash 的变量进不了 WSL**：`EXTRA_ARGS=x wsl bash …` 里 WSL 收不到，除非列进 `WSLENV`
   （`WSLENV=EXTRA_ARGS EXTRA_ARGS=x wsl bash …`）。`lang2` 那一轮就是这么白跑的：命令行里
   没有 `lang_probe=1`，探针一次都没跑。

---

## 5. 风险总表

| 风险 | 影响 | 缓解 |
|---|---|---|
| 0x60 区域访问挂死 | 需断电重启，可能丢当前运行状态 | 先训链路再碰；不自动重试；一次一步 |
| 存档写坏 / 半写 | 丢一代设置（回落到镜像骨架） | 双槽轮换 + 头最后写 + 两次 CRC，只丢自己 |
| ramfs 名字 bug | 设置静默丢失 | 见 P1，先复现再修 |
| 壁纸走注入层 | SpringBoard 崩溃循环 | 独立 boot-arg 开关，默认关 |
| DSI 直写 | 花屏 / 面板不可恢复 | P2 单独立项，先在纸上算寄存器 |

---

## 6. 与其它文档的分工

| 文档 | 管什么 |
|---|---|
| `docs/SESSION-HANDOFF.md` | 每一轮的实证记录（含失败尝试与原始日志路径）：§13.13 存储显示、§13.15 持久化 |
| `docs/PROJECT-MAP.md` | 代码 / 脚本在哪、上机速查、GPU 四条路径的辨析 |
| `docs/PROJECT-PLAN.md`（本文） | 现在到哪、接下来按什么顺序、验收判据 |
| `docs/WIFI-REFERENCE-PORT.md` | 照搬参考实现的可行性与实录：构建步骤、真机三轮、七轮扫描失败的原因 |
| `README.md` | 对外口径：只写真机验证过的 |

---

## 变更记录

- **2026-11-03 16:10**：初版。存储显示（`3c3df45`）与持久化（`9c7b712`）已关；主线收敛到
  Wi-Fi 链路；本周先交付中文语言；新增 P1「ramfs 名字查找 bug」。
- **2026-11-04 15:00**：Wi-Fi 这一层重估。链路（`24f4d4a`）与照搬参考实现（§13.18）都有真机证据：
  固件搬到 AMSS、`en0` 起来、`ath11k returned 0`；扫描未拿到，根因是参考内核上我们的用户态定时器
  不可靠。新增 `docs/WIFI-REFERENCE-PORT.md`，P0-2 改成"三条可选的路"。
- **2026-11-03 16:50**：语言取证（§13.16）。三处 prefs 域名 + 进程内实测把 H1/H3 全部排除；
  loctable 里有真中文（438 键）；问题收敛到"SpringBoard 是否被判为有中文"，并给出一次上机
  即可分开的 H4a/H4b。探针首轮把 SpringBoard 打崩（`AppleLocale` 当数组打印），已修；
  `lang3` 载荷编好但上机被中断，等待重跑。
