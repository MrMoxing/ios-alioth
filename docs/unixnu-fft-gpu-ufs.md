# 从 FFT-infra/UniXNU-alioth 借鉴 GPU 与 UFS 的修复（2026-10-31）

对照对象：<https://github.com/FFT-infra/UniXNU-alioth>（`main`，最后提交 2026-10-30 12:26 UTC）。
它在**同一颗 SoC、同一块板子（alioth / SM8250-AC / Adreno 650）**上把 GPU 从 GMU 冷启动一路做到了
"发布设备 → Metal 加速器 → 桌面出图"，并把 UFS 做到了 root 直读。本文只记录**能落到本项目上的差异**，
不重复 `upstream-lessons-unixnu.md`（那是 2026-10-22 对旧组织 UniXNU/UniXNU 的 4 KiB 页分析，仍然有效）。

---

## 0. 一句话结论

> **2026-10-31 真机验证：step 20 已通过。** `CP_SQE_CNTL=1` 写入返回、CP 接受 initialize 命令、
> 存活 10 秒以上不复位，系统继续启动到 `SpringBoard`。证据见 §6 与
> `docs/evidence/2026-10-31-gpu-step20-passed.log`、`2026-10-31-springboard.png`。

**解药是 2026-10-27 就已写进构建树的那两件事**：`a6xx_hwcg` 的 105 条时钟门控表（含先关/后开
SP 时钟的时序）和 `CP_PROTECT` 的 27 条保护表。那一版此前**从未编译上机**（`out/` 里的产物停在
10-23），10-31 第一次跑就通过了。

本文第 1 节以下的逐项对照仍然成立，但要更正一处判断：早先看到 Windows 侧仓库（10-23 基线）时
以为"缺 `CP_PROTECT` / `hwcg` / biweight 三块"，实际构建树（10-27）**已经有了**，那次补充属于
重复劳动。仓库现已同步为构建树的版本（`235794` 字节，即真机验证通过的那份）。


---

## 1. step 20 的逐项对照

UniXNU 的 CP 启动序列在 `package/GpuProbe.cpp:871 cpStart()`，基线在 `:728 hwInit()`。
本项目在 `redwood_adreno.c:step_core_start()`。逐条比：

| Linux `a6xx_gpu.c` 的写 | 本项目 | UniXNU | 结论 |
|---|---|---|---|
| `GBIF_HALT=0` / `RBBM_GBIF_HALT=0` | ✅ 已写 | ✅ | 一致 |
| `RBBM_SECVID_TSB_CNTL` + trusted base/size | ✅ 已写 | ✅ | 一致 |
| `*_ADDR_MODE_CNTL = 1`（64 位） | ✅ 12 条 | ✅ 12 条 | 一致 |
| **`a6xx_set_hwcg()`** | ❌ **缺** | ✅ 49 条 + `RBBM_CLOCK_CNTL=0x8aa8aa82` | **已补** |
| `GBIF_QSB_SIDE0..3 = 0x00071620` | ✅ 已写 | ✅ | 一致 |
| `UCHE_WRITE_RANGE_MAX/TRAP_BASE/WRITE_THRU_BASE` | ✅ 已写 | ✅ | 一致 |
| `CP_ROQ_THRESHOLDS_1/2`、`CP_MEM_POOL_SIZE=128` | ✅ 已写 | ✅ | 一致 |
| `PC_DBG_ECO_CNTL`、`CP_AHB_CNTL=1` | ✅ 已写 | ✅ | 一致 |
| UBWC 的 `*_NC_MODE_CNTL`（hbb 16） | ✅ 已写 | ✅ | 一致 |
| `RBBM_INTERFACE_HANG_INT_CNTL=(1<<30)\|0x1fffff` | ✅ 已写 | ✅ | 一致 |
| `UCHE_CLIENT_PF` | ✅ 已写 | ✅ | 一致 |
| **`a650_protect`（39 条 + `CP_PROTECT_CNTL`）** | ❌ **缺** | ✅ | **已补** |
| **`TPL1_BICUBIC_WEIGHTS_0..4`** | ❌ 原注释"故意留 reset 值" | ✅ 有确切值 | **已补** |
| `CP_APRIV_CNTL = bits 6,5,3,2,1` | ✅ 已写 | ✅ | 一致 |
| GMU 计数器（`GPU_GMU_AO_GPU_CX_BUSY_MASK` 等） | ❌ 缺 | ✅ | 未补（只影响忙闲统计，不影响启动） |
| `RBBM_INT_0_MASK` | 部分 | ✅ | 待确认 |
| SQE 去掉第一个 dword | ✅ `microcode+4, size-4` | ✅ `size-4` | 一致 |

