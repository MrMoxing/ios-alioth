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

## 5. 上机实录（2026-11-04 13:1x–13:2x，真机，我们的 loader + 我们的 system.apfs）

参考内核直接跑在**我们现有的上机装置**上（`boot/alioth-boot.img` + `boot/system.apfs` + 我们的 init），
没有换 loader、没有换系统镜像。三轮：

| 轮 | payload | 结果 |
|---|---|---|
| `refwifi` | 参考内核 + 我们的 root.apfs（**fw 名字不对**） | 桌面起来了，pcie 走到 step 31 后 `pinned device firmware absent; no DMA` |
| `refwifi2` | + `qca6390_*` 四个固件 | **整条 Wi-Fi 栈跑通到"驱动起来了"**，只差 MAC（见下） |
| `refwifi3` | + `wifi_mac=<本机 MAC>` | 见 §5.2 |

### 5.1 refwifi2 的关键日志（`runs/refwifi2.log.txt`，1.3 MB）

```
pcie: 28: endpoint 01:00.0: id 0x110117cb (vendor 0x17cb, device 0x1101)
pcie: 29: BAR0 mask 0xffffffff:0xfff00004, size 0x100000
pcie: 29: BAR0 0x60300004, command 0x2; DMA disabled
pcie: 29: MHI len 0x100 ... BHI offset 0x100, BHIE offset 0x224, chdb 0x300, erdb 0x700
pcie: 30: apps SMMU scr0 0x270406, idr0 0x4c017e57, idr1 0x6000004a, SID 0x1c01
pcie: 30: SID 0x1c01 -> match 7 context 6, root 0x6000191a04000; isolated 0xc00000-byte DMA mapping
pcie: 31: PBL reset acknowledged before BHI; ctrl 0 status 0
pcie: 30: requested signed SBL transfer (512 KiB) through BHI
pcie: 31: after SBL transfer MHI status 0x101, execenv 0x1, version 0x1000000
pcie: 31: MHI cfg 0x30880, context bases 0xa0000000/0xa0002000/0xa0003000
pcie: 31: after M0 MHI status 0x201, execenv 0x1
pcie: 31: requested 4352144-byte AMSS transfer in 9 BHIE vectors
pcie: 31: BHIE status 0x9e7f6f75, execenv 0x2, MHI status 0x201
pcie: 31: mission observation MHI status 0x201, execenv 0x2, stable M0 2000 ms
ath11k: fw_version 0x1016046e fw_build_timestamp 2023-10-12 01:51 ... QCAHSTSWPL_SILICONZ
ath11k (debug): BDF download sequence completed
ath11k (debug): firmware init done / firmware start / wlan cfg req / wlan mode req mode 0
lx: the driver registers a radio: channels: 14 in band 0, 28 in band 1, 60 in band 3
lx: the regulatory domain "CN" (4 rules) allows the radio's channels: 13 of 14 in band 0 ...
lx: 249 ms after its start the driver IS UP as far as it is built: the firmware has said "unified ready"
lx: its address, by the firmware: 00:00:00:00:00:00
lx: THE RADIO HAS NO ADDRESS (the kernel was given none, and the firmware has none): no interface is made
ath11k (debug): firmware stop / wlan mode req mode 4
pcie: 35: ath11k returned 49
```

**这就是答案**：整套照搬在本机是可行的——BHI 的 512 KiB SBL、BHIE 的 9 段 AMSS、QMI 的 board/regdb/m3、
ath11k 的 pdev 起、固件"unified ready"、射频通道表、CN 监管域，全部走通。
唯一断掉的一处是 **MAC 地址**：固件报 `00:00:00:00:00:00`，`lx` 于是不建接口。

`wifi_mac` 是参考实现自己的开机参数（`hakios.radio.c` 用 `PE_parse_boot_arg_str("wifi_mac", …)`，
要求 17 个字符）；本机真 MAC 从 Android 的 `/mnt/vendor/persist/wlan_mac.bin` 读出
（`wlan0=6cf7848b81b0`），`refwifi3` 带的就是它。

### 5.2 refwifi3：`wifi_mac` 一给，接口就出来了

```
wifi: station address supplied and validated
lx: early station interface attached 1
net_test: wifi: en0 is there after 0.0 s
net_test: wifi: configd has a network service for en0 after 1.4 s: it configures it itself
lx: coherent allocation 6160384 bytes at IOVA 0xa1020000 …（AMSS/M3 的段、rings、copy engines）
lx: the rings are set up and the copy engines have their buffers: the bus is started
lx: 257 ms after its start the driver IS UP as far as it is built: the firmware has said "unified ready"
pcie: 35: ath11k returned 0
pcie: 36: station offered to iOS; keeping isolated endpoint until session return
```

