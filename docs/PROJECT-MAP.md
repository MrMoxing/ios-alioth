# 项目文件地图（2026-10-31）

> 目的：**下次动手前先查这里**，不要重新发明已有的东西。
> 这份地图覆盖仓库根、`boot/`、`docs/`、`tools/`、`src/` 三层，以及 WSL 里 `~/k40` 的构建环境。

---

## 0. 三层结构，先分清你在改哪一层

| 层 | 位置 | 是什么 | 谁在用 |
|---|---|---|---|
| **发布层** | 仓库根 `E:\...\ios-alioth` | 给人用的成品包：预编译镜像 + 一键脚本 + 文档 | 换台电脑直接 `.\boot-one.ps1` 就能跑 |
| **构建层** | `~/k40`（WSL） | 编译内核、打包镜像、构建用户态 | 改代码后重新出镜像 |
| **源码快照** | `src/hackios-xnu-kernel-main` | 上游 HackiOS 工程的副本 | 看代码、改驱动的**参考**（真正编译用的是 `~/k40`） |

⚠️ **两份源码的坑**：`src/.../xnu/files/**`（快照）与 `~/k40/xnu-src/xnu-8796.121.2/**`（编译树）
是两份。改完必须同步，否则"改了没生效"。2026-10-31 已把编译树版本覆盖回快照。

---

## 1. 仓库根：一键脚本（**先看这里，别自己写 fastboot 流程**）

| 文件 | 用途 | 关键参数 |
|---|---|---|
| `boot-one.ps1` | **Windows 一键上机**（PowerShell，13 KB） | `-Secs 45` `-Shots 3` `-Gap 5` `-Image` `-Payload` `-SystemImage` `-OutDir` `-NoSystem` `-Reboot` |
| `boot-one.sh` | Linux/WSL 同上（7.4 KB） | 环境变量 `SECS=45 SHOTS=3 GAP=5` |
| `boot-one.bat` | 双击入口，转调 ps1 | — |
| `fetch-firmware.ps1/.sh/.bat` | 独立的固件获取工具（重建 system.apfs 时才用；**boot-one 不调用**） | — |
| `SHA256SUMS.txt` | `boot/` 内镜像的校验和 | — |
| `README.md` | 20 KB 总说明（含"从零重建"的 §5.4） | — |

**`boot-one.ps1` 已经做完的事**（这轮我重写了一遍，是重复劳动）：
进 bootloader → `fastboot boot` → 确认 `product=xnuloader` → `oem ramdisk` →
`stage system.apfs` → `stage run-payload.bin` → `oem run` → 连拍截图（**纯 .NET 转 PNG，不需要 Python**）
→ 取日志 → 归档到 `runs/<时间戳>/`。`-NoSystem` 用载荷自带 `root.apfs` 当根（内核/驱动调试用，省 98 秒上传）。

---

## 2. `boot/`：预编译镜像（发布层自带，不必自己编）

| 文件 | 大小 | 是什么 |
|---|---|---|
| `alioth-boot.img` | 1.9 MB | BootShim + UEFI FD + loader 的 XNUP 容器，给 `fastboot boot` |
| `uefi-alioth.fd` | 3 MB | Mu-Silicium UEFI（alioth 端口） |
| `run-payload.bin` | 18.4 MB | **第二阶段载荷**：`kernel`(Mach-O) + `ramdisk`(4 MB APFS seed) + `cmdline` |
| `kernel-ios.bin` / `kernel-latest.bin` | 15.1 MB | 单独的内核（对照/备用） |
| `root.apfs` | 4 MB | **精简根**：redwood 用户态库、驱动注入库、**GPU 固件在里面的 `fw-gpu/`** |
| `system.apfs` | 2.32 GB | 完整 iOS 16.5 系统卷（SpringBoard 要它） |
| `inspect_payload.py` | 882 B | 解析 XNUP 载荷的条目表（**我上一轮又写了一个，重复**） |

**XNUP 格式**（`inspect_payload.py` 就是现成解析器）：`u32 magic 0x50554E58` + `u32 size`，
随后若干 `name[16] + size + reserved + data((16 字节对齐))`，以 24 字节空名结束。
生成用 `~/k40/xnuloader/mkpayload.py -o out.bin kernel=... ramdisk=... --cmdline "..."`。

---

## 3. `docs/`