### 为什么这三块可能与"写 `CP_SQE_CNTL=1` 即复位"有关

按可能性排序，都不是结论，是**下一步要一次验证一个的假设**：

1. **`CP_PROTECT_CNTL` 没有按 a650 的表初始化。** 该寄存器 bit0 是"使能保护"、bit1 是
   "越权即 fault"。如果 bootloader 或上一次内核把它留成使能状态，而 48 条 span 表是别的 SoC 的
   （或全是 0），CP 一启动去读自己的 SQE / ring 就可能被判越权，结果是 CP 挂死而不是报错。
   Linux 与 UniXNU 都在启动 CP **之前**把 39 条 span 全部写一遍，然后才置 `CP_PROTECT_CNTL`。
2. **`hwcg` 没写，`GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL` 的 bit0 状态未知。**
   `a6xx_set_hwcg` 的顺序是"**先关 SP 时钟 → 写 49 条表 → 再开 SP 时钟**"。
   本项目从未读过这个 GMU 寄存器；如果 SP 时钟在此时是关的，CP 启动后访问 SP 相关寄存器会挂。
   这也解释了记录里"SPTPRAC 控制寄存器写不进"——写的是一个需要时钟在跑的块。
3. **`RBBM_INTERFACE_HANG_INT_CNTL` 已写但值得复核**：它决定了 AHB 接口多久无响应算 hang、
   以及 hang 之后是报中断还是直接复位。`reason1 0x2` 的整机复位与 hang 检测的行为一致。

### 已验证一致、不必再查的（本项目记录里已排除的项得到的印证）

| 本项目已排除 | UniXNU 侧的印证 |
|---|---|
| 寄存器偏移 | 同为 `a6xx_reg.h` 的 dword 偏移，逐条一致 |
| SQE 微码地址 | 同样 `+4` 跳过首 dword |
| SMMU 拒绝取指 | UniXNU 也把 SMMU 放在 CP 之前，且 S2CR/SMR 通 |
| STALL 模式 | 同为 `HUPCF`（bit8），`CFCFG=0` |
| PTE 权限 | 同为 `AP_UNPRIV` |
| zap 顺序 | UniXNU 同样在 CP 之后做 `CP_SET_SECURE_MODE` |

### SMMU 那条要单独对一次

本项目 step 18 的结论是"K40 可用 SMR/S2CR 表项有限，0/4/5 号写不进，核心用 2 号"，
并用自适应选表项绕开了。UniXNU 的做法不同，但**结果等价且更省事**：

> 该部件的空闲 slot 读回 `0x000203ff`（FAULT 类型但低字节残留 `CBNDX=0xff`），
> 而 `ArmSmmu4K` 的判空要求整字等于 `0x00020000`，于是所有 slot 都被判 busy。
> 修法是 attach 前把要用的 slot **重写为规范 FAULT 字 `0x00020000`**，且只改类型已是 FAULT 的 slot。
> **绝不能写 0**——0 是 TRANSLATE 类型，slot 会从"空闲"变成"已翻译"，attach 依旧失败。

这条与本项目"0/4/5 号写不进"是同一个现象的两个说法：**写不进 ≠ 硬件没有这个表项**，
而是写的值不对、或该表项处于一种读回值不匹配的状态。如果以后要回到标准表项分配，
按 FAULT 字归一化重试一次，比继续自适应绕开更接近 Linux 的行为。

---

## 2. 曾经做过的改动（已作废）