**结论：整套照搬在本机可行。** 参考内核 + 我们的 loader + 我们的 iOS 镜像，开机进桌面，
`en0` 建起来、`configd` 自己给它配了网络服务、ath11k 以 0 返回、station 交给 iOS
（截图 `runs/refwifi3-1.png`：桌面正常，状态栏还没有 Wi-Fi 图标——扫描要进
Settings → Wi-Fi 才会发生，这一步还没做）。

### 5.3 扫描这一步没拿到（四次尝试，13:39–14:17）

iOS 只在 **Wi-Fi 面板打开时**才让 wifid 去扫描，所以问题是"把设置页打开"。四次都没成：

| 尝试 | 做法 | 结果 |
|---|---|---|
| `wifiscan` | 20 s 时 `rwtouch` 直接点 Settings 图标 | 没反应；后来发现 **锁屏还没滑走**，点在锁屏上 |
| `wifiscan3` | 通知 `com.redwood.wifi`，注入库用 `LSApplicationWorkspace` 开 `App-Prefs:root=WIFI` | 处理函数一句话都没打印，通知没送到 SpringBoard |
| `wifiscan4` | 开机参数 `open_wifi=1` 定时 30/60 s 触发 | 也没有打印——**参考内核里 `sysctlbyname("kern.bootargs")` 不回答**，开关根本没被读到 |
| `wifiscan5` | 场景先 `var_store stamp /private/var/mobile/open-wifi`，注入库在 25/55 s 检查该文件 | 文件确实建了（`29 bytes added`），注入库仍无输出 |
| `wifiscan6` | 按 `rc.jobs.launch` 已验证的序列：40 s 上滑解锁 → 46 s 点 (0.615, 0.71) → 52 s 点 Wi-Fi 行 | **SpringBoard 在 14:14:52 重启了**（pid 8 → 99），截图正好抓在 BootUI 的转圈上；没有扫描 |

`wifiscan5/6` 说明两件事：注入库本身是加载的（`redwood: SpringBoard: …` 有），但**延时块在参考内核上没执行**，
而 `wifiscan6` 的 SpringBoard 重启把界面重置了。这一层（自动化点 UI）比修驱动更费时间，先停在这里。

### 5.3.1 真正的原因：参考内核上 `sleep` 不返回（14:41 定案）

第六轮把问题定住了。场景里最普通的一行：

```
=== /redwood/etc/rc.jobs ===
$ sleep 30
```

**之后再没有下一条命令**。也就是说：

| 现象 | 真因 |
|---|---|
| `rwtouch wait 25 notify …` 只打印命令行、不通知 | `rwtouch` 的 `wait` 就是 sleep，卡在第一次睡 |
| `rwtouch wait 40 swipe …` 一次触摸都没发生 | 同上，滑屏那一步根本没执行到 |
| 注入库里 `dispatch_after` 从不执行（13 次 `at start`、0 次 `25 s in`） | 延时块同样依赖计时器 |
| 线程版定时器只打印第一次心跳就停 | 第一次 `sleep(6)` 过了，第二次不再醒来 |

**结论：这套内核里，用户态 `sleep()` 一次之后就再也不返回**，所有"等一会儿再做"的自动化都失效。
所以扫描这一步不该再靠延时触发。

再进一步：把定时器换成 `pthread` 也只成了一次。`wifiopen3` 里线程的**第一次** `sleep(6)` 回来了
（`timer 25 s: heartbeat 1`），第二次没有；把设计改成"只睡一次"（`start_after(10)` → `unlock()` →
开面板）后，`wifiscan7` 只留下 `redwood: start_after(10) called`，**10 秒的这一次也没回来**。
所以在这套内核上，用户态定时器不是"慢"，是**偶发地永远不醒**——任何"等一会儿再做"的自动化都不可靠。

（注意：这不是 iOS 本身的问题，wifid/launchd 的定时器都在跑；是本移植的内核里，我们这些
进程/线程的 `sleep` 与 `dispatch_after` 醒来这一侧不可靠。）

### 5.3.2 用探针量出来：短等待正常，等待久了就不醒（`rc.jobs.sleep`，15:0x）

`src/sleep_test.c` 一个进程里依次问每一种等待，各打一行 before/after：