| 文件 | 内容 |
|---|---|
| `PROJECT-README.md` | 里程碑、构建命令、启动参数表、已知问题（**部分过时**：说 GPU step 19 卡死，实际已到 step 20 并通过） |
| `SESSION-HANDOFF.md` | 74 KB 详细交接：硬件事实表、坑、§9 GPU 全记录、§10 下一步 |
| `unixnu-fft-gpu-ufs.md` | 与 UniXNU-alioth 的逐项对照 + **step 20 通过的实测**（§6） |
| `WIFI-REFERENCE-PORT.md` | **分支 `wifi-ref-port`**：照搬参考实现的可行性、构建步骤、真机三轮、七轮扫描失败的原因 |
| `PROJECT-PLAN.md` | 现在到哪、接下来按什么顺序、验收判据 |
| `evidence/` | 每轮真机日志与截图，按日期命名 |
| `hardware/` | 硬件档案 |
| `screenshots/` | 界面截图 |

---

## 4. `tools/`（发布层）与 `~/k40/tools`（构建层）

| 工具 | 用途 |
|---|---|
| `tools/pngcrop.py` | 截图 RAW(BGRX) → PNG，可裁剪放大。`pngcrop.py in.raw out.png W H stride x y w h [zoom]` |
| `tools/symbolize.py` | 用 `.dSYM` 的 DWARF 把 panic 地址反查函数名 |
| `tools/dump-gpu.sh` / `pci-topology.sh` | 从 Android 侧 dump GPU/PCIe 信息 |
| `tools/logdump.py` | 从 Android 直读 logdump：`log`（按会话截断）、`head N`（查存档头） |
| `tools/loctable-langs.py` / `loctable-keys.py` | 读 `.loctable`：有哪些语言、两种语言逐键对照（语言问题的取证工具） |
| `tools/mkp37.sh` / `mkp38.sh` | pcie=14 的载荷：BHI 只送 512 KiB SBL；p38 换成自有连续缓冲 + 回读 |
| `ubuntu/ref-wifi-build.sh` | **照搬参考实现的 Wi-Fi**：拉 Linux/FreeBSD 源码、编 `ath11k.o`、内核链进去 |
| `ubuntu/ref-wifi-payload.sh` | 用参考内核 + 我们的镜像打载荷（`SCENE=` 选场景；含 `QCA=1` 的固件名映射） |
| `~/k40/tools/gpu-test/` | **GPU 判据测试**：`test_gpu.c`（用户态直测）、`test_tables.py`（校验 SoC 表）、`run.sh` |
| `~/k40/tools/check-constants.py` | 源码常量 vs 设备树/ABL 表的一致性检查 |
| `~/k40/tools/apfs/` | APFS 读取器（`apfs.py`、`apfs-tool`），与内核实现交叉验证 |
| `~/k40/tools/mesa/` | **Metal/GLES 渲染链路**：`air/`、`air-*.patch`、`ir3-*.patch`、`freedreno-darwin.patch`、`redwood-drm/` |
| `~/k40/tools/mk-ath11k.sh` | 编 Wi-Fi 驱动对象（不跑它，内核里就是 `weak stub`） |
| `~/k40/tools/ufs-super.py` / `ufs-survey.py` | UFS 分区勘察 |
| `~/k40/tools/userdata-carve.py` | 从 userdata 切出 iOS 卷 |

---

## 5. `~/k40` 构建环境（真正编译的地方）

| 项 | 用途 |
|---|---|
| `xnu-build.sh` | 编内核。`DEVICE=alioth ./xnu-build.sh -j16 -k` |
| `common/build.sh` | 全流程：`kernel → loader → images → boot`。`SKIP="images"` 可跳过重建系统卷 |
| `common/run.sh` | 上机（WSL 侧版） |
| `xnu/apply.sh` | 把 `xnu/files/**` 套用进 `xnu-src`（**树已存在就跳过**，见 SESSION-HANDOFF §5.1） |
| `xnuloader/` | EFI loader 源码 + `mkpayload.py` |
| `mkxnuimg.py` | 打包 `out/<device>-boot.img` |
| `userland/` | iOS 用户态构建：`rc.ios`（常规启动脚本）、**`rc.ios.metal`（Metal 实验）**、`src/metal_*.m`、`libmini/` |
| `userland/system.apfs` | 2.32 GB 系统卷（`-NoSystem` 时可不用） |
| `prebuilt/` | **没有 theos 时的兜底内核/loader**——注意它不含你的改动 |

**预期内的"失败"**：`check_all_exports` 会在内核链接后报错，`common/build.sh:27` 注明
"on purpose"，判据是 `mach.development.bcm2837` 是否更新，不是 make 退出码。

---

## 6. 内核自写代码 `xnu/files/`（106 个文件，按功能分组）

### 6.1 硬件驱动 `bsd/dev/`（名字沿用上游 redwood，地址已换成 alioth）