> **作废说明**：下面这些改动是基于 Windows 仓库里 10-23 的旧副本做的。构建树当时已经是 10-27 版，
> 这些内容**早已存在**，而且用的是更全的**厂商 105 条** hwcg 表（不是 Linux 的 49 条）。
> 仓库现已被构建树版本覆盖（235794 字节，即真机验证通过的那份）。以下仅作历史记录。

| 位置 | 改动 |
|---|---|
| 宏定义区（`CP_APRIV_CNTL` 后） | 新增 `CP_PROTECT_CNTL`、`CP_PROTECT(n)`、`TPL1_BICUBIC_WEIGHTS_0..4` |
| GMU 宏区 | 新增 `GMU_GX_SPTPRAC_CLOCK_CONTROL`、`GMU_GX_SPTPRAC_POWER_CONTROL`、`GMU_AO_GPU_CX_BUSY_MASK`、`GMU_POWER_COUNTER_SELECT_0/ENABLE` |
| step 20 之前 | 新增 `hwcg_table[49]` + `set_hwcg()`；新增 `protect_table[39]` + `set_cp_protect()` |
| `step_core_start()` 中 `address_modes` 之后 | 调用 `set_hwcg()` |
| `cp_chicken_dbg` 之前 | 调用 `set_cp_protect()` |
| 原"双三次权重留 reset 值"注释处 | 改为实际写入 5 个寄存器 |

### 真正起作用的那一版（10-27，构建树）

| 项 | 实际实现 |
|---|---|
| 时钟门控 | `a6xx_hwcg` **105 条**（厂商表），先 `GMU_GX_SPTPRAC_CLOCK_CONTROL` 清 bit0 → 写表 → 置 bit0，最后 `RBBM_CLOCK_CNTL = 0x8aa8aa02` |
| CP 保护 | `a6xx_protect` **27 条**，`CP_PROTECT_CNTL = 3` |
| 开关 | `adreno_hwcg`、`adreno_protect`、`adreno_vbif`、`adreno_tsb`、`adreno_sqe_halt`、`adreno_trust`、`adreno_cp`、`adreno_verify` |

**与 UniXNU 仍存在的差异**（都跑通了，可自行选择是否对齐，非阻塞）：

| 项 | 本项目 | UniXNU |
|---|---|---|
| `RBBM_CLOCK_CNTL` | `0x8aa8aa02` | `0x8aa8aa82`（bit7） |
| `CP_PROTECT_CNTL` | `3` | `0xB`（多 bit3） |
| hwcg 条目 | 厂商 105 条 | Linux 49 条 |
| protect 条目 | 27 条 | 39 条 |

**两份源码**（见 `SESSION-HANDOFF.md` §7）：overlay 的 `xnu/files/` 与构建树的
`xnu-src/xnu-8796.121.2/bsd/`。已把构建树版本同步到仓库，overlay 与构建树现在一致。

---

## 3. 真机上怎么验证（一次只动一个变量）

顺序按"代价从低到高"，每轮只改一个参数：

| 轮 | 参数 | 看什么 | 通过 |
|---|---|---|---|
| 1 | `adreno=20 adreno_hwcg=0` | 是否还在写 `cp_sqe_cntl` 时复位 | 复位消失 → `hwcg` 表有问题；仍复位 → 与 hwcg 无关，看第 2 轮 |
| 2 | `adreno=20`（默认，表已写） | `clock gating written: rbbm_clock_cntl 0x0 -> 0x8aa8aa82` 是否打印；`cp_protect_cntl 0xb` 读回 | 两行都对 → 基线完整 |
| 3 | 同上 | 写 `cp_sqe_cntl` 后是否出现 `the write ... came back` | 出现 → 挂死点前移，看 `CP_HW_FAULT` / `CP_PROTECT_STATUS` |
| 4 | 同上 + `adreno_oob=1` | `it answers 0x80000000`（bit31） | OOB 握手成功 |

第 3 轮一旦通过，紧接着要看的是 `cp_protect_status`（`0x0824`）：非 0 说明 CP 撞上了
我们刚写的某条 span，把那条 span 的 `reg/len` 打出来就知道是哪一条。

---