```
usleep(1000)    came back after    3 ms
usleep(200000)  came back after  202 ms
sleep(1)        came back after 1003 ms
sleep(5)        came back after 5004 ms
nanosleep(2 s)  came back after 2004 ms
select(2 s)     came back after 2004 ms
sleep(1) again  asked …（再没有回来）
```

**结论**：等待本身是对的（六个都准到毫秒），**同一个进程里第七次等待永远不醒**；
而 `rwtouch wait 25` 这种"一次长等待"在 `wifiscan3` 里也没回来。再往前推一层，
场景里的 `sleep N` 并不是 `sleep(2)`，而是 `init.c` 的内建实现——**`N*5` 次 `usleep(200000)`**：

```c
for (int left = seconds * 5; left > 0; left--) usleep(200000);
```

所以 `sleep 20` 在这个内核上大约 **1.4 秒后就死在第七个 200 ms 上**，
之前所有"等了 20/30/40 秒"的场景其实都没等到，日志停在命令行那一行。

**这解释了全部七轮失败**，也划出了可用的边界：**单次、几秒以内的等待可以用；再长就不可靠。**

**下一步（不依赖计时器）**：挂在事件上——`libredwood-sb.dylib` 已经处理解锁
（`com.redwood.unlock` → `unlock()`，每次开机自动解锁时会走一遍），在 `unlock()` 之后顺手
`open_wifi_settings()`，条件是标记文件在。解锁是 SpringBoard 自己的事件，不需要任何 sleep。

**更省的办法**：把网络写进 `fw/network`（参考实现的 `lx_os_network`：第一行 SSID、第二行密码），
驱动自己连，不需要设置页；或者跑一轮不自动返回的（`SECS` 长、`REBOOT=0`），由人手动点开 Wi-Fi 面板，
再从日志里读扫描结果。

### 5.3.3 开放网络（无密码）连不上：参考实现根本不收（15:2x 定案）

校园网 `SDP-WeNet` 在 Settings 里点下去得到 **"Unable to join the network"**，而热点的 WPA2 一次就通。
原因不在固件，在 `wifi/lx/src/lx_80211.c` 的 `protection()`：

```c
if (rsn == NULL) {
    return (target->capability & 0x10 ? "it is protected by something older than WPA2" : "it is not protected at all: an "
           "open network is not joined by this");
}
```

**一个 beacon 里没有 RSN 元素，就是开放网络；这个分支直接把它拒了**（返回 `ENOTSUP`），
`hakios.apple80211.c` 把 `key_length == 0` 的请求标成 `HAKIOS_STA_OPEN` 之后照样走到这里，
于是 iOS 只能报"无法加入"。参考项目自己验证的是 WPA2-PSK，这条路它没走过。

修法（`ubuntu/ref-open-network.patch`，四处，都不改变 WPA2 行为）：

| 位置 | 改动 |
|---|---|
| `protection()` | 无 RSN 且不声称加密 → `*group_cipher = 0` 并放行；声称加密却没有 RSN（WEP）仍旧拒绝 |
| `request_elements()` | 只有 `group_cipher != 0` 才带 RSN 元素，开放网络"什么都不说" |
| 关联请求 capabilities | Privacy 位跟着网络走：`0x01` + 有密钥时 `0x10` |
| 握手与密钥 | `group_cipher == 0` 时跳过四步握手与 `set_key`，直接 `ASSOC → AUTHORIZED`，然后照常 `THE NETWORK IS JOINED` |

**另一处**：`hakios.radio.c` 的 `hakios_wifi_network()` 在这个机型上是**空桩**（`return false`），
所以 `wifi_join=1`（读 `fw/network` 自连）从来没生效过——只有 iOS 从 Settings 发起的连接是真的。
现在它从 `fw/network` 读：第一行网络名，第二行密码，**第二行空=开放网络**。

**一个必须记住的副作用**：`wifi_join=1` 是"内核自己连"，射频**不交给 iOS**（走的是 `lx_station_join`，
不是 `lx_station_offer`），所以 Settings 里的 Wi-Fi 开关是灰的、打不开；`wifi_hold` 一到驱动还会收掉，
界面就彻底没有无线电了。要 iOS 能用开关，必须是 `wifi_station=1 wifi_keep=1`。

### 5.3.4 "连上了但浏览器打不开网页"：这台机器上没有 DHCP，也没有 DNS

`en0` 关联成功、帧可以过，但浏览器说 "the server can't be found"，日志里同时有：

```
failed lookup: name = com.apple.networkd_privileged, requestor = configd[9],  error = 3: No such process
failed lookup: name = com.apple.networkd_privileged, requestor = wifid[11],  error = 3: No such process   (4 次)
```