| 文件 | 用途 |
|---|---|
| `redwood_adreno.c` (230 KB) | **GPU**：Adreno 650 逐级唤醒（step 1–22）、`gpu=1`/`adreno=N` 启动、`adreno_keep` 常驻、故障恢复 |
| `alioth_pcie.c` (26 KB) | Wi-Fi 的 PCIe 控制器（QCA6390，逐 step 探测） |
| `redwood_wifi.c` (190 KB) + `redwood_wlfw.c` + `redwood_apple80211.c` + `redwood_net.c` + `redwood_sta.c` | Wi-Fi：WCN6750/ath11k 侧、固件服务、Apple80211 ioctl、en0 接口、站点状态机 |
| `redwood_lx.c` | Linux ath11k 对象与 XNU 的桥（不编 ath11k 时为 weak stub） |
| `redwood_ufs.c` (61 KB) | UFS 分区块设备（`/dev/ufs_*`、`/dev/rufs_*`）——**经 UEFI 的 UFSDxe 读**，一个请求一个请求地来 |
| `redwood_ufshc.c` (50 KB) | 内核自带的 UFS 主控驱动（只读，补足并发） |
| `redwood_goodix.c` / `redwood_focaltech_fw.h` | 触摸（GT9916 / FTS 0x5652，含固件数组） |
| `redwood_buttons.c` | 电源/音量键（PMIC PON） |
| `redwood_battery.c` | 电量（PMPowerSource 数据源） |
| `redwood_cpufreq.c` | EPSS 调频 |
| `redwood_adsp.c` / `redwood_sound.c` / `redwood_speaker.c` / `redwood_mic.c` / `redwood_guard.c` / `redwood_audio.c` | 音频链（DSP → 功放 → 扬声器/麦克风，含保护算法） |
| `redwood_camera.c` (161 KB) | 相机（OV02B10 / S5KHM2SD 等） |
| `redwood_haptic.c` / `redwood_torch.c` | 马达（AW86927）/ 闪光灯 |
| `redwood_image.c` | 从 RAM disk 取文件（`fw/NAME`），各驱动加载固件用 |
| `redwood_sparse.c` | 稀疏块设备（只读部分块的镜像） |
| `redwood_input.c` | `/dev/redwood-touch` 样本队列 |
| `redwood_apfs_check.c` | APFS 卷一致性遍历（`apfs_check=N`） |
| `redwood_virtio_{blk,input,net,sound}.c` | **QEMU 专用**，真机不用 |

### 6.2 文件系统 `bsd/miscfs/`

| 文件 | 用途 |
|---|---|
| `apfs/apfs*.{c,h}` | **APFS 只读挂载**：`apfs_lib.c` 懂格式、`apfs_vfsops/vnops` 接 VFS |
| `apfs/apfs_write.c` | 只写"已存在文件的内容"（不改元数据） |
| `apfs/apfs_store.c` | 数据持久化：打包成 archive 写进 flash 的 `darwin` 分区（两槽各 2 GB） |
| `apfs/apfs_zbitmap.c` + `libzbitmap/` | iOS 16 cryptex 的 LZBITMAP 解压 |
| `apfs/libdeflate/` | 第三方 zlib/deflate（MIT） |
| `ramfs/ramfs.c` | **内存可写文件系统**：`/private/var`、`/tmp`，关机即失 |

### 6.3 `iokit/Kernel/`：Apple 闭源组件的替身（Mini*）与真机服务（Redwood*）

| 文件 | 替代/提供 |
|---|---|
| `MiniIOSurface.cpp` | IOSurface（跨进程像素缓冲） |
| `MiniIOMobileFramebuffer.cpp` | IOMobileFramebuffer（**显示 = 软件合成到 boot framebuffer**，vsync 是定时器） |
| `MiniAppleKeyStore.cpp` | AppleKeyStore（无密码设备的固定情形） |
| `MiniIONetworkStack.cpp` | IONetworkStack（只为让 configd 的接口命名器满意） |
| **`RedwoodGPU.cpp`** | **GPU 服务**：说 Linux msm DRM 的接口（freedreno 认得），缓冲/提交/等待/`surfaceBuffer()` |
| `RedwoodBattery.cpp` | IOPMPowerSource |
| `RedwoodWLAN.cpp` | 让 iOS 找到 Wi-Fi 接口的注册表项 |
| `MiniDrivers.h` | 上面这些的总说明 |
| `GenericARMPE.cpp` | 平台专家，启动时把这些服务挂上去 |

### 6.4 `osfmk/` 与 `libkern/`