## 4. UFS：本项目是 md0，UniXNU 是 UFS root

两者不是同一个问题域，但 UniXNU 有三条**可直接用**的结论：

### 4.1 `powergate-latency` 缺失 → 所有短延迟变成 830 µs

UniXNU 实测（`docs/evidence/2026-10-27-ufs-time.md`）：`IODelay(1)` 走 `delay_for_interval`，
后者只在 `ml_delay_should_spin` 认为"间隔小于 CPU 空闲延迟"时才自旋。这个延迟来自 ADT 的
`powergate-latency` 属性，**该板设备树没带，于是它恒为 0**，结果每次 `IODelay` 都阻塞到下一个
tick（约 830 µs）。同一批 UFS 命令在阶段早期平均 1990 µs/条、后期 331 µs/条，差的 1659 µs
就是两个被阻塞的 tick。

**对本项目的意义**：`redwood_adreno.c:345/351`、`redwood_adsp.c:187/193`、`alioth_pcie.c:60`
都用 `delay_for_interval` 做微秒级等待。凡是在轮询循环里等 1–100 µs 的，实际每次至少 830 µs。
修法二选一：

1. 在设备树/ADT 里补 `powergate-latency`；
2. 对短等待改成**有界自旋**（UniXNU 的 boot-usb42 就是"先在 doorbell 上自旋一个有界窗口"）。

第 2 种不依赖 ADT，改动局部，建议先做。

### 4.2 Data 卷不能靠 `/dev/md0`

UniXNU 记录（`porting.toml` storage 区，2026-10-26 实测）：
Data seed 经 `/dev/md0` 挂载失败（`ENOENT`，panic 屏 `offline scratch Data mount failed: 2`），
改成**用 RamBlock 把 seed 发布成一个真正的 IOMedia** 才挂上。

如果本项目以后要让 iOS 的私有目录可写（现在的 root 是 APFS 镜像在 RAM，本质上只读），
这条是前提：**镜像要作为块设备发布，而不是当裸内存盘直接挂**。

### 4.3 分区几何（同一台机器，可直接引用）

```
userdata: first LBA 0x2a8000, last LBA 0x3b037fa, 4096-byte blocks
```

以及 `pil_gpu_region@8681a000`（size `0x2000`，`no-map`）与 GPU 中断 `SPI 300, level high`，
与本项目已核实的值一致——**这两个值不要改**，且 `0x8681a000` 归 TrustZone 管，
CPU 直接写它必挂；本项目现在的"用内核自有连续两页"是对的。

---

## 5. 参考位置（便于回查）

| 内容 | 路径 |
|---|---|
| CP 启动全序列 | `package/GpuProbe.cpp:871 cpStart()` |
| a650 hw_init（含 protect/hwcg/UBWC） | `package/GpuProbe.cpp:728 hwInit()` / `:710 setHwcg()` |
| 39 条 protect 表原文 | `package/GpuProbe.cpp:672 kProtect[]` |
| 49 条 hwcg 表原文 | `package/GpuProbe.cpp:689 kHwcg[][2]` |
| GX 上电（HFI 投票 + OOB bit30/31） | `kits/pipa/A650Gmu.c:334 A650GmuPowerGx()` |
| SMMU slot 归一化 | `docs/evidence/2026-10-29-gpu-smmu-slot.md` |
| GPU 全阶段读数 | `docs/evidence/2026-10-29-gpu-gx-and-context.md` |
| UFS 时序三段论 | `docs/evidence/2026-10-27-ufs-time.md` |
| zap 镜像 13964 B 的布局 | `porting.toml` gpu 区 / `package/GpuFirmwareA650.c` |

---

## 6. 实测结果（2026-10-31，真机）

### 构建

| 项 | 值 |
|---|---|
| 内核 | `mach.development.bcm2837`，15179888 字节，`Sat Oct 31 19:25:01 CST 2026` |
| 确认含改动 | 二进制里 `adreno_hwcg` / `cp_protect_cntl` / `adreno_trust` / `adreno_sqe_halt` / `protection entries are written` 各命中 1 次；`prebuilt/` 那个 10-22 的旧内核全为 0 |
| 关于 `check_all_exports` | **失败是预期的**：`common/build.sh:27` 注明"the export check fails after the kernel is linked, on purpose"，以 mach 文件为准。首轮误判为故障，清 config 目录重试后确认与代码无关 |

