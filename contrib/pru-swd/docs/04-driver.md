# 04 — OpenOCD 驱动

`src/jtag/drivers/pru-swd.c`，纯 SWD transport（`transport_ids =
TRANSPORT_SWD`），实现 `swd_driver` 五个回调 + 适配器生命周期。

## 生命周期

```
init:
  open /dev/gpiomem
  mmap PRUSS(256K) + GPIO1(4K)
  快照 GPIO1 OE/DATAOUT（退出还原）
  load_firmware:
    CTRL=0 复位保持
    IRAM ← pru-swd.bin 逐字直写
    MBOX_MAGIC = 0            ← 清魔数，防陈旧 DRAM 冒充 v2
    CTRL = SOFT_RST_N|EN      ← PC 从 0 启动
  DELAY=100（保守初始，等 adapter speed 设置）
  握手：CMD_GPIO_IN 一条哑命令证明 PRU 活着
  fw_block_read = (MBOX_MAGIC == "SWD2")?

quit:
  HALT(0x08) → 固件 __halt()，不再碰引脚
  CTRL=0 关 PRU
  还原 GPIO1 OE/DATAOUT 的三个 SWD 位
```

Tcl 命令：`pruswd firmware <path>`（默认 `/lib/firmware/pru-swd.bin`）。

## pru_swd_exec：两级轮询

```c
自旋 PRU_EXEC_SPIN=50000 次（每次 ~150ns，覆盖 ~7.5ms）
  → 未完成则 usleep(100) 轮询，直到 timeout_ms
```

**自旋不是装饰**：一条寄存器事务只有几十个 SWCLK（几十 µs）。若每条
命令固定先 usleep(100)，每字平白多 ~100 µs，批量读吞吐直接砍半
（这是 36s→3.2s 那次优化的一半来源，commit 898f5ddde）。超时兜底
打印的提示包含 PRUSS 电源态 sysfs 路径（别背，复制粘贴用）。

## 批量读（fw v2 路径）

OpenOCD 的 mem-ap 块读（`flash read_bank` 等）体现为**长串相同 AP
DRW 读**（TAR 自增）。驱动把它们攒起来一条命令重放：

```c
pru_swd_read_reg(cmd=AP_DRW 读):
  条件: fw_block_read && APNDP && A32==DRW
    ├─ 攒入 pend_value[]（目标指针）
    ├─ 攒满 1024 或 cmd8 变化 → flush
    └─ return（不产生任何 mailbox 流量）
  否则: flush 后走单字路径

flush:
  DATA=count; exec(READ_BLOCK | cmd8<<8 | delay<<24)
  done = RESULT1            ← 固件可能提前中止
  for i < done: *pend_value[i] = pruss_map[0x400/4 + i]
  ack/奇偶错 → 置 queued_retval
```

细节：
- **flush 必须先于任何其它邮箱写**（它自己要写 DATA 字）——所有入口
  （idle/switch_seq/write_reg/run_queue）第一件事都是 flush
- 中止后缓冲对前 `done` 个字仍有效，host 照抄，错误码另报
- 攒批上限 1024 = 驱动自限（DRAM 缓冲物理上到 1792 字，留裕量）

## 速度映射（pru_swd_speed）

```
period_ns = 1e6 / khz
iters     = period_ns <= 550 ? 0 : (period_ns - 222) / 190
MBOX_DELAY = iters
```

| 常量 | 值 | 含义 |
|---|---|---|
| `PRU_PHASE_FIXED_NS` | 222 | 半相位固定开销（指令+DRAM 读）折算到周期 |
| `PRU_DELAY_NS_PER_ITER` | 190 | 每延迟迭代的周期增量（实测 160–190，取上界保守） |
| `PRU_OCP_FLOOR_NS` | 550 | ≤此周期直接 iters=0（OCP 写流水地板 ~440ns/时钟） |
| `PRU_MAX_KHZ` | 2000 | 驱动钳位（超过也只会跑 OCP 地板） |

实测档位表（256 KiB 倾倒全部 md5 校验一致）：

| adapter speed | iters | 实测线速 | 256 KiB 倾倒 |
|---:|---:|---:|---:|
| 2000 kHz | 0 | ~2.3 MHz | 1.81 s |
| 1800 kHz | 1 | ~1.9 MHz | 2.3 s |
| 1000 kHz | 4 | ~1.0 MHz | 3.7 s |
| 500 kHz | 9 | ~0.55 MHz | 6.2 s |
| 300 kHz | 16 | ~0.35 MHz | 9.5 s |

档位是**离散步进**（iters 整数），实速可略超请求（≤~15%），方向是
"不低于请求"偏快而非偏慢——标定见 [06](06-timing.md)。

## SWD 回调到邮箱的映射

| swd_driver 回调 | 邮箱动作 |
|---|---|
| `switch_seq` | 位模式 → 字 1..8，SIG_GEN（≤256 bit 上限来自邮箱参数区） |
| `read_reg` | DRW 读攒批（上）；其余 READ_REG 单字，WAIT→清 ABORT 重试 128 次 |
| `write_reg` | DATA=值，`[23:16]`=奇偶，WRITE_REG |
| `run` | flush + SIG_IDLE(8)（队列尾保停车拍） |
| `init` | 生命周期见上 |

srst：`reset(srst)` / `JTAG_RESET` 命令 → `GPIO_OUT(mask=1<<15,
level=!srst)`（P8_15 低有效）。trst 不存在（SWD 无此线）。

## 构建要点

```sh
./bootstrap
./configure --enable-pruswd --enable-internal-jimtcl \
            --host=arm-openwrt-linux-muslgnueabi --build=<host> \
            PKG_CONFIG=false
make
```

- `--enable-internal-jimtcl`：交叉环境没有宿主 jimtcl 时必须
- `PKG_CONFIG=false`：否则 configure 探到**宿主** libusb 头文件，
  链接目标程序时炸
- strip 时注意 PATH 前缀 + `STAGING_DIR` 要对整个复合命令 export
  （zsh 里 `A=x make` 的 env 前缀不跨命令存活）