| 文件 | 用途 |
|---|---|
| `arm64/gic_v3_irq.c` | GICv3 中断控制器（开源 XNU 没有） |
| `arm64/psci_smp.c` | 多核启动（PSCI，8 核只用 7 个是已知问题） |
| `arm64/loader_return.c` | **无串口时的内核日志通道**：panic/超时把 msgbuf 交回 loader |
| `arm64/user_exception_trace.c` | `systrace=1` 时记录用户态崩溃（本系统唯一的崩溃记录点） |
| `arm64/kpc.c` / `generic_arm64_support.c` / `ppl/{sart,uat}.h` | 可链接但空转的占位 |
| `minicrypto/mc_{aes,digests,provider}.c` | 替代 corecrypto：AES、MD5/SHA-1/384/512、HMAC、PRNG |
| `libkern/amfi/miniamfi*.c` | AMFI 与沙箱的替身（**不是安全机制**） |
| `bsd/kern/kern_minimac.c` | 同上，MAC 策略层 |
| `bsd/pthread/pthread_kext_static.c` | 静态链入的 pthread kext |

### 6.5 `bsd/sys/` 头文件

`redwood_input.h`（触摸/按键样本格式）、`redwood_audio.h`、`redwood_camera.h`、
`redwood_gpu_layout.h`（GPU 算术：chip id 编码、rail 投票、UBWC、HFI 头）、
`redwood_lx.h`、`redwood_net.h`、`redwood_apfs_check.h`。

---

## 7. 常用操作速查（**动手前先看这张表**）

| 我想…… | 用这个，别自己写 |
|---|---|
| **在真机上跑一轮（推荐）** | `FB_T=600 REBOOT=1 bash boot-one.sh`（**Git Bash**） |
| 只调内核、不要完整系统 | `SYS= bash boot-one.sh`（根 = 载荷自带 `root.apfs`，省 98 秒） |
| 换内核 | `PAYLOAD="C:/path/payload.bin" bash boot-one.sh` |
| 换系统卷 | `SYS="C:/path/system.apfs" bash boot-one.sh` |
| 要 IPSW（只在重建系统卷时） | 手动跑 `fetch-firmware.bat` / `bash fetch-firmware.sh`；启动流程不下载固件 |

### ⚠️ 上机三个必踩的坑（2026-10-31 实测）

1. **`FB_T=600` 是必须的**。`boot-one.sh` 给每次 fastboot 调用加了超时，默认
   `FB_T:-30` 只有 **30 秒**，而上传 `system.apfs` 需要 **~97 秒** → 默认值下 stage 必被杀掉。
   症状：脚本在 "resident buffer for the system image" 后失败，`stage` 总是跑 2 分 1 秒。
2. **路径必须是 Windows 风格**。脚本设了 `MSYS_NO_PATHCONV=1`，而 `fastboot.exe` 是 Windows 程序，
   不认 `/c/...`、`/e/...`。`IMG` / `SYS` / `PAYLOAD` 一律写成 `C:/...`、`E:/...`。
   否则报 `cannot load '/e/...': No such file or directory`。
3. **别用 PowerShell 版 `boot-one.ps1` 跑 stage**。它的 `Invoke-Tool` 用 .NET 重定向
   stdout/stderr，实测在同一台机器上 stage 稳定失败（4/4 失败，均在 97~100 秒处），
   而 Git Bash 直接调 fastboot 稳定成功。**上机统一用 `boot-one.sh` + Git Bash。**
4. 失败后设备常停在 `xnuloader fastboot`，脚本会因"0 台 adb 设备"直接退出。
   先 `fastboot reboot` 回 Android（可能报 `Too many links`，但实际会重启成功）再跑。
5. 截图/日志落点是**项目根目录** `run<HHMMSS>.png`（不是 `runs/`），因为路径转换未生效。
   日志若缺失，从 logdump 取：`adb shell 'su -c "dd if=/dev/block/sda25 of=/data/local/tmp/k.bin bs=1M count=8"'` → `adb pull`。
| 换内核跑 | 改 `~/k40/out/run-payload.bin`（`mkpayload.py`），再 `-Payload <路径>` |
| 看载荷里有什么 | `python boot/inspect_payload.py <file>` |
| 改启动参数 | 重新生成 payload 时 `--cmdline "..."`，或在 `boot-one.ps1 -Payload` 前改 |
| 截图转 PNG | `tools/pngcrop.py` |
| 反查 panic 地址 | `tools/symbolize.py` |
| 校验源码常量 | `~/k40/tools/check-constants.py` |
| 测 GPU 是否真能画 | `~/k40/tools/gpu-test/` |
| 读 APFS 镜像 | `~/k40/tools/apfs/apfs.py` |
| 编内核 | `DEVICE=alioth ./xnu-build.sh -j16 -k`（WSL 只有 7 GB 内存，别 -j32） |

