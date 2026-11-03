# GPU 与 Wi-Fi 的真机读数（2026-10-28）

本文件是 `porting.toml` 中 gpu 与 network 两个 area 的 evidence 指向。

## GPU（Adreno 650）

`pipa-gpu=1`，boot-usb106：

```
GPUROW0 GPU m1 gcc=ok cc=ok smmu=skip gpu=skip io=0 cx0 clocks0 ahb0
```

`pipa-gpu=6`，boot-usb107：

```
GPUROW0 GPU m6 gcc=ok cc=ok smmu=ok gpu=skip io=0 cx1 clocks1 ahb0
```

`pipa-gpu=7`，boot-usb122（附写测试）：

```
GPUROW13 GMUCTX step 4 status e00002ca kit 0/10 routes 0 id2 5511 phase 5 wr 13 rb 1 en 1
GPUROW14 PRE idr0 4c017e06 smr5 8000007f->00000000 scr0 0026040e->00260406
```

结论：CX 域可上电、SMMU/GMU 时钟可运行；GMU 上下文在 Attach 步失败，因为**这颗 SMMU
对 EL1 的写全部被接受且全部不生效**（SMR 与 sCR0 两次写入均用了不同值，均读回原值），
而同一映射的读正确、同构造的 GPUCC 窗口写生效。

## Wi-Fi（QCA6390）

`pcie=1`，boot-usb129：

```
PCIE 1: gcc pcie_0_gdscr (0xfffffff84986b004) = 0x22f001
```

`pcie=2`，boot-usb130：

```
PCIE 2: about to write gcc pcie_0_gdscr ...: 0x22f000
PCIE 2: gcc pcie_0_gdscr after (...) = 0xf822f000
PCIE 2: THE CONTROLLER HAS POWER (gdscr 0xf822f000, 1 ms)
```

`pcie=3`，boot-usb131：`aux_halt = 0`、`slv_q2a_halt = 0`。

`pcie=4`，boot-usb132：`phy_status = 0x68`，bit 6 置位，未写 PHY 即就绪。

`pcie=9 pcie_skip=32`，boot-usb133：

```
PCIE 9: parf_mhi_clock_reset_ctrl = 0x3 -> 0x13
PCIE 9: parf_axi_mstr_wr_addr_halt_v2 = 0x80000014
```

结论：控制器可从完全塌陷上电并经 PARF 配置，全部与 ios-sweet 系列在这台机器上的记录一致。
step 10（DBI/配置空间/链路训练）在参考实现里也从未运行。

## 追加：step 5 通过，step 10 仍未通过

`pcie=5`（不跳过 PHY）：

```
PCIE 5: gcc pcie_phy_aux after the reset (0xfffffff274507004) = 0x1
PCIE 5: gcc pcie_0_phy_bcr after (0xfffffff27450401c) = 0
PCIE 5: about to write gcc pcie_phy_com_bcr (release) at 0xfffffff274507010: 0
PCIE 5: the PHY IS READY (status 0x69, bit 6 is its ready)
PCIE 5: step 5: done
```

ios-sweet 当年跳过这一步（`pcie_skip=32`），因为它释放 PHY common reset 时会把整机打死。
**在本内核与本板上它跑完且 PHY 就绪（0x69）**，所以那条限制不适用于这里。

`pcie=10`（已有 pipe clock）：日志停在 `step 9: done`，设备回到 bootloader。
所以"step 10 需要 pipe clock"这个判断**最多只对了一半**，下一步要读的是它第一个访问碰了什么。

## step 10 是只读的，却把机器打死

`alioth_pcie.c:603` 的 `step_dbi()` 全部是 `get()`（读）：

```
get(4, "iatu_viewport", 0x00);
get(5, "conf_vendor_device (bus 0, device 0, function 0)", 0x00);
get(5, "conf_header_type", 0x0c);
get(6, "dbi_vendor_device", 0x00);
get(6, "dbi_capability", 0x70);
get(3, "elbi_sys_ctrl", 0x04);
```

所以在有 pipe clock 的情况下它仍然复位，**不是写坏了什么，而是第一次访问就挂了**——
按顺序是窗口 4 的 `iatu_viewport`（`0x60001000`）。

这条把下一步缩小到一个具体动作：**单独读窗口 4 的 0x00**，其它都不碰。
如果那一读就挂，问题在 IATU 窗口的映射或它的访问许可；如果读得回来，再往后一个一个试。

## 触控：每一轮的真机读数（2026-10-28）

| 轮次 | 配置 | 关键读数 | 结论 |
| --- | --- | --- | --- |
| 起初 | 单一 HID 设备 | `hres=1`、`hid` 缓慢增长、界面不动 | 报告被拒（当时的判断） |
| 分离设备 | `sheng-touch-split=1` | `tsplit=1` | 设备起来了 |
| 分离 + 触摸 | 同上 | `low=190`、`raw=000042e10607`、`[touch multi] lifts=14` | **触控核心正常，14 次接触都完成** |
| 判据更正 | — | 分离前 `edge[1]` 曾到 16/23 | `hres=1` **不是拒绝**，合并设备返回它是预期 |
| edge 修复前 | 全局 `originalDispatch` | `edge=1,0`、`[touch multi] reports=0` | 分离设备的驱动从未被包，`FromEdgeTip` 不注入 |
| edge 修复后 | per-driver（子模块 `1eade81`） | `reports 0→781`、`max=5`、`lifts=88` | 事件进系统了；但 **5 个同时触点是"手掌"** |
| area 过滤 + 关 split | `tsplit=0` | `irq=1`、`low=0`、`raw=ffffffffffff` | 抓取窗口内芯片无数据（空闲态） |

