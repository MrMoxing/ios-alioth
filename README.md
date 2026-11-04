# HackiOS on Redmi K40 (alioth) — 启动全流程交付包

把 Apple 开源的 **XNU 内核（iOS 16.5 / `xnu-8796.121.2`）** 跑在 **Redmi K40 / POCO F3（alioth，骁龙 870 / SM8250）** 上，
上面跑**未修改的 iOS 16.5 用户态**。启动链：

```
fastboot boot → Mu-Silicium UEFI → xnuloader (EFI 应用) → XNU → launchd → backboardd → SpringBoard
```

**全程只写 RAM**（`fastboot boot` / `fastboot stage`），不刷任何分区；重启即回到原来的 Android / HyperOS。
唯一的持久化写操作是 loader 经手机 UFS 的 `darwin` 分区保存数据卷（`var_store`，双槽 2×2 GiB）与
崩溃时写小米的 `logdump` 分区；两者都不碰系统分区，重启回 Android 不受影响。

- 打包日期：2026-10-31（GPU 用户态渲染通路打通，见 §1 与 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)）
- 真机验证的目标：Redmi K40（alioth），MIUI/HyperOS 原系统保留
- 详细技术记录见 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)（文件地图、GPU 三条渲染路径、构建配方与坑）、
  [docs/SESSION-HANDOFF.md](docs/SESSION-HANDOFF.md)（参数、根因、踩过的坑、GPU/Wi-Fi 进展）

---

## 1. 包内容能做什么

**已在真机达成**

| 能力 | 证据 |
|---|---|
| UEFI + loader 自启，USB fastboot 通道常驻 | `fastboot getvar product` → `xnuloader` |
| XNU 启动并挂载 APFS 系统卷 | `apfs: volume 0 "System" … 45143 files`、`BSD root: md0` |
| iOS 用户态完整起来 | `launchd.ios` → `runningboardd` → `backboardd` → `SpringBoard`(pid 7) |
| **iOS 16.5 主屏显示** | `docs/screenshots/01-springboard.png` |
| **GPU 用户态渲染**（CoreAnimation 走 Adreno） | `docs/evidence/2026-10-31-gpu-switcher.png`；日志 `the server has an accelerated renderer`、`OpenGL ES 3.2 Mesa 25.0.7; FD650` |
| **小组件出内容**（日历 / 天气 / Apple News） | `docs/evidence/2026-10-31-gpu-widgets-content.png`（此前空白白块） |
| Dock 半透明材质 / 壁纸模糊 / 主屏编辑模式 | `docs/evidence/2026-10-31-gpu-edit-mode.png` |
| 应用切换器（多卡片、真实内容） | `docs/evidence/2026-10-31-gpu-switcher.png` |
| **真实手指触摸**（FT 0x5652） | `docs/screenshots/r6.png`（手指点开「设置」） |
| 电源键 / 音量键、真实电量 | 见 `docs/SESSION-HANDOFF.md` §5.3–§5.5 |
| EPSS 三簇调频 1804 / 2419 / 3187 MHz | `docs/evidence/2026-10-22-first-boot-kernel.log` |
| 数据卷跨启动持久化（`var_store`，2×2 GiB） | `com.redwood.varstore` job；`kern.redwood.ufs_write_bytes` |

**仍未达成（勿期待）**：Wi-Fi 的**上网**（链路、扫描、WPA2 关联已通，还差 DHCP 拿地址，见 §6 表）、
真实音频（`mediaserverd` 未启动）、相机、蜂窝/蓝牙；
**真熄屏**（电源键目前是画黑帧，面板仍通电——AMOLED 需 DSI 命令，见 §6）；系统默认壁纸
（iOS 16 的壁纸由 PosterBoard 程序化渲染，见 §6）；系统语言（默认仍英文，
中文种子在开发分支尚未生效）；7 个核里只有 4 个 online。

---

## 2. 目录结构