---

## 8. GPU 渲染路径（四条，别搞混）

| 路径 | 实现 | 状态（2026-10-31 21:00 复核） |
|---|---|---|
| **A. 软件合成**（当前 SpringBoard **唯一**在用） | `MiniIOMobileFramebuffer.cpp`：swap 时把各 layer 像素 memcpy 到 boot framebuffer，vsync 是定时器 | ✅ 工作 —— 但这就是"界面没用上 GPU"的直接原因 |
| **B. GPU 2D blit 上屏**（内核自测，**已验证**） | `redwood_adreno.c:step_frame()`：CP 发 PM4 → 2D blit 引擎填三条色带 → `MiniIOMobileFramebufferShow()` | ✅ **已画出三色帧并上屏**，日志 `evidence/2026-10-31-gpu-step22-passed.log`，截图 `evidence/2026-10-31-gpu-frame-step22.png` |
| **C. GLES/Mesa → RedwoodGPU**（应用渲染路径） | `libredwood-gl.dylib`（`userland/src/eagl.c` + Mesa OSMesa）→ `RedwoodGPU.cpp` | ✅ **2026-10-31 21:15 已接通并实测**：`OpenGL ES 3.2 Mesa 25.0.7; FD650`、`the server has an accelerated renderer`、241 帧 × 122 draws/帧、累计上传 45.7 MB IOSurface 纹理、blur 5 级金字塔 |
| **D. Metal** | `libredwoodmetal.dylib` + `RedwoodMetal.bundle` + `libredwoodair.dylib`（AIR→IR3 编译器） | ⚠️ 驱动在，**编译器 `libredwoodair.dylib` 不在**。日志原文：`triangle: ... there is no GPU compiler here: either the Adreno was not started (gpu=1) or libredwoodair.dylib...` |

### 更正（2026-10-31 21:00）：上一版这里写的"应用已在用 GPU"是**误判**

那段 `RedwoodGPU: 101 submissions / 267 frames (53 a second) / waiting for the GPU` 的原文，
是在 logdump（环形缓冲，含多次启动历史）里读到的，**属于某次 epoch 为 10-26 的旧会话**，
不是 10-31 20:35 这轮。判据：logdump 里 10-31 那两段的 `redwood-gl:` 行数为 0。

**20:35 与 21:00 两轮的当前真实状态**（`runs/widget1.log.txt`，653 KB，完整系统 + `adreno=21 keep=1`）：

```
adreno: step 21: done
adreno: THE GPU STAYS ON for the system to draw with: the service RedwoodGPU
init: library insertion: libredwood.dylib backboardd containermanagerd...;
      libredwood-sb.dylib SpringBoard; libredwood-media.dylib mediaserverd; ...
RedwoodGPU: <0 条>          <- 用户态一次提交都没有
redwood-gl: frame <0 条>    <- GL 通路一次都没走
```

即：**内核侧 GPU 完全正常（能上电、能跑 CP、能画帧上屏），但用户态没有任何一条路径把界面交给 GPU。**
SpringBoard 的画面全部由 `MiniIOMobileFramebuffer` 用 CPU memcpy 合成。

> 注：`gpu=1` 这个便捷开关**在当前内核上有 bug** —— 它硬编码 `skip_steps = 1<<10|1<<11|1<<12`，
> 跳过 step 11（TrustZone）和 12（zap 签名），导致 step 19 的 `zap_unload` SMC 挂死。
> 用等价的显式参数绕开：`adreno=21 adreno_skip=0 adreno_keep=1`。

### 壁纸黑 / Dock 没模糊 / 小组件空白 —— 一条根因链

这三个现象不是三件事，是同一层缺失的三个表现：

| 现象 | 直接原因 | 证据 |
|---|---|---|
| 小组件空白、Dock 白块无毛玻璃 | iOS 16 的 widget 与 backdrop 模糊都要 GL/Metal 渲染，而**镜像里两个通路都没有**（`libredwood-gl.dylib` 缺、`libredwoodair.dylib` 缺） | 本次 653 KB 日志：`RedwoodGPU:` 0 条、`redwood-gl:` 0 条；`triangle: there is no GPU compiler here` |
| 壁纸黑 | 壁纸设置存 `/private/var`，而它是 **ramfs**，每次启动清空 → 等于从未设置过壁纸 | `memory: ... ramfs 253 MB`；`root.apfs/private/var` 是空目录 |
| 设备从未初始化 | `redwood: SpringBoard: activation answered here, **setup assistant refused**` | 同上日志 |