**`low` 不是中断计数**：源码里它是轮询时看到 GPIO 39 为低的次数；`irq=1` 是当前电平（高=空闲）。
`int=01` 是芯片内部计数器 0x8F 的残留值，与 `low` 不矛盾。

**下一步的判据**：在确认手指按住屏幕的窗口里读同一行。
`low` 上升 → 芯片在报事件，问题在 HID 语义（`max` 是否仍 >1、area 过滤是否把它们全滤掉）；
`low` 恒为 0 → 芯片没有扫描，问题在触控 IC 侧，与 HID 无关。

## 2026-10-30：step 10 复现，并把范围缩到时钟投票

`pcie=10`（十步全开、不跳过任何一步）上板后**板子再没回来**：无 fastboot、无 adb、
`/dev/tty.usbmodem14101` 不枚举。上一次同样的尝试记录是「日志停在 `step 9: done`，设备回到
bootloader」，这次连 bootloader 都没到。同一动作两种结果，说明这条路径现在每次尝试都可能要断电，
且不一定换得到信息。参数已撤回。注意 `pcie=10` 是记录里早就写好的下一步（`step_dbi` 只做一次
`get(4, "iatu_viewport", 0x00)`），不是新试验。

**窗口地址与 vendor 完全一致，不是映射错。** 对照 `reference/k40-kernel/arch/arm64/boot/dts/vendor/qcom/kona-pcie.dtsi:7-13`：

| reg-name | vendor | 我们 |
| --- | --- | --- |
| parf | `0x01c00000 0x3000` | 同 |
| phy | `0x01c06000 0x1000` | 同 |
| dm_core | `0x60000000 0xf1d` | 同 |
| elbi | `0x60000f20 0xa8` | 同 |
| iatu | `0x60001000 0x1000` | 同 |
| conf | `0x60100000 **0x100000**` | `0x1000` ← 唯一差异，且不是挂死的那次访问 |

**真正可疑的是时钟投票集合。** vendor 的 `clock-names`（同文件 `:76-81`）列了十一项：

```
pcie_0_pipe_clk, pcie_0_ref_clk_src, pcie_0_aux_clk, pcie_0_cfg_ahb_clk,
pcie_0_mstr_axi_clk, pcie_0_slv_axi_clk, pcie_0_ldo, pcie_0_slv_q2a_axi_clk,
pcie_tbu_clk, pcie_phy_refgen_clk, pcie_ddrss_sf_tbu_clk
```

而 `step_clocks`（`alioth_pcie.c:318`）只投了五项：slv_axi、mstr_axi、cfg_ahb、aux、slv_q2a。
**缺的是 `pcie_0_ldo`、`pcie_phy_refgen_clk`、`pcie_0_ref_clk_src`、`pcie_tbu_clk`、
`pcie_ddrss_sf_tbu_clk`。** 其中 LDO 与 refgen 直接决定 core 与 PHY 的供电和参考时钟——
在它们没投票的情况下访问 IATU，最可能的后果就是总线不返回，与实测吻合。

下一步（不需要再猜）：把 `pcie_0_ldo` 与 `pcie_phy_refgen_clk` 的投票补上，其余不动，
只跑到 `step 9`（不碰 DBI）确认这两项不挂；确认后再开 `pcie=10`。这样断电次数从「每次未知」
变成「每一步可预期」。

## 追加：`elbi` 与 `dbi` 的映射重叠不解释挂死

窗口表（`alioth_pcie.c:88-98`）里 `elbi`（`0x60000f20` + `0xa8`）整个落在 `dbi`
（`0x60000000` + `0x1000`）之内，而 `map_all()` 是逐块各调一次 `ml_io_map`，不去重也不检查包含。
这看起来像「同一段物理地址被映射两次、可能拿到坏 VA」。

**但 `step_look`（第 1 步）就成功读了 `get(3, "elbi_sys_ctrl", 0x04)`**——同一个物理 4 KiB 页，
而且 `pcie=1` 到 `pcie=9` 全部通过。所以：

- 那段物理地址的映射与总线应答是好的；
- 重叠本身不会让读挂死，否则第 1 步就该挂；
- 挂死是 **`iatu`（`0x60001000`）这一页特有**的，而它与 `elbi` 不在同一页。

这把方向从「映射错」移到「IATU 这一侧的时钟或复位」。下一步仍是纯代码：读
`pcie_0_cfg_ahb_clk` / `pcie_0_slv_axi_clk` 的 branch 使能与 halt 状态（`step_clocks`
已读过一部分，属安全区），确认 IATU 所在的 AXI slave 分支有没有真的跑起来。
