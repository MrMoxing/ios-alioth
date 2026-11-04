# 照搬参考实现的 Wi-Fi：可行性验证（分支 `wifi-ref-port`）

> 这份文档只回答一件事：**把 Klipboard2022/k40-ios-experiments 的 Wi-Fi 方案整套搬过来跑，可行吗。**
> 结论先给：**可行，但它不是"一个驱动文件"，而是"把 Linux 的 ath11k 编译进 XNU"这一整套。**
> 本分支只做这件事的验证，不合入 `main`；`main` 上仍是我们自己写的 `alioth_pcie.c` 路线。

---

## 1. 参考实现到底是什么

`overlays/hakios/` 那 19 个文件只是"改动"，底下站着三层东西：

| 层 | 是什么 | 在哪 |
|---|---|---|
| 硬件启动 | `hakios.pcie.c`（4,001 行）：电源/时钟/复位、PERST、LTSSM、iATU、BAR、**SMMU 页表**、BHI(SBL)、MHI、BHIE(AMSS)、QRTR/QMI | 参考仓库 overlay |
| 驱动本体 | **Linux 7.0.14 的 `drivers/net/wireless/ath/ath11k`，原样不改** | 构建时下载，`tools/mk-ath11k.sh` |
| 兼容层 | FreeBSD 15.1 的 LinuxKPI 头 + 自写 `wifi/lx/src`（锁、睡、work、内存、固件、总线）+ 内核侧 `hakios.lx.c` | 参考仓库 `wifi/` |
| 接口 | `hakios.apple80211.c`（en0）、`hakios.qrtr.c`、`hakios.radio.c` | 参考仓库 overlay |

也就是说，**"能连上 WPA2 并打开网页"靠的是 Linux 的 ath11k**，参考仓库做的是"让 ath11k 在 XNU 里跑起来"的垫层与硬件启动。
这解释了我们自己那条路线的天花板：`alioth_pcie.c` 手写寄存器序列可以走到链路、BAR、BHI，但后面还有 MHI 通道、QRTR、QMI
（board file/fw 下载/校准）、802.11 状态机、mac80211 语义——不是一个文件能补完的。

## 2. 可行性证据（已核）

1. **补丁在其上游干净应用**：`ufogxl/hackios-alioth @ 569a9cc`（`source-manifest.json` 指定的提交），
   `git apply --check patches/hakios-k40-r45.patch` → 通过。
2. **XNU 版本与我们相同**：`xnu-8796.121.2`（我们的 `~/k40/xnu-src` 就是该 tag 的克隆，可直接给 `xnu/apply.sh` 用
   `XNU_URL=file://` 喂源，不必再下 Apple 的仓库）。
3. **工具链可复用**：参考树的 `xnu-deps/host/bin`（ld64、iig、xnu-clang …）我们 `~/k40/xnu-deps` 里已有，
   硬链接进去即可（不重复下载 Apple 开源件与 SDK）。
4. **缺的只有两份源码**：Linux `7.0.14`（~150 MB，cdn.kernel.org）与 FreeBSD `15.1-RELEASE`（~253 MB）。
   这是唯一的外部依赖，也是校园网下唯一可能卡住的环节。

## 3. 怎么建、怎么上机

```bash
# 1) 树 + 补丁（已做）
git clone https://github.com/ufogxl/hackios-alioth ~/hakios-upstream
cd ~/hakios-upstream && git checkout 569a9cc
git apply .../k40-ios-experiments/patches/hakios-k40-r45.patch

# 2) XNU 源码铺上 HAKIOS 工作（用我们本地的 tag 克隆，避免再下 500 MB）
XNU_URL=file:///home/mrmoxing/k40/xnu-src/xnu-8796.121.2 bash xnu/apply.sh

# 3) ath11k + LinuxKPI → 一个 .o
bash tools/mk-ath11k.sh          # 产物 xnu-deps/build/ath11k/out/ath11k.o，未实现的部分进 stubs.txt

# 4) 内核（xnu-build.sh 会把 ath11k.o 作为 HAKIOS_OBJECTS 链进去）
bash xnu-build.sh
```

上机要带的是**参考实现自己的开机参数**（`hakios_wifi_option` 读的就是它们）：

```
pcie_step=31 pcie_ipcr=2 wifi_lx=2 wifi_station=1
```

`pcie_step=31` 是"BHI(SBL) → READY → M0 → BHIE(AMSS)"那一整套；`pcie_ipcr=2` 是走 QRTR/QMI 而不是只看；
`wifi_lx=2` 是启动 ath11k；`wifi_station=1` 是把选中的 station 接口发布出来给 `wifid`。

## 4. 构建实录（2026-11-04，WSL，逐个坑）