所以渲染错乱（撕裂、每行画两遍）是**软件合成路径**的问题，与 GPU 无关：
`MiniIOMobileFramebuffer` 在 memcpy 期间面板正在扫描，没有双缓冲/page-flip 的时序保护，
慢帧就会被扫到一半，表现为水平撕裂或内容重复。

**要做的不是再改内核**，而是把 C 路径补上：构建 Mesa（`tools/mk-mesa.sh`，产物
`libOSMesa.8.dylib` + `libredwoodair.dylib`）→ `userland/build.sh` 编出 `libredwood-gl.dylib`
→ 随之重建 `root.apfs`（会从 4 MB 涨到数十 MB）→ 再上机。

### 已经补上了：GL 通路可用（2026-10-31 21:15 实测）

两步，缺一不可：

```bash
# 1) Mesa + AIR 编译器（约 4 分钟；首次会下载 46 MB 源码）
cd ~/k40 && DRIVERS="softpipe,freedreno" AIR=1 bash tools/mk-mesa.sh
# 2) 带 GL 编 userland 并重打 root.apfs（4 MB -> 10.4 MB 压缩态）
cd ~/k40 && SKIP="kernel loader boot" ./common/build.sh alioth GL=1
```

**坑**：`mk-mesa.sh` 把 `$B/venv/bin` 放到 PATH 最前面，而那个 venv 里没有 meson，
ninja 的重新生成阶段会报 `No module named 'mesonbuild'`。先装一次即可：
`~/k40/xnu-deps/build/mesa/venv/bin/pip install meson`。

结果（`runs/gl1.log.txt`，758 KB，完整系统 + `adreno=21 adreno_skip=0 adreno_keep=1`）：

```
redwood-gl: 8 of 8 methods of EAGLContext are ours; OpenGL ES by Mesa, off-screen (OSMesa)
redwood-gl: an OpenGL ES 3 context for CoreAnimation ... OpenGL ES 3.2 Mesa 25.0.7; FD650
redwood:    the server has an accelerated renderer (0x144b044e0)
redwood-gl: frame 1: 136 GL calls, 4 draws ... 1 textures refilled from IOSurfaces (10962 KB)
redwood-gl: a blur whose radius varies: level 1..4 of texture 762
```

界面上的变化：Dock 从"纯白圆角块"变成**半透明材质条**，底部 Search 胶囊也带材质，
16 个图标齐全。壁纸仍是黑的（数据缺失，见上表）。

**已知缺口**：`redwood: CoreAnimation's OpenGL ES renderer has no function 40 ("sb3"): what needs it
is not drawn` —— `sb3` 是"阴影叠在背后画面"用的（切换器卡片、主屏编辑模式），
`eagl.c:1756` 记录了它为什么还没实现。表现就是**阴影整体缺失**，不影响其他绘制。

### 怎么直接验证小组件 / 编辑模式 / 切换器（轮子早就有了）

`userland/rc.jobs.*` 是一批现成场景脚本，由 launchd 的 `com.redwood.demo` 执行，
`rwtouch` 负责注入触摸：

| 想验证 | 用哪个 | 它做什么 |
|---|---|---|
| 小组件页 | `rc.jobs.widget`（本次新加） | 等 40 s 后从左边沿右滑进 Today View，再补一次滑动 |
| 主屏编辑模式（阴影/sb3） | `rc.jobs.jiggle` | 长按空白处让图标抖动，SpringBoard 随后导出图层树 |
| 应用切换器 | `rc.jobs.switcher` | 起三个 app 后从底边拖起停在中途 |
| 打包方式 | `RC_JOBS=rc.jobs.xxx` 传给 `build.sh`；或直接 `cp` 到 `userland/root/etc/rc.jobs` 后跑一次 `mkapfs.py`，不必全量重编 | |

### 小组件为什么只有白块（2026-10-31 21:30 定位）

`widgets3.png` 里小组件的**容器排好了版**（两个小方格 + 一个大方块 + 一个中块），
但内容全是纯白 —— 内容由 widget extension 进程画，而它没起来。日志里 SpringBoard 的原话：

```
failed lookup: name = com.apple.chronoservices, flags = 0x200, requestor = SpringBoard[7]
```

即 SpringBoard 要 widget 宿主服务，系统里**没有进程注册它**。注册 `com.apple.chronoservices` 的是
`chronod`（`/System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod`，
plist 为 `com.apple.chronod`），而精简根默认的 22 个 job 里没有它。