**`com.apple.networkd_privileged` 在这份 iOS 16.5 卷里根本不存在**：`/usr/libexec/networkd`、
`com.apple.networkd.plist` 在完整清单（`ios165/rootfs.slt`，763 万行）里都没有，
System Cryptex 里也没有（那里只有 `usr/lib`）。iOS 16 的 DHCP 客户端就是这个守护进程，
没有它 → 没有地址、没有 DNS 服务器 → Safari 只能报"找不到服务器"。
`mDNSResponder`（解析器本身，1.4 MB）在镜像里、job 也在，但**从来没进过 job 列表**，已加（`MDNS=1` 默认开）。

**更正（15:41 实测）**：地址和解析**其实是通的** —— 桌面截图里 Safari 已经加载出 `sina.com.cn`
（`runs/openfix-1.png`），说明 DNS 解析成功、TCP 连上、TLS 握手也走到了证书那一步。
真正卡住的是证书：日志里 **76 次 `failed lookup: name = com.apple.trustd, requestor = com.apple.WebKit`**，
`trustd`（判证书的守护进程）不在 job 列表里，WebKit 得到"没人负责评判"，Safari 就显示
"This Connection Is Not Private"。镜像里 `usr/libexec/trustd` 和它的 plist 都在，已加进列表（`TRUSTD=1`）。
`com.apple.networkd_privileged` 那五条查找仍然存在，但没有挡住地址与 DNS —— 说明这条路径上
configd 自己做了这件事（至少对 WPA2 热点）。

能拿到的结论：**"能上网"这条链差的不是驱动，是地址与解析**。三条可行路：

1. **自己写一个 DHCP 客户端**（BPF 收发包 + `SIOCAIFADDR` + 路由 socket），顺手把 DNS 写进 configd 的动态存储，
   最完整，工作量也最大。
2. **静态地址**：`en0` 用固定 IP/网关/DNS（校园网段内可行），configd 的 `preferences.plist` 里给这个服务写死——
   不需要 DHCP，但每换一个网络就得改。
3. **换一个带 `networkd` 的 iOS 版本/卷**（16.5 之后的卷里它可能在 System Cryptex 里），最省事但要重做镜像。

### 5.3.5 开放网络真正的拒绝点：`station_join()` 的第一行（16:0x，已修）

上一节改的 `protection()` 是对的，但**没有执行到**。开放网络真正的闸门在 `station_join()` 开头：

```c
if (what->security != HAKIOS_STA_WPA2_PSK) {
    lx_tell("lx: a network without WPA2 and a passphrase is asked for: that is not built
");
    return (ENOTSUP);          /* ← Settings 的 "Unable to join the network" 就是这一行 */
}
```

它在任何扫描、任何 `protection()` 之前，所以开放网络连 beacon 都没读就被 `ENOTSUP` 退回去了。
这就是为什么四轮日志都指向 `protection()` 却始终不起作用——**看代码要看入口，不要从症状附近开始读**。

修法：`HAKIOS_STA_OPEN` 一并放行，后面那条（不带 RSN 元素、不置 Privacy 位、跳过四步握手、
`ASSOC → AUTHORIZED`）已经在上一节里写好了。

真机状态：用户确认 `SDP-WeNet` 现在可以加入；**但日志窗口没有抓到那一次**
（`runs/opengate.log.txt` 覆盖 16:00:16–16:04:42，`join` 出现 0 次；更早的四次 `NETWORK IS JOINED`
全是 WPA2 热点）。下一轮补一条 `apple80211: a join was asked for … AN OPEN NETWORK` 的现场记录，
这件事才算有证据闭合。

### 5.4 扫到、并且连上了（15:1x，真机，`runs/wifihand3.log.txt`）

手动点开 Settings → Wi-Fi（这一步自动化做不了，原因见 §5.3），射频侧一次就到位：

```
lx: THE SCAN HEARD 14 NETWORKS (access points): 8 at 2.4 GHz, 6 at 5 GHz;
    by channel: 2 on 1, 3 on 6, 3 on 11, 2 on 36, 2 on 44, 2 on 157,
    the strongest at -16 dBm, the weakest at -92; 13 open, 0 without a name
lx: the network that is wanted: an access point on channel 6 (2437 MHz) at -16 dBm,
    capabilities 0x1431 … WPA2 with a passphrase and CCMP: it can be joined
lx: AUTHENTICATION: the access point accepts (status 0)
lx: ASSOCIATION: THE ACCESS POINT ACCEPTS (status 0); we are its station number 2
lx: THE KEYS ARE GIVEN TO THE DRIVER, 33 ms after the association
lx: THE NETWORK IS JOINED: frames can pass
lx: the network is joined, 35 s after it was joined: 18 frames sent (0 could not be),
    4 received for us and 0 sent to everybody; 2 messages of the handshake
```

