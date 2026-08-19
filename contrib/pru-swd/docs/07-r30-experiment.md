# 07 — R30 SWCLK 实验报告（负结果，已回滚）

分支 `pru-swd-r30`，2026-08-19。结论先行：**功能可行、性能无增益、
已回滚**。保留完整记录，避免任何人（包括未来的自己）重蹈。

## 动机与假设

stock 方案 iters=0 时每时钟 2 个 SWCLK OCP 写，测得地板 439ns/时钟。
假设：把 SWCLK 挪到 PRU 本地输出寄存器 **R30**（单周期，免 OCP），
SWDIO 留 GPIO1（换向必须 OE 翻转）。预计每时钟省 ~300ns → ~2× 提速。

P8_12 (GPMC_AD12) 恰好是 `pru0_r30_14`——同一根线不用改接线，只改
dtb mux mode。零硬件成本的完美实验。

**假设错在哪**（后见之明）：OCP 写是 fire-and-forget 流水（~150ns/个，
背靠背不阻塞 PRU），根本不是瓶颈；真正的墙是每采样位的 **OCP 读**
（~300ns 全延迟阻塞）。详见 [06](06-timing.md#ocp-物理节拍)。

## 过程

### 1. pinmux 事实（血泪）

**GPMC 球的 pruout 是 MUX_MODE6，不是 5。**

- `config-pin` 工具输出的 "mode 5" 是**位置序号**，不是 TRM mux mode
  值——被它坑了。mcasp/lcd 球的 PRU 功能才是 mode 5（家族差异，
  无统一规律）
- 权威源：beagleboard `bb.org-overlays/tools/pinmux-generator/
  BeagleBone_Black.dts`，或 TRM padconf 表
- GPMC 球 PRU 功能速查：P8_11=r30_15、P8_12=r30_14、P8_15=r31_15、
  P8_16=r31_14，全 mode 6；**输出/输入是不同的球，无同球双向**

mode 5 dtb + R30 固件的症状：固件活、邮箱通、padconf 寄存器值
"正确"（0x0d = pull-disable|mode5——dts 要什么它就是什么，但 dts
要错了），目标永远沉默（"cannot read IDR"）——时钟根本没接到球上。

### 2. clpru 寄存器写法

```c
/* GCC 式绑定：clpru 2.3.3 静默忽略（编过、零 r30 引用、纯 no-op！） */
register uint32_t __R30 __asm("r30");          // ✗ 陷阱

/* TI 支持包惯用法：文件级 volatile register，靠名字绑定 */
volatile register uint32_t __R30;               // ✓ */
```

验证必须看反汇编：`dispru` 输出**大写** `R30`（grep 小写会误判 0
引用）。实验版验证：15 条 R30 指令（8 SET + 7 CLR bit14）。

### 3. 固件改动（分支上，md5 bb1eb67e，1760 B）

```c
#define SWCLK_R30 (1u << 14)
static inline void clk_low(void)  { __R30 &= ~SWCLK_R30; }
static inline void clk_high(void) { __R30 |= SWCLK_R30; }
```

SWDIO 路径（OE 翻转、DATAIN 采样）原样保留。main 初始化加
`__R30 |= SWCLK_R30`（停车高电平）。

### 4. 功能验证 ✅

MODE6 dtb（md5 55168378）+ R30 固件：
- 枚举：`SWD DPIDR 0x2ba01477`、`Examination succeed`
- 倾倒：1000/300 kHz 256 KiB md5 全对（424c5ecd）

### 5. 性能验证 ❌

prupoke 加忙轮询（`exec_cmd_busy`，分支上）后的干净数据：

| 指标 | stock | R30@iters=0 | R30@iters=1 |
|---|---:|---:|---:|
| idle 时钟 ns | 439 | 290 | 450 |
| 读事务 ns/时钟 | ~439 | **553** | **714** |
| 2000 kHz 倾倒 | 1.81s ✅ | **崩溃** | 慢于 stock |

## 失败机理

### iters=0 写位竞争（崩溃的直接原因）

stock 方案里 SWCLK 的 OCP 写隐性地给了 SWDIO OCP 写 ~300ns 的落地
时间。R30 后时钟边沿单周期直达，而 `write_bit` 的顺序是：

```
clk_low → SWDIO 写发出（OCP，~300ns 后才落到引脚）→ 半相(145ns) → clk_high
                                                        ↑ 时钟上升沿在数据落地前！
```

目标在上升沿采样到**旧的** SWDIO → 请求位错帧 → ack=7。倾倒表现为
跑到 ~word 525 处 "Failed to read memory"（前 524 字侥幸通过——
边缘时序，WAIT 重试之类的扰动一来自就崩）。

安全运行需 iters≥1（半相 ≥305ns 让 OCP 写落地）→ 714ns/时钟，反而
比 stock 的 439 慢 60%。

### 结构性亏损（就算解决竞争也不赚）

- 采样时钟（36/48）：OCP 读 ~300ns 阻塞顶上关键路径，R30 救不了
  → 553 vs stock 439（stock 的写流水在掩护读延迟）
- idle 时钟（12/48）：290 vs 439，小赚
- 加权：(36×553 + 12×290)/48 ≈ 482ns > 439ns。**净亏 ~10%**

## 回滚与现场状态

板子已恢复 stock 并复验（2000 kHz 枚举 + 倾倒 md5 ✓）：

| 资产 | 位置 | md5 |
|---|---|---|
| stock dtb | `/mnt/p1/dtbs/am335x-boneblack.dtb`（= `.dtb.swd7` 备份） | 316ab5b5 |
| v2 固件 | `/lib/firmware/pru-swd.bin`（= `pru-swd.v2.bin` 备份） | b43a8fcd |
| R30 dtb | 仅宿主 linux 树（未部署） | 55168378 |
| R30 固件 | 仅分支 pru-swd-r30 | bb1eb67e |

分支 `pru-swd-r30` 未提交改动：
- `firmware/pru-swd.c`：R30 版（头部注释还写着 "mode 5"，若续作改 6）
- `prupoke.c`：忙轮询计时 + GPCFG 打印（**这两个改动独立有价值**，
  计时修复尤其值得择机合入主线）
- linux 树 dts：GPMC_AD12 MODE6

## 教训清单

1. 换瓶颈假设前先测**写 vs 读**的 OCP 代价差——fire-and-forget 和
   全延迟阻塞是两个世界
2. 免 OCP 的时钟不是免费的午餐：它同时拿走了对 OCP 数据写的隐性
   建立时间掩护
3. `config-pin` 的 mode 编号不可信，查 TRM 或 beagleboard 生成 dts
4. clpru 的 `__asm("r30")` 静默 no-op，TI 惯用法 + dispru 大写验证
5. 单点提速要看**加权**账本（idle 快 1.5× × 采样慢 1.26× = 净亏），
   别只看最显眼的数字
