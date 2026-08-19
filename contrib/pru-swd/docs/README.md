# pruswd 项目文档

OpenOCD `pruswd` 适配器（BeagleBone AM335x PRU-ICSS 位 banging SWD）的完整
技术文档。中文撰写，面向维护者；上手使用请先读仓库根部的英文
[README.md](../README.md)，再按需深入本目录。

## 阅读顺序

| 文档 | 内容 | 读者 |
|---|---|---|
| [01-architecture.md](01-architecture.md) | 系统架构、内存映射、引脚、设计取舍 | 所有人，先读 |
| [02-mailbox-protocol.md](02-mailbox-protocol.md) | 邮箱协议：字表、命令集、v2 魔数握手 | 固件/驱动开发 |
| [03-swd-engine.md](03-swd-engine.md) | SWD 引擎：事务帧结构、两个致命坑、换向 | 固件开发 |
| [04-driver.md](04-driver.md) | OpenOCD 驱动：初始化、自旋轮询、批量读、速度映射 | 驱动开发 |
| [05-board-bringup.md](05-board-bringup.md) | 板级部署：内核/dts/gpiomem/固件/prupoke、板子怪癖 | 部署运维 |
| [06-timing.md](06-timing.md) | 时钟模型：OCP 写流水 vs 读阻塞、标定方法、实测表 | 性能调优 |
| [07-r30-experiment.md](07-r30-experiment.md) | R30 SWCLK 实验全记录（负结果，已回滚） | 想继续提速的人 |
| [08-hardware-roadmap.md](08-hardware-roadmap.md) | 硬件改线路线图：电阻弱驱全本地 SWDIO | 下一步升级 |

## 项目状态（2026-08-19）

**主线（分支 `pru-swd`，全部已验证并推送 fork）**

| commit | 内容 |
|---|---|
| ef5d96ef4 | 驱动主体 pruswd |
| 71b91c111 | contrib：固件、gpiomem 模块、prupoke、文档 |
| 2ad54744e | Makefile 波浪线修复 |
| 9a174130c | 写事务 46 时钟帧结构修复（真目标暴露） |
| 898f5ddde | 邮箱自旋轮询 + 延迟重标定（36s→3.2s） |
| 31e9ad934 | 固件批量读 + 诚实速度映射（→1.81s） |

性能终点：256 KiB STM32F401 flash 倾倒 **1.81 s @2.28 MHz 实测**
（iters=0，OCP 节拍天花板），各档 md5 一致（424c5ecd）。

**实验线（分支 `pru-swd-r30`，未提交）**

R30 驱动 SWCLK 的混合方案。功能可行（枚举 + md5 全对），性能无增益
（详见 07），板子已回滚 stock。分支保留固件/prupoke 实验改动作参考。

**下一步（未实施）**

硬件改线（1 kΩ 弱驱 + R30/R31 全本地 SWDIO），预估 4–8 MHz、
256 KiB 约 0.4–0.8 s，见 08。

## 术语约定

- **stock / 主线**：分支 `pru-swd` 上已提交的 GPIO-over-OCP 双线方案
- **R30 实验**：分支 `pru-swd-r30` 上的 SWCLK 走 R30 混合方案
- **半相位 (half phase)**：SWCLK 半个周期，对应固件 `half_phase()`
- **iters**：邮箱 word 20 的延迟迭代数（半相位节拍粒度）
- 板子：BeagleBone Black（AM3358，BBB），192.168.5.1
- 目标：STM32F401CCU6（Cortex-M4，SW-DP）