屏幕上出现的真实热点（截图 `docs/evidence/2026-11-04-wifi-joined.png`）：
`SDP-WeNet`（-54）、`SDP-WeNet-Teacher`、`OnePlusAce 6`（-7，就是它连上的那个）、
`h#hMagic8 Pro`，另有一个中文 SSID 显示成乱码。**四步握手完成，帧可以通过。**

**到此为止，M3（扫到 SSID）与 WPA2 关联都通了。** 差最后一段：IP。日志里五个
`failed lookup: com.apple.networkd_privileged` —— 这个守护进程不在我们的 job 列表里，
所以 DHCP 没跑，`en0` 还没有地址。

### 5.5 下一步：把 `networkd_privileged` 加进 job 列表，让它拿 IP

`build.sh` 的默认 daemon 列表里没有 `com.apple.networkd_privileged`；补上它（可能还需要
`com.apple.networkd`、`com.apple.apsd`），重启后看 `en0` 是否拿到 DHCP 地址，然后才是 Safari。

### 5.6 下一步（旧）

1. 进 Settings → Wi-Fi 看是否扫到 SSID（可以用 `rwtouch` 点，或手动点）。
2. 能扫到就把网络写进 `fw/network`（参考实现的 `lx_os_network`：第一行 SSID、第二行密码）试连。
3. 决定：整包采用，还是把 §7 的四条补进我们自己的 `alioth_pcie.c`。

## 6. 风险与未知
| 风险 | 影响 | 备注 |
|---|---|---|
| 两份源码下不下来（400 MB） | 整个方案停在构建 | 只此一处外部依赖；失败可换镜像/离线包 |
| `ath11k.o` 的 stubs | 某些路径只打印名字并返回 0 | 参考实现已用它连上 WPA2，说明关键路径不在 stubs 里 |
| 参考实现的 loader/userland 与我们不同 | 上机脚本要换 | 他们的 `xnuloader`（含 `hakios_image_file` 读 `fw/`）与人家的 `xl` 工具链才是配套的 |
| 系统镜像 | 可能仍可用我们的 `boot/system.apfs` | 同一 iOS 16.5 rootfs；但 `en0` 需要 `wifid` 起来，且 `apple80211` 的接口名要对上 |
| 与我们自己的 `alioth_pcie.c` 冲突 | 两套 PCIe 驱动同时编进内核 | 照搬时应把我们的从内核配置里摘掉 |

## 7. 与我们自己路线的对照（这次照搬最该带走的东西）

即便最后不整包采用，下面四条是我们自己那份 `alioth_pcie.c` 必须补的，且已被参考实现验证过：

1. **BHI 只装 SBL（512 KiB）**，AMSS 随后走 BHIE 的 9 个 512 KiB 向量——p36 把整个 4.3 MB 塞进 BHI，
   endpoint 200 ms 后全 1（PBL 不接受这种镜像）。
2. **SMMU 要自己写**：SID `0x1c01`（不是 `0x1c00`），IOVA `0xa0000000`，`CBAR/CBA2R` + 页表 + context，
   然后才允许设备 DMA；给设备的是 IOVA，不是裸物理地址。
3. **写 BHI 前先复位并等 PBL**：`PCIE_SOC_GLOBAL_RESET`（经 WLAON 窗口 `0x310c` 打开 `0x3008`）→
   `qca_wait_pbl_reset`（`MHICTRL` bit1 清、`MHISTATUS` bit4 清）——"MHI RESET 还没清就写 BHI 描述符"会被丢掉。
4. **MSI 与轮询两种都要**：`BHI_INTVEC=0` 走轮询；`qca_boot_msi` 给的是 MHI 的 MSI 目标。

## 8. 本分支的边界

- 分支 `wifi-ref-port` 只放：本文件、构建脚本（`ubuntu/ref-wifi-build.sh`）、以及照搬过程中的适配提交。
- 不合入 `main`：`main` 上的 `alioth_pcie.c` 路线继续推进，二者不混编。
- 一旦照搬验证通过（能在 Settings → Wi-Fi 里连上 WPA2），再决定是"整包采用"还是"把第 5 节的四条补进我们自己的驱动"。