### 上机（`fastboot boot` 纯 RAM，未写任何分区）

```
adb reboot bootloader → fastboot boot alioth-boot.img → getvar product = xnuloader
oem ramdisk → stage system.apfs (2.32 GiB, 98.3 s) → stage run-payload.bin (18.9 MB) → oem run
```

cmdline：`... touchscreen=1 buttons=1 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=20 adreno_watch=10`

### step 20：通过

| 读数 | 结果 |
|---|---|
| 时钟门控 | `the 105 hardware clock-gating registers are written` |
| CP 保护 | `the command processor's 27 protection entries are written` |
| 启动寄存器回读 | **`11 of 11 start registers kept their value (mask 0x7ff)`** |
| `CP_SQE_CNTL=1` | **`the write to cp_sqe_cntl came back`**（此前写下去内核就再无输出） |
| CP 状态 | `rbbm_status 0x1`、`cp_hw_fault 0`、`cp_protect_status 0`、`cp_interrupt_status 0` |
| initialize 命令 | `the command processor TOOK the initialize command: rptr 9 (want 9)` |
| 存活 | `alive 1 s` … `alive 10 s after step 20`，**不再复位** |

### step 21 / 22：同日复测也已通过

同日 19:43 用 `boot-one.ps1 -NoSystem -Payload <adreno=22 的载荷>` 再跑一轮（20 秒内）：

| 步骤 | 打印 | 结论 |
|---|---|---|
| step 21 | `THE 3D CORE WRITES OUR MEMORY ON COMMAND` → `adreno: step 21: done` | CP 出安全模式并真的按命令写了我们的内存 |
| step 22 | `A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top` → `adreno: step 22: done` | **GPU 用 2D blit 引擎画了一帧并上屏** |
| 存活 | `alive 1 s … 9 s after step 22` | 稳定 |

像素判据（内核自己读回）：中间 `ff ff ff ff`（白）、末行 `ff 80 00 ff`（蓝），与
`ring_fill(0,800,255,128,0) / (800,1600,255,255,255) / (1600,2400,0,128,255)` 一致。

截图为橙/白/蓝三条带，与仓库里上游的 `docs/gpu-first-frame.png` 是同一张判据图。

证据：`docs/evidence/2026-10-31-gpu-step22-passed.log`、`2026-10-31-gpu-frame-step22.png`。

> 注意：`adreno=22` 跑完会把 GPU 停掉。要让 window server 用 GPU 画，得用 `gpu=1`
> （= `adreno=21 … adreno_keep=1`），它会保持 GPU 并启动 `RedwoodGPU` 服务。

### 之后

- GPU 正常收尾：`slumber` → `TrustZone has given it up`（zap）→ SMMU 关闭 → `over; the system goes on starting`
- `Darwin 22.5.0 iPhone10,3`，`memory 6144 MB, page size 16384, 7 cpu`
- `launchd.ios` → `backboardd` → **`SpringBoard[7]`**，`Spotlight[40]`
- 触摸有事件（`contact 1 presence: touching / none`）
- 截图：主屏正常，时间 4:51、电量 53%，图标与 Dock 完整
- 收尾 `fastboot reboot` → Android `alioth` 正常启动

### 结论

**`CP_SQE_CNTL=1` 的复位已消除**。此前把它归因为"启动动作本身致命"，实际是启动前的基线不完整
所致：CP 在保护表与时钟门控都没按 a650 配置好的状态下被放行，硬件随即把整机带下去。

下一步的自然延伸（本次未做）：
1. `adreno=22`（让它继续走 step 21/22：出安全模式 + 写内存），确认 CP 能真正执行命令；
2. 若要 GPU 出图，还需要 UniXNU 那条完整链路（NativeAdreno + Turnip + AdrenoPM4），
   本项目现用的寄存器级 `redwood_adreno.c` 只到"命令处理器可用"。

