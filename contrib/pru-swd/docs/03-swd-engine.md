# 03 — SWD 引擎与帧结构

固件 `pru-swd.c` 的 SWD 部分逐时钟实现协议。本章是帧结构、换向机制、
以及两个只有真目标才能暴露的致命坑。

## 基元

一个 SWCLK 时钟 = 下降沿 + 半相位等待 + 上升沿（**目标在上升沿采样**）
+ 半相位等待。半相位等待 = `half_phase()`：读邮箱 word 20 的迭代数，
循环烧掉（节奏由 host 的 `adapter speed` 控制，见 [06](06-timing.md)）。

| 基元 | 动作 |
|---|---|
| `write_bit(b)` | clk↓ → SWDIO 驱 b（OCP 写）→ 半相 → clk↑ → 半相 |
| `read_bit()` | clk↓ → 半相 → **采样 SWDIO（OCP 读 DATAIN）** → clk↑ → 半相 |
| `trn_input()` | clk↓ → SWDIO OE 置输入（OCP 写）→ 半相 → clk↑ → 半相 |
| `sig_idle_cycles(n)` | n 个纯时钟，SWDIO 驱低 |

SWDIO 换向 = 翻 GPIO1 OE 位（软件三态）：host 驱动阶段 OE=输出，
目标驱动阶段（ack/读数据）OE=输入。这是把 SWDIO 放在 GPIO1 上的
根本原因（PRU 本地脚无法三态，见 [01](01-architecture.md)）。

## 读事务（READ_REG）

```
SWDIO:  ──低── [8 bit 请求 LSB 先行] ──TRN── [3 bit ACK] [32 bit 数据] [奇偶] ──TRN──
host 驱动:      ▲________________________▲                                         
目标驱动:                                 ▲____________________________________▲
时钟数:  2 idle + 8 + 1 + 3 + 32 + 1 + 1  = 48 时钟（事务本体 46）
```

结果打包：ack（3 bit）、32 bit 数据、奇偶、奇偶校验标志
（固件按数据重算偶校验比对）。

## 写事务（WRITE_REG）

```
SWDIO:  ──低── [8 bit 请求] ──TRN── [3 bit ACK] ──TRN── [32 bit 数据] [奇偶]
host 驱动:      ▲________________▲                ▲________________________▲
目标驱动:                         ▲______________▲
时钟数:  2 idle + 8 + 1 + 3 + 1 + 32 + 1        = 48 时钟（事务本体 46）
```

**第二个 TRN 是独立的不采样时钟**——这是坑 ② 的核心（下）。

## 坑 ① 事务间 idle 时钟 SWDIO 必须驱低

现象：连接 DP 失败，ack 恒为 7（=SWDIO 读全 1，目标沉默）。

机理：SWD 协议里 SWDIO 高电平 + 时钟 = START 位。若 idle 时钟期间
SWDIO 停靠在高电平，DP 会把其中一个 idle 时钟当成新事务的 START，
后续整条事务错位，目标从此装死。

修复：每条 read/write 事务前置 2 个 SWDIO 驱低的 idle 时钟；事务
尾部 SWDIO 停靠低电平（`dio_drive(0) + dio_set_output()`）。

## 坑 ② 写事务必须整 46 时钟，TRN 不可与数据位融合

现象：**前 1–2 个写事务 ack OK，之后全部 ack=7**。DPIDR 读正常
（说明位序/极性/时钟都对），只有写挂——真目标（STM32F401）才暴露，
逻辑分析仪模拟器上完全正常。

机理：写事务 ack 后的第二个 TRN（目标→host 换向）被固件与 WDATA
bit0 融合成一个时钟 → 事务 45 时钟。SWD 里 TRN 是**不被采样的时钟**，
但 DP 内部状态机按固定节拍走：少一个时钟，DP 把下一事务的 START 位
吃作缺失的 TRN 节拍，请求整体错位一比特 → 从第二个写事务起全崩。
第一个写事务幸存是因为错位发生在其后的边界上。

修复：TRN 独立成拍：clk↓ → 驱 WDATA bit0 + OE 置输出 → 半相 →
clk↑（TRN 时钟）→ 半相 → 再进 32 bit 循环。

> 排障启发：**ack=7 优先查事务长度/对齐（时钟数、idle 电平、TRN），
> 其次才是位序极性**。位序错的话 DPIDR 都读不出来。

## WAIT / FAULT 处理

- 固件批量读：每字 WAIT 重试 8 次（事务间自然重发）；FAULT/奇偶错
  立即中止并报告已完成字数
- 驱动单字路径：WAIT 重试 128 次，每次先清 DP sticky error（写
  ABORT）；FAULT → `ERROR_SWD_FAULT`
- 读奇偶错：驱动按 `parity_u32()` 复核固件上报的数据，不一致按
  FAIL 处理（固件也算一份，双保险）

## 特殊序列（SIG_GEN）

line reset（≥50 个 1 + 0）、JTAG-to-SWD、dormant 进出等：host 把
位模式（≤256 bit，LSB 先行）拷进邮箱字 1..8，一条 SIG_GEN 逐位
`write_bit` 播出去。JTAG-to-SWD 后补 8 个 idle 时钟（协议要求）。

## 时钟电平细节

- 空闲态：SWCLK 停靠**高**（固件 main 初始化即拉高，事务内每拍自然
  回到高相位结束）
- SWDIO 空闲态停靠**低**（坑 ① 的要求）；但开机会先驱高一下再归低
  （dio_drive(1) 初始电平，避免上电毛刺当作 START——无碍，line reset
  会重新对齐）