补法：**已写进 `userland/build.sh` 的默认 `IOS_JOBS`**（`com.apple.SpringBoard com.apple.chronod`），
不必再传 `JOBS=`。手动指定时仍然是：

```bash
JOBS="<默认 22 个> com.apple.chronod" SKIP="kernel loader boot" ./common/build.sh alioth GL=1
```

注意 `build.sh` 会用默认值覆盖 `root/etc/rc` 与 `root/etc/rc.jobs`，
所以自定义场景脚本要在**构建之后**再 `cp` 一次并重跑 `mkapfs.py`。

**结果（2026-10-31 21:39 实测，`runs/chrono.log.txt` / `chrono.png`）：小组件出内容了。**

| 指标 | 补之前（widgets3） | 补之后（chrono） |
|---|---|---|
| `com.apple.chronoservices` 查找失败 | 有（SpringBoard 每次都问） | **0 次** |
| `chronod` | 不存在 | `job state = running`（pid 30） |
| widget 扩展进程 | 0 个 | **7 个**（CalendarWidget / FitnessWidget / gamecenter.widgets / podcasts.widget 等） |
| 界面 | 空白白块 | 日历（THURSDAY 22 / No events today）、天气（蓝色渐变 + No weather data）、Apple News（Top Stories / Trending）都画出来了 |

动态内容仍缺（`No weather data`、`Apple News Is Offline`）是因为没联网，与本机渲染无关。
shell 里那条 `ThrottleInterval type: string` 是 plist 类型告警，不影响启动。

### 2026-10-31 21:55 GPU 端到端验收（当前真实状态）

| 界面元素 | 结果 | 证据 |
|---|---|---|
| 主屏图标 / Search / Dock 材质条 | ✅ | `runs/jiggle-3.png` |
| 小组件：日历、天气、Apple News | ✅ | `chrono.png` |
| 主屏编辑模式（减号徽章、`+` / Done） | ✅ | `jiggle.png` |
| 应用切换器（三张卡片、真实内容、背景模糊、顶部应用条） | ✅ | `switch.png` |
| Settings → General → About（逐行正常，**不再重复两遍**） | ✅ | `runs/switch-4.png` |
| Calendar 引导页（文字/图标/红色按钮） | ✅ | `switch.png` 中卡片 |

稳定性与性能（`runs/switch.log.txt`，230 s 运行）：

```
GL 帧 90 帧，平均 88 draws/帧，纹理累计上传 32.1 MB
平均帧耗时 3.5 ms（最大 22 ms）
panic 0；com.apple.chronoservices 查找失败 0；回落到软件渲染 0 次
```

那些 `Crash` 行是 launchd 的 `_PanicOnCrash` 类型告警和 Apple 自己的
`com.apple.WebKit.WebContent.Crashy` 服务名，不是崩溃。

**GPU 侧到此为止没有已知缺陷**。剩下的都不是 GPU 问题：

| 遗留 | 原因 | 归属 |
|---|---|---|
| 壁纸黑 | wallpaper store 为空：`/private/var` 是 ramfs，PosterBoard 从未跑过，没有人烘焙过壁纸快照；材质因此回落成纯色洋红（`rc.jobs.bake` 就是作者为此做的诊断） | 数据/状态层 |
| 天气/新闻无数据 | 没有网络 | 网络层 |
| Wi-Fi / 蓝牙 N/A | 无线电未起（`wifi=1` 才起） | 驱动层 |
| 可用空间 Zero KB | 数据卷容量小 | 存储层 |

`sb3`（函数 40）不是缺口：`inject.c:337` 已实现 `shadow_blur_source`，但作者实测对照
CoreAnimation 自己的软件渲染器，卡片边缘逐级相同，**本来就没有阴影缺失**，所以它默认不注入
（需要 `glsb3=1`）。`eagl.c` 里"阴影缺失"那段是更早的推测，已被 `inject.c` 的实测结论取代。

### 20:54 试过：给精简根补一个 PaperBoard job（**失败，已回退**）

查证过程：`/System/Library/LaunchDaemons` 是指向 `/redwood/LaunchDaemons` 的符号链接，
即 launchd 配置被集中到 `root.apfs` 由它裁剪；精简根里只有 **26 个** plist（SpringBoard、
backboardd、logd 等），**没有 PaperBoard**。而组件都在：`/Applications/PaperBoard.app`、
`PosterBoard.framework`、`PaperBoardUI.framework`、`GradientPoster.framework`。
完整 iOS 的 491 个 LaunchDaemon 里也没有它 —— 它靠 LaunchServices 按需启动，不靠 daemon。

