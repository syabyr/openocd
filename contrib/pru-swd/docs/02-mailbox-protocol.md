# 02 — 邮箱协议

host（OpenOCD/prupoke）与 PRU0 固件之间唯一的通信通道：PRU0 DRAM
（host 视角 = PRUSS 窗口偏移 0）里的一组 32 位小端字。无中断、无锁、
无内核参与——正确性完全靠**访问次序约定**。

## 字表

| 字 | 方向 | 名称 | 含义 |
|---|---|---|---|
| 0 | host→PRU | CMD | 命令字（见下）。PRU 取走后**立即写 0**＝回执 |
| 1..8 | host→PRU | DATA | 参数：写值 / idle 数 / SIG 位模式（最多 256 bit） |
| 16 | PRU→host | RESULT0 | 读：`奇偶<<31 \| ack`；写：`ack`；GPIO_IN：GPIO1 DATAIN 原始值；READ_BLOCK：`ack \| (奇偶错<<3)` |
| 17 | PRU→host | RESULT1 | 读：数据字；READ_BLOCK：完成的字数 |
| 18 | PRU→host | COUNTER | 完成计数器，每条命令执行完 +1。host 快照后等它变化 |
| 20 | host→PRU | DELAY | `half_phase()` 延迟迭代数（0 合法＝OCP 节拍地板）。固件默认 100 |
| 21 | PRU→host | MAGIC | 协议魔数。v2 固件启动时写 `0x53574432`（"SWD2"） |

**命令字位域**：`[7:0]` 命令码；`[15:8]` 8 bit SWD 请求（含
START/STOP/PARK）；`[23:16]` 写事务数据奇偶；`[31:24]` AP 访问后附加
idle 时钟数。

**批量读缓冲**：DRAM 字节 `0x400` 起（字 256 起），最多 1024 字。
0x400..0x1000 为缓冲区，DRAM 8 KiB 内富余（缓冲实际可容 1792 字，
驱动限 1024）。

## 命令集

| 码 | 命令 | 参数/结果 |
|---|---|---|
| 0x00 | HALT | 特殊：host 必须发 **0x08** 编码（见坑 ①），固件写结果、递增计数器后 `__halt()` |
| 0x01 | BLINK | 保留 no-op |
| 0x02 | GPIO_OUT | 字 1=mask，字 2=电平；同时强制 OE 输出（nRST 用） |
| 0x03 | GPIO_IN | RESULT0 = GPIO1 DATAIN（bring-up 观测用） |
| 0x04 | SIG_IDLE | 字 1=时钟数，SWDIO 驱低发 idle 时钟 |
| 0x05 | SIG_GEN | 字 0 `[15:8]`=位数（≤256），字 1..8=位模式 LSB 先行（line reset / JTAG-to-SWD 等特殊序列） |
| 0x06 | READ_REG | 一条完整 SWD 读事务（帧结构见 [03](03-swd-engine.md)），RESULT0/17 回 ack+数据+奇偶 |
| 0x07 | WRITE_REG | 一条完整 SWD 写事务，字 1=写值，`[23:16]`=奇偶 |
| 0x09 | READ_BLOCK | 字 1=次数 N（≤1024）：同一条读请求连发 N 次（TAR 自增），数据写 0x400；固件内 WAIT 重试 8 次/字 |

命令码由 `switch (w0 & 0xff)` **按完整 8 位**解码——0x08 与 0x00 是
两个 case，都进 HALT（坑 ①）。

## 访问次序（伪代码）

```
host 侧:                              PRU 侧:
参数字全部写好
CMD = 命令字        ──最后一次写──►    轮询 CMD != 0
                                      CMD = 0            (回执)
                                      执行
快照 COUNTER，自旋等变化 ◄────────────  COUNTER++
读 RESULT / 0x400 缓冲
```

规则：
1. **命令字最后写**——它是唯一的"发射"信号，参数必须先就位
2. PRU 清零命令字 ≠ 完成信号，只是"已受理"；完成只看计数器跳变
   （HALT 例外：`__halt()` 前就递增计数器）
3. READ_BLOCK 的参数字（word 1）会被固件读走，host 下一批前必须重写

## v2 魔数握手（批量读能力探测）

问题：host 重启 OpenOCD 不掉电，PRU DRAM 残留上一次会话的魔数，
新会话无法区分"固件真的支持批量读"和"陈旧 RAM 冒充"。

解法（协议级三方约定）：
1. host 装载固件前**先把 word 21 清零**
2. 固件启动后自己写 `0x53574432`
3. host 释放 PRU 复位、握手成功后读 word 21——只有"固件真的跑过
   main() 开头"才可能非零 → 置 `fw_block_read`，v1 固件自动退回单字
   路径

## 内存布局保护（链接脚本）

```
PRU0 DRAM:  [0x000 邮箱 0..0x57][0x100 .data/.bss/.stack][0x400 批量缓冲][...]
```

`am335x_pru0.cmd` 把 PAGE 1 限死在 `org=0x100, len=0x300`：固件数据段
一旦超过 0x400 边界，**链接期报错**而不是静默覆盖批量缓冲。同时
`.text:_c_int00* > 0x00000000` 把入口钉在 IRAM 0——PRU 复位后无条件
从 PC=0 取指（坑 ②）。

## 坑档案

**① HALT 必须发 0x08。** PRU 轮询条件是 `CMD != 0`，而 HALT 命令码
恰为 0——原样发送等于"无命令"。发 `0x08`（低 3 位仍命中 HALT case）。
固件对 0x00/0x08 双 case 兜底。

**② `--rom_model` 不保证入口在 0。** clpru 链接默认按链接脚本排布，
`_c_int00` 不一定落在地址 0；PRU 却固定 PC=0 启动。症状：PC 冻结、
邮箱永不响应、SYSCFG 的 STANDBY_INIT 保持置位。修复＝链接脚本显式
`.text:_c_int00* > 0x00000000`。

**③ OCP 主端口默认关着。** PRU 复位后 SYSCFG.STANDBY_INIT=1，OCP
主端口禁用，任何 GPIO 访问静默失败。固件 main() 第一件事清该位。
健康判据：prupoke 读 SYSCFG = 0x0a（bit4=0）。

**④ 批量读的 WAIT 重试在固件内。** 单字路径 host 侧重试 128 次，
批量路径 host 只发一条命令——固件内每字最多 8 次 WAIT 重试，仍 WAIT
或奇偶错则提前返回（RESULT1 = 已完成字数，缓冲对该字数有效）。