| 步骤 | 结果 | 备注 |
|---|---|---|
| `git apply patches/hakios-k40-r45.patch` @ 569a9cc | ✅ | 无冲突 |
| `xnu/apply.sh`（`XNU_URL=file://…/xnu-8796.121.2`） | ✅ | 193 个文件铺到内核树；省掉 Apple 源再下一遍 |
| 两份源码 | ✅ 157 MB + 253 MB | 官方站 36 KB/s、`download.freebsd.org` 连不上；**aliyun 1.9 MB/s、ustc 1.6 MB/s**，sha256 与脚本里钉的一致 |
| `tools/mk-ath11k.sh` | ✅ **`out/ath11k.o` 764,492 B，`stubs.txt` 0 行** | Linux 的 ath11k 真的编出来了，且没有落到 stub |
| `xnu-build.sh` | ❌ 两次失败，原因见下 | 都是文件行尾，不是代码 |

**两个必须记住的坑（都是 CRLF）**：

1. **仓库是 Windows 检出**：`xnu/apply.sh: line 15: set: -: invalid option`——bash 脚本带 `\r`。
   在 `~/hakios-ref` 里 `grep -rIl $'\r' … | xargs sed -i 's/\r$//'` 解决。
2. **`doconf` 是 csh 脚本**（`#!/usr/bin/env -S csh -f`）。`xnu/apply.sh` 早于去 CRLF 运行，
   于是它铺进内核树的 193 个文件仍带 `\r`，`doconf` 第一行就报
   `set: Variable name must begin with a letter.`——**八个 conf 目录各报一次，看起来像工具链崩了，
   其实是行尾**。修法：把 `xnu-src` 也过一遍 CRLF 清理（或先清理仓库再 `apply.sh`）。

**工具链复用**：`cp -al ~/k40/xnu-deps ~/hakios-ref/xnu-deps`（硬链接，530 MB 不重复占盘），
`tcsh`/`csh` 本来就在那份 deps 的 `host/bin` 里——`doconf` 需要的正是它。

## 5. 风险与未知
| 风险 | 影响 | 备注 |
|---|---|---|
| 两份源码下不下来（400 MB） | 整个方案停在构建 | 只此一处外部依赖；失败可换镜像/离线包 |
| `ath11k.o` 的 stubs | 某些路径只打印名字并返回 0 | 参考实现已用它连上 WPA2，说明关键路径不在 stubs 里 |
| 参考实现的 loader/userland 与我们不同 | 上机脚本要换 | 他们的 `xnuloader`（含 `hakios_image_file` 读 `fw/`）与人家的 `xl` 工具链才是配套的 |
| 系统镜像 | 可能仍可用我们的 `boot/system.apfs` | 同一 iOS 16.5 rootfs；但 `en0` 需要 `wifid` 起来，且 `apple80211` 的接口名要对上 |
| 与我们自己的 `alioth_pcie.c` 冲突 | 两套 PCIe 驱动同时编进内核 | 照搬时应把我们的从内核配置里摘掉 |

## 6. 与我们自己路线的对照（这次照搬最该带走的东西）

即便最后不整包采用，下面四条是我们自己那份 `alioth_pcie.c` 必须补的，且已被参考实现验证过：

1. **BHI 只装 SBL（512 KiB）**，AMSS 随后走 BHIE 的 9 个 512 KiB 向量——p36 把整个 4.3 MB 塞进 BHI，
   endpoint 200 ms 后全 1（PBL 不接受这种镜像）。
2. **SMMU 要自己写**：SID `0x1c01`（不是 `0x1c00`），IOVA `0xa0000000`，`CBAR/CBA2R` + 页表 + context，
   然后才允许设备 DMA；给设备的是 IOVA，不是裸物理地址。
3. **写 BHI 前先复位并等 PBL**：`PCIE_SOC_GLOBAL_RESET`（经 WLAON 窗口 `0x310c` 打开 `0x3008`）→
   `qca_wait_pbl_reset`（`MHICTRL` bit1 清、`MHISTATUS` bit4 清）——"MHI RESET 还没清就写 BHI 描述符"会被丢掉。
4. **MSI 与轮询两种都要**：`BHI_INTVEC=0` 走轮询；`qca_boot_msi` 给的是 MHI 的 MSI 目标。

## 7. 本分支的边界

- 分支 `wifi-ref-port` 只放：本文件、构建脚本（`ubuntu/ref-wifi-build.sh`）、以及照搬过程中的适配提交。
- 不合入 `main`：`main` 上的 `alioth_pcie.c` 路线继续推进，二者不混编。
- 一旦照搬验证通过（能在 Settings → Wi-Fi 里连上 WPA2），再决定是"整包采用"还是"把第 5 节的四条补进我们自己的驱动"。