```
ios-alioth/
├── boot-one.bat                一键启动（Windows 入口，双击或命令行；不需要 Git Bash/Python）
├── boot-one.ps1                boot-one.bat 的实现（.NET 截图转换、超时控制）
├── boot-one.sh                 一键启动（Git Bash；按设备状态续跑：Android / bootloader / xnuloader）
├── fetch-firmware.bat          固件获取（Windows 入口；独立工具，boot-one 不调用）
├── fetch-firmware.ps1          fetch-firmware.bat 的实现（断点续传 + 大小/MD5）
├── fetch-firmware.sh           固件获取（Git Bash；断点续传 + 大小/MD5）
├── ubuntu/                     Linux 构建入口：sync.sh 组装工作树、build.sh 一键构建、README
├── runs/                       运行产物（首次运行自动创建）：runHHMMSS.png / .log.txt
├── firmware/                   本地固件目录（**不入库**）：iOS 16.5 IPSW，仅重建系统卷时需要（启动流程不下载）
│   ├── iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
│   ├── iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw.md5   基准 MD5
│   └── .verified               校验通过标记（脚本快速路径）
├── boot/                       可直接启动的全部镜像（不需要编译）
│   ├── alioth-boot.img         Android boot 镜像：自编 Mu-Silicium UEFI（fastboot boot 用）
│   ├── uefi-alioth.fd          同一份 UEFI 的裸 FD（3 MB，调试/替换用）
│   ├── run-payload.bin         XNUP 载荷：内核 + root.apfs + 完整 cmdline（fastboot stage 用）
│   ├── system.apfs             iOS 16.5 系统卷（2.49 GB，**不入库**：GitHub 单文件 100 MB 限制；本地按 §5.4 构建）
│   ├── root.apfs               启动卷 10.4 MB：init / 注入库 / **GL 库（libOSMesa + libredwood-gl + libredwoodair）** / GPU 固件 / launchd jobs（含 chronod）
│   ├── kernel-ios.bin          当前载荷里那份内核（与 run-payload.bin 内嵌的一致，重建载荷用）
│   ├── kernel-latest.bin       2026-10-31 源码构建的内核（含 GPU step 20 修复，即真机验证通过的那份）
│   └── inspect_payload.py      打印载荷里的 cmdline 与各段长度/MD5
├── src/
│   ├── hackios-xnu-kernel-main/   内核工程（上游 fork + alioth 移植改动、xnuloader、UEFI、Wi-Fi 兼容层）
│   ├── hackios-xnu-userland-main/ iOS 用户态（init、注入库、APFS 镜像构建、ios165 提取脚本）
│   ├── mu-silicium-config/        Mu-Silicium 的 alioth 设备配置（alioth.toml / DeviceBuild.py / build_uefi.py / alioth.dts）
│   └── tools/                     fdt_dump.py（纯 Python FDT 解析）
├── docs/
│   ├── PROJECT-MAP.md           项目地图：每个文件的用途、GPU 三条渲染路径、构建配方与坑（先看这个）
│   ├── PROJECT-README.md        工作区原始说明
│   ├── SESSION-HANDOFF.md       完整交接文档：参数、根因、排除记录、GPU/Wi-Fi 进展
│   ├── unixnu-fft-gpu-ufs.md    与 FFT-infra/UniXNU-alioth 的修复对照
│   ├── evidence/                真机原始日志与截图（2026-10-22 起，含 GPU 打通的逐轮证据）
│   ├── screenshots/             主屏 / 锁屏 / 注入点击 / 真实触摸
│   └── hardware/alioth.dts      从真机导出的设备树（1.5 MB）
├── tools/                       symbolize.py（panic 地址反查）、pngcrop.py（帧缓冲裁剪）、dump-gpu.sh、pci-topology.sh
└── SHA256SUMS.txt               关键镜像校验和
```

---

## 3. 直接启动（不用编译）

**前置条件**

