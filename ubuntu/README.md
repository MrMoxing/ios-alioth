# ubuntu/ — 在 Ubuntu 上从源码构建这套系统

这个目录不含二进制，也不含工具链。它的作用只有一个：**把 `src/` 里两份上游源码组装成一棵
可以直接构建的工作树，并把构建命令固定下来**，免得每次靠记忆拼参数。

## 为什么需要它

`src/hackios-xnu-kernel-main` 和 `src/hackios-xnu-userland-main` 是上游的两个独立项目，
但**真正的构建工作区是它们的并集**：内核侧提供 `xnu/`、`common/`、`xnuloader/`、`uefi/`、
`xnu-build.sh`，用户态侧提供 `userland/`、`ios165/`，两边的 `tools/`、`prebuilt/`、`docs/`
互不重叠（2026-10-31 逐文件核对过：并集 135 个文件覆盖 WSL 侧全部 130 个）。

所以 `~/k40` 不是任何单个仓库的检出，而是两者的叠加。`sync.sh` 做的就是这件事。

## 用法

```bash
# 1) 组装工作树（默认 ~/k40）
./ubuntu/sync.sh            # 或 ./ubuntu/sync.sh /path/to/workspace

# 2) 构建（首次会自动下载并编译工具链、拉取 Apple 的 XNU 源码）
./ubuntu/build.sh           # 或 ./ubuntu/build.sh /path/to/workspace

# 不要 GL（窗口服务器退回软件渲染）
GL=0 ./ubuntu/build.sh
```

产物：

| 文件 | 内容 |
|---|---|
| `out/alioth-boot.img` | UEFI + loader，`fastboot boot` 用它 |
| `out/run-payload.bin` | 内核 + `root.apfs` + 启动参数 |
| `userland/root.apfs` | 精简根：`init`、`/redwood/lib` 下的自有库、launchd job |

## 机器上要先有

构建脚本会自己下载源码并编译工具链，但下面这些必须由系统提供：

| 类别 | 需要 |
|---|---|
| 基础 | `git curl tar xz make cc clang flex bison m4 perl python3 autoconf` |
| 内核链接 | `~/theos/sdks/iPhoneOS16.5.sdk` 与 `ld64`（`xnu-deps/setup.sh` 会处理一部分；缺 theos 时内核退回 `prebuilt/`，**源码改动就不会被编译**） |
| loader | `clang` + `lld-link`（缺则用 `prebuilt/loader.efi`） |
| GL 路径 | `meson ninja bison flex llvm-ar`（`tools/mk-mesa.sh` 需要；`meson` 建议装进它自己的 venv，见下） |
| iOS 文件 | `ios165/rootfs`（`ios165/fetch.sh` 从 Apple 下载约 5.7 GB，`build.sh` 的 userland 阶段依赖它） |

## 两个必须知道的坑

**1. `check_all_exports` 失败是预期行为。** 内核链接完成后配置阶段会报几百个
`exported name not in import list`，这是 `xnu/README.md` 里写明"on purpose"的，
判据是 `BUILD/obj/.../mach.development.bcm2837` 有没有更新，不是 make 的退出码。

**2. Mesa 构建要在它自己的 venv 里装 meson。** `tools/mk-mesa.sh` 把
`xnu-deps/build/mesa/venv/bin` 放到 `PATH` 最前面，而那个 venv 默认没有 meson，
ninja 重新生成阶段会报 `No module named 'mesonbuild'`。先做一次：

```bash
DRIVERS="softpipe,freedreno" AIR=1 bash tools/mk-mesa.sh   # 首次会失败在这一步，属正常
xnu-deps/build/mesa/venv/bin/pip install meson
DRIVERS="softpipe,freedreno" AIR=1 bash tools/mk-mesa.sh   # 再跑一次即可完成
```

产物在 `xnu-deps/build/mesa/out`：`libOSMesa.8.dylib`（14.8 MB，1296 个 `gl*`）、
`libredwoodair.dylib`（9.3 MB，31 个 `rwa_*`）。

## 开机参数（真机）

```
adreno_soc=a650 adreno=21 adreno_skip=0 adreno_pause=0 adreno_keep=1
```

`gpu=1` 是文档里的便捷写法，等价于 `adreno=21 adreno_skip=0x1c00 ...`，但它在当前内核上
**会跳过 step 11（TrustZone）和 step 12（zap 签名）**，导致 step 19 的 `zap_unload` SMC 挂死，
所以这里写显式参数。`adreno_soc=a650` 不能省，否则 SoC 回退 a660、去找并不存在的固件。