于是手写了一个最简 `com.apple.PaperBoard.plist` 放进 `userland/root/LaunchDaemons/`，
用 `tools/apfs/mkapfs.py -z --plain fw -o root.apfs redwood=root` 重建 4 MB 精简根
（93 files / 27 plists），替换 `boot/root.apfs` 后上机。

**结果：界面退化。** 主屏只剩 4 个图标、Dock 消失；Settings 界面**每行重复两遍**（严重撕裂）。
PaperBoard 并没有带来壁纸，反而让合成更乱。已用 `root.apfs.bak-2050` 回退。

**这次失败暴露了真正的问题**：壁纸不是独立缺口，**渲染合成本身是错乱的**。
同一轮里 Settings 每一行都画了两次（General/General、Control Center/Control Center…），
这是 framebuffer 双缓冲或 tile 合成的问题，不是缺某个 daemon。

下一步应该查 `MiniIOMobileFramebuffer.cpp` 的 swap 路径与 `redwood-gl` 的提交/回读顺序，
而不是继续往 root.apfs 里塞进程。改动前的 `boot/root.apfs` 备份在 `boot/root.apfs.bak-2050`。

**入口参数**：
- `adreno=22` —— 诊断模式，跑到 step 22 并**画一帧**，然后**把 GPU 停掉**。验证用。
- `gpu=1` —— 正规入口，等价 `adreno=21 adreno_skip=0x1c00 adreno_pause=0 adreno_keep=1`：
  跑到 step 21 并把 GPU **留着**，随后启动 `RedwoodGPU` 服务给 window server 用。
  取不到 4 个固件（`gmu`/`sqe`/`zap.mdt`/`zap.b02`）时明确打印
  "the system draws with its software renderer"。

**step 编号对照**（`steps[]` 数组，共 22 步；任一步失败即 `break`，后续不再尝试）：
17 3D 核上电 → 18 地址空间 → 19 zap → 20 启动 CP → **21 CP 执行命令**（`CP_SET_SECURE_MODE=0` + 写内存）→ **22 画帧**。

---

## 9. 本次（10-31）之前不知道、差点又重写的东西

1. `boot-one.ps1/.sh` 一直在仓库根——已有一键流程、截图、日志归档。
2. `boot/inspect_payload.py` 已能解析 XNUP——我自己又写了一个解析器。
3. `common/build.sh` 对 `check_all_exports` 失败有明确注释（预期行为）——我当成故障排查了半天。
4. `gpu=1` 是渲染的正规入口（不是 `adreno=N`，后者是诊断模式，跑完就停 GPU）。
5. `tools/mesa` 在 WSL 里（仓库快照没有），Metal/GLES 链路早就开了头。
6. `-NoSystem` 可以省掉 98 秒的 2.4 GB 上传——调内核时完全不需要完整系统卷。

## 10. 这次 GPU 的修复落在哪一层（提交前必读）

分三层，而且**只有一层是"改代码"**：

| 层 | 这次做了什么 | 产物落在哪 |
|---|---|---|
| **内核驱动**（`xnu/files/bsd/dev/redwood_adreno.c`） | 10-27 就已写好（`hwcg` 105 条、`CP_PROTECT` 27 条），10-31 只是**第一次真的编译上机**并验证通过 | 源码在仓库；`boot/kernel-*.bin` 是编出来的 |
| **用户态子系统**（`userland/`：Mesa、`eagl.c`、`inject.c`、`inject_sb.c`、job 列表） | 这里才是今天的实质进展：补 `libredwood-gl.dylib`（Mesa 首次构建）、把 `com.apple.chronod` 加进默认 job | 源码在仓库；**真正生效的是 `boot/root.apfs`**（10.4 MB，含 `libOSMesa.8.dylib` / `libredwood-gl.dylib` / `libredwoodair.dylib`）与 `boot/run-payload.bin` |
| 构建开关 | `GL=1`、`DRIVERS=softpipe,freedreno`、`AIR=1` | 已写进文档；`GL=1` 不进源码（是 `build.sh` 的参数） |

**关键事实：界面能不能用 GPU，取决于 `boot/root.apfs` 里有没有那几个 dylib 和 `chronod` 的 plist，
不是取决于源码。** 所以：

- 只提交源码、不提交新的 `boot/root.apfs`，别人拉下来 boot 起来仍是纯 CPU 合成 + 空小组件；
- 只提交 `boot/root.apfs`、不改 `userland/build.sh` 的 job 列表，下次重新构建又会丢掉 `chronod`。

两样都要提交，镜像才是可复现的。`GL=1` 无法记在源码里，所以 `docs` 里必须写清（本文与 README）。