1. Redmi K40 / POCO F3（alioth），**bootloader 已解锁**（否则 `fastboot boot` 会拒绝）。
2. Windows + [platform-tools](https://developer.android.com/studio/releases/platform-tools)，`adb` / `fastboot` 可用。
   脚本会自动探测 PATH、`%LOCALAPPDATA%\Android\Sdk\platform-tools`、`C:\Android\Sdk\platform-tools`、`D:\Android\Sdk\platform-tools`。
3. 手机开着 USB 调试，`adb devices` 能看到且只有一台设备。
4. 任选一种启动方式：
   - **Windows：`boot-one.bat`**（推荐，双击或命令行均可）：不需要 Git Bash，也不需要 Python；
     `.bat` 自动用 `-ExecutionPolicy Bypass` 调用同目录的 `boot-one.ps1`。
   - **Git Bash：`boot-one.sh`**：需要 Python 3 把帧缓冲转 PNG。
   WSL 里的 adb/fastboot 看不到 USB 设备，不要用。

**一条命令（Windows，包根目录；也可直接双击 boot-one.bat）**

```bat
cd <交付包路径>\open-source
boot-one.bat
:: 带参数：
boot-one.bat -Secs 60 -Shots 3 -Gap 5
```

**一条命令（Git Bash，包根目录）**

```bash
cd <交付包路径>/open-source
SECS=45 SHOTS=3 GAP=5 bash boot-one.sh
```

`.bat` 与 `.sh` 流程完全一致：**先探测设备当前状态，从所处状态继续**——
在 Android（adb）→ 重启进 bootloader；已在 bootloader → 直接 boot；
loader 已在跑（`product: xnuloader`）→ 跳过 boot 直接续跑后续步骤。
之后：`fastboot oem ramdisk` → `fastboot stage boot\system.apfs`（2.49 GB，约 1.5 分钟）→
`fastboot stage boot\run-payload.bin` → `fastboot oem run` → 等 `Secs` 秒 → 连拍截图（间隔 `Gap`）→
取日志 → **留在 iOS**（不回安卓）。中途断了不用重启到安卓：脚本会从当前状态接着跑。

**产物**：`runs\runHHMMSS.png` / `runs\runHHMMSS.log.txt`。主屏约在内核启动后 20 秒出现；
`Secs=45` 能稳定拍到主屏。

**常用开关**

| PowerShell 参数 | Bash 变量 | 默认 | 作用 |
|---|---|---|---|
| `-Secs 45` | `SECS=45` | 45 | `oem run` 之后等多久再截图 |
| `-Shots 3` | `SHOTS=3` | 3 | 截图张数 |
| `-Gap 5` | `GAP=5` | 5 | 两次截图间隔秒 |
| `-Reboot` | `REBOOT=1` | 关 | 跑完回到 Android；默认留在 iOS（`loader_return_after` 到点后才回 loader） |
| `-NoSystem` | `SYS=`（空） | 关 | 不 stage 2.5 GB 系统卷，只跑内核（载荷自带 root.apfs 当根） |
| `-Serial <sn>` | `SERIAL=<sn>` | 自动 | 多设备时指定序列号 |
| `-Payload x.bin` | `PAYLOAD=x.bin` | `boot\run-payload.bin` | 换载荷 |
| `-Image x.img` | `IMG=x.img` | `boot\alioth-boot.img` | 换 loader 镜像 |
| — | `FB_T=600` | 30 | 每条 fastboot 命令的超时秒数；**stage 系统卷必须 `FB_T=600`** |

**Git Bash 两个必踩的坑**：路径必须写 Windows 形式（`C:/...`，脚本已设 `MSYS_NO_PATHCONV=1`，
fastboot 是 Windows 程序不认 `/c/...`）；`FB_T` 默认 30 秒，传系统卷（97 秒）必须放大。

### 3.1 固件目录与 MD5

`firmware/` 放的是 **iOS 16.5 20F66（iPhone10,3 / iPhone10,6）IPSW**（6.16 GB）。
它是一键启动**不需要**的——启动读的是 `boot/system.apfs`；它只在你想从零重建系统卷时才用到。
**`boot-one.*` 完全不碰固件**：不下载、不校验、也不调用 `fetch-firmware.*`。

> **GitHub 仓库不包含 firmware/ 和 boot/system.apfs**（体积 + 版权限制）。克隆仓库后：
> 只启动、体验现有镜像 → 什么都不用做（`-NoSystem` 用载荷自带 root.apfs，或自备 system.apfs）；
> 要完整桌面 → 按 §5.4 用本机固件构建 `system.apfs`（这一步才需要 IPSW）。

- 需要 IPSW 时**手动**运行独立工具 `fetch-firmware.*`：
  `curl` 断点续传（`.part`）、最多 3 次重试，下载完按 **大小 6,165,698,198 + MD5**
  校验，通过后才改名并写 `.verified`；任一步失败都**不会**拿坏文件去构建。
- 基准值：`MD5 f87b4bd6f3c86fb174ce017b5a5ca97b`，同目录 `*.ipsw.md5` 可直接 `md5sum -c`。
- 手动获取：

```bat
fetch-firmware.bat              :: 缺失则下载，存在则校验
fetch-firmware.bat -CheckOnly   :: 只校验
fetch-firmware.bat -Force       :: 重新下载
```

```bash
bash fetch-firmware.sh
CHECK_ONLY=1 bash fetch-firmware.sh
FORCE=1 bash fetch-firmware.sh
```

- 校验通过后写入 `firmware\.verified`，后续启动直接命中标记、不再重复算 6 GB 的 MD5；
  删掉该标记即可强制重新校验。
- 固件目录可用 `-Dir` / `DIR=` 改到别处。工具失败只打印错误、不留半成品，且**与启动无关**。
- 重建系统卷（需要 Linux + 7z + APFS 工具链）：

```bash
cd src/hackios-xnu-userland-main
LOCAL_IPSW=<交付包>/firmware/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw ios165/fetch.sh
REST=1 userland/build.sh
```

**排障**

| 现象 | 原因 / 处理 |
|---|---|
| `< waiting for any device >` | 手机没进 fastboot / bootloader 未解锁；脚本对每条 fastboot 都有 30 s 超时 |
| `stage` 报 `bad size` | 少了 `fastboot oem ramdisk`（脚本已自动做） |
| loader 起不来 | `fastboot boot` 报错说明 bootloader 拒绝（未解锁/签名），与镜像无关 |
| 截图全黑 | `SECS` 太短，主屏还没出来；用 `SECS=45` |
| 想看内核日志 | `fastboot oem log` 只有 loader 的；内核日志在 `logdump` 分区，回 Android 后 `adb shell "su 0 dd if=/dev/block/by-name/logdump ..."` |

---

## 4. 载荷里的启动参数（cmdline）

`run-payload.bin` 当前的完整 cmdline（`inspect_payload.py` 可复核）：

```
loader_return_after=240 epoch=1792659000 sb_delay=8 touchscreen=1 buttons=1 touch_stat=5
cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0
adreno_keep=1 adreno_watch=10
```

| 参数 | 作用 |
|---|---|
| `touchscreen=1` | FocalTech 0x5652 触摸驱动（真实手指） |
| `buttons=1` | PM8150 PON 电源键 / 音量键 |
| `cpufreq=1` | EPSS 三簇调频 |
| `bootcpu=7 cpus=8` | 启动核与核数 |
| `loader_return_after=N` | N 秒后内核把控制权交回 loader（日志/截图窗口） |
| `touch_stat=N` | 每 N 秒一行触摸统计 |
| `sb_delay` / `epoch` | SpringBoard 启动延时 / 时间戳基准 |
| `adreno_soc=a650` | 选 K40 的 GPU 行（不能省：缺了回退 a660 去找不存在的固件） |
| `adreno=21 adreno_skip=0 adreno_keep=1` | GPU 上电到 step 21 并**常驻**，把 Adreno 交给窗口服务器渲染（§6） |
| `gpu_log=1` | 每帧打印 GL 统计（调 GPU 时才开） |

**不要用 `gpu=1`**：它等价于 `adreno=21 adreno_skip=0x1c00 …`，会跳过 step 11（TrustZone）和
step 12（zap 签名），导致 step 19 的 `zap_unload` SMC 挂死——这是当前内核上的已知 bug。
排查 GPU 时可加 `gpu_log=1`；`adreno=22` 会让 GPU 画一张三色测试帧。

| 其他 | 作用 |
|---|---|
| `touch_demo=1..6` | 注入点击/滑动/唤醒解锁（1 定点、2 竖扫、3 上滑、5 唤醒+解锁+点击、6 唤醒+解锁） |
| `pcie=N` | QCA6390 PCIe0 分步调试（1..10），默认不带 |

**不要带**：`tlmm_scan=1`（扫描 TLMM 会打挂系统）、`demo_lock`、`-v`（内核日志会叠在 iOS 界面上）。

---

## 5. 从源码重建（内核与系统镜像需要 Linux）

### 5.0 哪些步骤必须 Linux

| 产物 | 环境 | 为什么 |
|---|---|---|
| 内核 `kernel-*.bin` | Linux x86-64（WSL2 Ubuntu 或原生 Linux） | XNU 的 makefile、clang/lld、theos 的 ld64 |
| `system.apfs` / `root.apfs` | Linux | `ios165/fetch.sh`（7-Zip ≥23 读 APFS）、`mkapfs.py`、theos 交叉编译（无工具链时用 `prebuilt/userland`） |
| `uefi-alioth.fd` / `alioth-boot.img` | Linux | Mu-Silicium（EDK2/stuart） |
| `run-payload.bin` | Windows Python（推荐） | 只是打包；但 cmdline 必须完整，不能经 `wsl -lc` 传（会被截断） |
| 启动、截图、取日志 | Windows | `boot-one.bat`，不需要编译 |

结论：**只启动、不改代码 → 完全不需要 Linux**；改内核 / 用户态 / UEFI 才需要下面这些。

### 5.1 环境（官方仓库与文档）

| 需要 | 地址 / 命令 |
|---|---|
| WSL2 + Ubuntu | https://learn.microsoft.com/windows/wsl/install — `wsl --install -d Ubuntu` |
| 基础包 | `sudo apt update && sudo apt install -y build-essential git curl ca-certificates python3 perl xz-utils clang lld device-tree-compiler` |
| 7-Zip ≥ 23（读 APFS） | https://www.7-zip.org/download.html；Ubuntu 24.04 也可 `sudo apt install 7zip` |
| theos（Linux 安装） | https://theos.dev/docs/installation-linux |
| iOS 16.5 SDK | https://github.com/theos/sdks — 只取 `iPhoneOS16.5.sdk` |
| iOS 交叉工具链（ld64/nm/otool/lipo） | https://github.com/L1ghtmann/llvm-project/releases — x86_64 用 `iOSToolchain-x86_64.tar.xz`，解到 `$THEOS/toolchain/linux/iphone` |
| Mu-Silicium（仅重编 UEFI 需要） | https://github.com/Project-Silicium/Mu-Silicium |
| Apple XNU 源码 | `xnu/apply.sh` 自动拉 https://github.com/apple-oss-distributions/xnu |
| 资源 | 约 12 GB 空闲磁盘；4 GB 以上内存 |

自检：

```bash
echo "$THEOS"                       # 例如 /home/<user>/theos
command -v clang ld64 7z python3
ls "$THEOS/sdks/iPhoneOS16.5.sdk"
```

### 5.2 搭一棵构建树（两份源码合并到同一目录）

上游脚本按“同一棵树”设计（`userland/build.sh` 会找 `../xnu-deps`、`../tools/apfs`、`../ios165`），
所以先在 Linux 下把 `src/` 的两份源码叠到一起（路径不要有空格）。**可以用 `ubuntu/sync.sh` 自动做**：

```bash
./ubuntu/sync.sh          # 组装到 ~/k40；也可指定目标路径
./ubuntu/build.sh         # 检查前提后一键构建（等价于下面 5.3 + 5.4 带 GL=1）
```

或手动：

```bash
mkdir -p ~/ios-alioth && cd ~/ios-alioth
SRC=/mnt/e/<交付包路径>/ios-alioth/src
cp -a "$SRC/hackios-xnu-kernel-main/." .
cp -a "$SRC/hackios-xnu-userland-main/ios165" .
cp -a "$SRC/hackios-xnu-userland-main/userland" .
cp -a "$SRC/hackios-xnu-userland-main/prebuilt" .
cp -a "$SRC/hackios-xnu-userland-main/tools/." tools/
```

### 5.3 内核

```bash
cd ~/ios-alioth
xnu/apply.sh          # 拉 Apple xnu-8796.121.2，打 redwood.patch + xnu/files
xnu-deps/setup.sh     # 依赖源码、mig/unifdef、SDK overlay、host 工具
DEVICE=alioth ./xnu-build.sh -j$(nproc) -k
```

- `-k` 必须带：最后 `check_all_exports` 会让 make 以非 0 退出，但内核已经链接完成。
  判断依据是 `mach.development.bcm2837` 的时间/大小，而不是 make 的退出码。
- 单独跑 `xnu-build.sh` 时要有 `XNU_DEVICE=alioth`，否则编进去的是 redwood 的地址。
- 改过 `xnu/files/**` 后要重新同步到 `xnu-src/xnu-8796.121.2/`（`apply.sh` 只在目标不存在时才拷）。
- 产物：`xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`
  → 拷回 `boot/kernel-ios.bin`（或 `kernel-latest.bin`）。

### 5.4 用户态、root.apfs 与 system.apfs

```bash
cd ~/ios-alioth
LOCAL_IPSW=<交付包>/firmware/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw ios165/fetch.sh
REST=1 userland/build.sh
```

- 没给 `LOCAL_IPSW` 时，`fetch.sh` 会从 Apple CDN 按字节区间读取两个磁盘映像（约 7.4 GB 流量，不整份下载）。
- 没装 theos + iPhoneOS16.5.sdk 时，`build.sh` 自动 `PREBUILT=1`，用 `prebuilt/userland` 里的程序。
- 产物：`userland/root.apfs`（10.4 MB，含 GL 库与 chronod）、`userland/system.apfs`（2.5 GB）。
- **GPU 渲染必须 `GL=1`**（完整构建命令见 `ubuntu/README.md`）：

  ```bash
  # 1) Mesa + AIR 编译器（首次约 4 分钟；会下载 46 MB 源码）
  DRIVERS="softpipe,freedreno" AIR=1 bash tools/mk-mesa.sh
  # 若 ninja 报 "No module named 'mesonbuild'"：Mesa 的 venv 里缺 meson，装一次即可
  xnu-deps/build/mesa/venv/bin/pip install meson
  # 2) 带 GL 编 userland（libredwood-gl.dylib 进 /redwood/lib，root.apfs 4 MB -> 10.4 MB）
  SKIP="kernel loader boot" ./common/build.sh alioth GL=1
  ```

- **小组件必须 `com.apple.chronod`**：它注册 `com.apple.chronoservices`，没有它 SpringBoard 的小组件
  只有空白容器。已加进 `userland/build.sh` 的默认 `IOS_JOBS`，无需再传 `JOBS=`。
- GPU 固件不在源码里；要打进 `root.apfs`，先在构建树根建 `fw-gpu/`，从手机取四个文件：

  ```bash
  adb pull /vendor/firmware/a650_gmu.bin  fw-gpu/
  adb pull /vendor/firmware/a650_sqe.fw   fw-gpu/
  adb pull /vendor/firmware/a650_zap.mdt  fw-gpu/
  adb pull /vendor/firmware/a650_zap.b02  fw-gpu/
  ```

  没有这四个文件也能启动，只是 GPU 不上电（`boot/root.apfs` 里已经带了它们）。
- 只改 userland 注入库时，同样重跑 `REST=1 userland/build.sh`，再按 5.6 重做载荷。

### 5.5 UEFI 与 boot.img（可选）

用 [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)，把 `src/mu-silicium-config/` 的文件放回对应位置
（见该目录的 README），然后：

```bash
rm -rf Build
python3 build_uefi.py -d alioth -r RELEASE
# → Build/aliothPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd (3,145,728 B)
```

再用 `build_uefi.py` 的 `create_android_boot_img`（`mkbootimg --kernel <FD+gzip> --ramdisk Resources/ramdisk`）
生成 `alioth-boot.img`。不重建就直接用 `boot/alioth-boot.img`。

### 5.6 生成载荷（Windows 侧，cmdline 不能截断）

```powershell
& python.exe src\hackios-xnu-kernel-main\xnuloader\mkpayload.py `
  -o boot\run-payload.bin kernel=boot\kernel-ios.bin ramdisk=boot\root.apfs `
  --cmdline "loader_return_after=240 epoch=1792659000 sb_delay=8 touchscreen=1 buttons=1 touch_stat=5 cpufreq=1 bootcpu=7 cpus=8 adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1 adreno_watch=10"
& python.exe boot\inspect_payload.py boot\run-payload.bin   # 核对 cmdline 全文
```

不要用 `wsl -lc "… --cmdline \"…\""`：引号会被剥掉，参数丢失后 loader 的 USB 服务会无响应。

### 5.7 回填与校验

把新产物覆盖到 `boot/`，然后重算校验和并核对载荷：

```bash
sha256sum boot/*.img boot/*.apfs boot/*.bin      # 或在 PowerShell 用 Get-FileHash
python boot/inspect_payload.py boot/run-payload.bin
```

GitHub 仓库不含 `firmware/` 与 `boot/system.apfs`（见 §3.1 / §7），本地交付包保留即可；不要把它们提交进仓库。

---

## 6. 已知阻塞点（下一手的起点）

| 领域 | 状态 | 下一步 |
|---|---|---|
| GPU（Adreno 650） | **已打通**：内核 step 1–22 全过（2026-10-31 首次上机验证），CoreAnimation 经 Mesa/FD650 渲染，小组件 / Dock 材质 / 切换器全部出图。遗留：`gpu=1` 便捷开关有 bug（用显式参数绕开，见 §4） | 阴影着色器 `sb3`（函数 40）已实现但默认不注入——实测证明本无缺失；细节见 `userland/src/inject.c` |
| 真熄屏 | **假息屏**：电源键后画黑帧，面板仍通电（AMOLED 无背光，亮度走 DCS；K40 面板 k11a 的 WLED 驱动未绑定可证）。XNU 无 DSI 驱动，面板由 UEFI DisplayDxe（闭源 .efi）初始化 | 需在 XNU 实现 MDSS DSI 命令模式 + DCS 0x28/0x10，是多会话工程 |
| 默认壁纸 | iOS 16.5 的壁纸是 PosterBoard 程序化渲染，镜像与固件里**没有位图**；壁纸进程从未运行过，材质回落洋红 | 让 PosterBoard 跑起来并产出 wallpaper store；或注入层给 PaperBoardUI 喂真实图片（`inject_sb.c` 已有挂点说明） |
| 存储 | **已通**：根卷报 darwin 的 20 GiB、可用 12.2 GiB（`runs/df10.log.txt`），About/设置不再显示 "0 可用"；**`/private/var` 跨重启保留**（写窗口在 logdump 的 8–64 MB，两槽 28 MiB 轮换，见 §13.15） | 数据卷仍是 RAM，持久化靠一份约 24 MB 的打包副本：prefs、keychain、容器元数据、图标布局能留住，装 App 和存照片不能 |
| Wi-Fi（QCA6390 PCIe0） | **面板可打开**（2026-11-01：补 configd/securityd/wifianalyticsd）；**链路已通**（2026-11-03，`24f4d4a`：LTSSM L0、端点 `17cb:1101`，三个死因见 §13.17）；**无线电也已起来**（2026-11-04，分支 `wifi-ref-port`：整套照搬参考实现——Linux 的 ath11k 编进 XNU——BHI 512 KiB SBL → MHI M0 → BHIE 4.3 MB AMSS → QMI 下 BDF/board/m3 → **`en0` 存在、`ath11k returned 0`**，见 §13.18）；**扫描与 WPA2 连接已通**（2026-11-04 15:1x 真机：扫到 14 个热点，`AUTHENTICATION status 0`、`ASSOCIATION … station number 2`、`THE NETWORK IS JOINED: frames can pass`，截图 `docs/evidence/2026-11-04-wifi-joined.png`）；**还差 IP**：`networkd_privileged` 不在 job 列表里，DHCP 没跑 | 把 `com.apple.networkd_privileged` 加进 daemon 列表，让 `en0` 拿到 DHCP 地址 |
| 音频 | `mediaserverd` 未启动，音量条电平由 userland 维持 | — |
| CPU | 只有 4 核 online（A55 未起） | — |

---

## 7. 许可与法律说明

- `src/hackios-xnu-kernel-main/` 沿用上游 **APPLE PUBLIC SOURCE LICENSE 2.0**（XNU 部分）与上游各自许可证；
  `src/hackios-xnu-userland-main/` 为 **MIT**（OOSEX GANG）。Mu-Silicium 为 **MIT/BSD**，以其上游为准。
- **`boot/system.apfs` 与 `boot/root.apfs` 是从 Apple 的 iOS 16.5 IPSW 提取/派生的内容，不属于开源许可范围**，
  仅供本人设备研究使用，**不要公开再分发 / 上传到公开仓库**。上游仓库刻意不含任何 Apple 文件，本包为"可启动交付"才附带。
- `firmware/` 里的完整 IPSW 同样是 **Apple 的固件，版权归 Apple**，不在开源许可范围内，**不可公开再分发**；
  它只作为本机重建 `system.apfs` 的输入。
- 本包不是 ROM：不刷机、不写分区，重启回原系统。使用风险自负（解锁 bootloader 本身有风险）。

---

## 8. 校验

```bash
cd open-source
sha256sum -c SHA256SUMS.txt        # Windows 下可用 PowerShell 的 Get-FileHash 对照
```

关键值：

```
b7a413c2…  boot/alioth-boot.img       2,007,040 B
bf3bcab2…  boot/uefi-alioth.fd        3,145,728 B
52f47426…  boot/run-payload.bin      25,600,424 B   (cmdline 见 §4)
a8342e31…  boot/root.apfs            10,420,224 B   (含 GL 库与 chronod，见 §5.4)
ca56e1f1…  boot/kernel-ios.bin       15,129,088 B   (与载荷内嵌的内核字节一致)
28aa4315…  boot/kernel-latest.bin    15,179,888 B   (2026-10-31 真机验证通过的那份)
bc15b275…  boot/system.apfs      2,492,223,488 B   （本地交付包；GitHub 仓库不含，见 §3.1）
```

固件不随仓库发布（6.16 GB，版权原因）：本地交付包里的 IPSW 用 `firmware/*.ipsw.md5` 校验，
基准 MD5 为 `f87b4bd6f3c86fb174ce017b5a5ca97b`；一键启动**不下载**它（启动流程不碰固件，见 §3.1）。

**Windows 检出的两个 `.dts` 校验会报 FAILED**：`core.autocrlf` 检出时把 LF 换成 CRLF 所致，
内容未变（`git diff` 为空即一致）；在 Linux 下校验或以 `git status` 为准。

**脱敏说明**：内核版本串里的构建者用户名已替换为 `buildusr`（等长替换，不影响 Mach-O 布局与运行），
`alioth-boot.img` 内 DTB 的 `serialno` 已置零，文档中的本机路径/用户名已改为占位符。
这些都是字符串层面的改动，镜像仍与真机验证过的产物等价。
