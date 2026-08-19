# 01 — 系统架构

## 总览

```
┌──────────────────────────── AM335x (BBB) ────────────────────────────┐
│                                                                      │
│  OpenOCD (ARM Cortex-A8, Linux)           PRU0 (200 MHz, 固件)       │
│  ┌───────────────────────┐                ┌────────────────────────┐ │
│  │ adapter driver        │   邮箱(轮询)    │ SWD 位引擎             │ │
│  │   pru-swd.c           ◄───────────────►│ write_bit/read_bit     │ │
│  └─────────┬─────────────┘  PRU0 DRAM     │ 时钟相位/换向/奇偶     │ │
│            │                              └──────────┬─────────────┘ │
│            │ /dev/gpiomem (白名单 mmap)               │ OCP 主端口    │
│            │   固件装载: IRAM 直写                     ▼              │
│            │                              GPIO1 (0x4804C000)        │
└────────────┼──────────────────────────────────────────┼──────────────┘
             │                                          │
        P8_11 ┴ SWDIO ◄─────────────── SWDIO ───────────┘
        P8_12 ┴ SWCLK ───────────────► SWCLK
        P8_15 ┴ nRST  (可选)           nRST
        P9_1/2 ┴ GND                   GND
```

两核分工：**PRU0 负责全部线上时序**（ns 级相位、换向、采样），
**ARM 侧只做邮箱往返**（µs 级命令/结果交换）。SWD 时序对死区敏感，
Linux 用户态不可能直接位 banging（调度抖动 µs 级），PRU 是无操作系统
的实时协处理器，正合适。

## 内存映射

驱动与 prupoke 通过 `/dev/gpiomem`（见 [05](05-board-bringup.md)）映射
两个窗口：

| 区域 | 物理基址 | 大小 | 用途 |
|---|---|---|---|
| PRU-ICSS | `0x4A300000` | 256 KiB | 邮箱/控制寄存器/IRAM |
| GPIO1 | `0x4804C000` | 4 KiB | 引脚状态保存/还原 |

PRU-ICSS 窗口内部（host 视角偏移）：

| 偏移 | 名称 | 用途 |
|---|---|---|
| `0x0000` | PRU0 DRAM | 邮箱 + 批量读缓冲（8 KiB） |
| `0x22000` | PRU0 CTRL | `CTRL`（复位/使能）、`PC`（观测） |
| `0x26004` | CFG.SYSCFG | `STANDBY_INIT` 位：清 0 打开 PRU OCP 主端口 |
| `0x34000` | PRU0 IRAM | 固件正文（8 KiB，raw image 直写） |

PRU0 本地视角（固件代码视角）：DRAM 就在地址 0，GPIO1 通过 OCP 以
`0x4804C100`（SETDATAOUT/CLEARDATAOUT/OE 窗口）访问。

## 引脚

| BBB 头 | AM335x ball | GPIO | pinmux | SWD 信号 | 驱动方 |
|---|---|---|---|---|---|
| P8_11 | GPMC_AD13 | GPIO1_13 | mode 7 + 输入使能+上拉 | SWDIO | PRU over OCP（OE 翻转换向） |
| P8_12 | GPMC_AD12 | GPIO1_12 | mode 7 | SWCLK | PRU over OCP |
| P8_15 | GPMC_AD15 | GPIO1_15 | mode 7 + 输入使能+上拉 | nRST | PRU over OCP（可选） |

SWDIO 必须配 **接收器使能**（dts `PIN_INPUT_PULLUP`）：PRU 释放
SWDIO（OE 置输入）后要读回目标驱动的电平，接收器关了就永远读 1。

PRU 备选本地引脚（R30 输出 / R31 输入，均 **MUX_MODE6**）：

| BBB 头 | ball | PRU 功能 | 方向 |
|---|---|---|---|
| P8_11 | GPMC_AD13 | pru0_r30_15 | 输出 |
| P8_12 | GPMC_AD12 | pru0_r30_14 | 输出 |
| P8_15 | GPMC_AD15 | pru0_r31_15 | 输入 |
| P8_16 | GPMC_AD14 | pru0_r31_14 | 输入 |

注意 GPMC 球的 PRU 功能是 **mode 6**，不是 mcasp/lcd 球的 mode 5；
且输出/输入是**不同的球**，没有同球双向——这是 R30 实验与硬件路线
（[07](07-r30-experiment.md)/[08](08-hardware-roadmap.md)）的核心约束。

## 关键设计取舍

### 为什么是邮箱轮询，而不是原版的中断？

bbg-swd 原版：ARM 踢 INTC 事件 21 唤醒 SLP 的 PRU，PRU 完成后回踢
事件 19，依赖 `uio_pruss`/`libprussdrv`。本移植改为：PRU 死循环轮询
DRAM 命令字，完成后递增计数器；host 自旋读计数器（驱动
`PRU_EXEC_SPIN`）。收益：

- 不碰 INTC，与 remoteproc 内核栈共存（固件由 OpenOCD 直接写 IRAM，
  不走 `/lib/firmware` + remoteproc 启动流程）
- 无 libprussdrv 依赖，OpenWrt 这类小系统友好
- 代价：PRU 空转（无所谓，它没别的事）；host 轮询烧 CPU（只在事务
  窗口内自旋，随后退化为 usleep，见 [04](04-driver.md)）

### 为什么是 /dev/gpiomem 白名单模块，而不是 /dev/mem？

- OpenWrt/musl 环境无 /dev/mem（且不安全）
- 主线 gpio-memory 类模块通常只放行 GPIO bank；本模块额外放行
  PRUSS 窗口（固件装载必须），白名单见 [05](05-board-bringup.md)

### 为什么 SWDIO 留在 GPIO1 而不是全上 PRU 本地脚？

PRU 输出脚（R30 mux）**无法三态**，而 SWD 读换向必须释放 SWDIO。
GPIO 的 OE 寄存器是唯一的软件三态手段 → SWDIO 必须（目前）走 OCP。
详见 [07](07-r30-experiment.md) 的实测与 [08](08-hardware-roadmap.md)
的电阻弱驱方案（绕开该限制的硬件手段）。

## 血缘

NIIBE Yutaka 的 **bbg-swd**（Flying Stone Technology，2016，pasm 汇编
+ libprussdrv 补丁）→ 本移植：C 重写固件（clpru 2.3.3）、适配现行
OpenOCD adapter driver API、remoteproc 共存、批量读与速度标定、
真目标全验证。

## 前提条件

- AM335x 必须**带 PRU-ICSS**：AM3358/AM3356 有；AM3352（部分矿渣板）
  没有——`pruss_tm` 一使能就 external abort（efuse 决定，无解）
- PRU0 必须空闲：`/sys/class/remoteproc/remoteproc0/state` = offline
- 目标供电 3.3 V（引脚非 5 V 容忍、推挽）
