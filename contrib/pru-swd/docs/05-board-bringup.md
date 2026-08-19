# 05 — 板级部署（BBB + 自编内核 6.12.94）

从零到 `openocd -f ... -c init` 成功的完整清单。板子：BeagleBone Black
（AM3358），自编内核 + OpenWrt 风格 musl 用户态，IP 192.168.5.1。

## 部件清单

| 部件 | 去向 | 校验 |
|---|---|---|
| `openocd` 二进制 | `/usr/bin/openocd` | 板上跑 `-v` |
| `pru-swd.bin` | `/lib/firmware/pru-swd.bin` | md5（当前 v2 = b43a8fcd） |
| `gpiomem.ko` | `/lib/modules/6.12.94/gpiomem.ko`（**平铺目录**） | `/dev/gpiomem` 存在 |
| `am335x-boneblack.dtb` | FAT 启动分区 `/dtbs/`（mmcblk1p1） | debugfs pinmux |
| tcl 脚本 | 板上 `/usr/share/openocd/scripts`（要 `-s` 指定） | — |

## gpiomem 模块

```sh
cd contrib/pru-swd/gpiomem
make KDIR=/path/to/linux-tree TOOLCHAIN=.../arm-openwrt-linux-muslgnueabi-
install -m 0644 gpiomem.ko /lib/modules/$(uname -r)/gpiomem.ko
grep -q gpiomem /lib/modules/$(uname -r)/modules.dep \
    || echo "gpiomem.ko:" >> /lib/modules/$(uname -r)/modules.dep
echo gpiomem > /etc/modules.d/15-gpiomem   # 开机加载
```

白名单（mmap 放行区）：

| 基址 | 大小 | 内容 |
|---|---|---|
| 0x44e07000 | 4 KiB | GPIO0 |
| 0x4804c000 | 4 KiB | GPIO1（SWD 三脚） |
| 0x481ac000 | 4 KiB | GPIO2 |
| 0x481ae000 | 4 KiB | GPIO3 |
| 0x4a300000 | 256 KiB | **PRU-ICSS**（老模块没有这条！） |

busybox 的 modprobe 只认顶层平铺模块（不解析子目录），所以 .ko 直接
丢 `/lib/modules/<ver>/` 根并手工补 modules.dep。

## 设备树（stock 方案）

```dts
&am33xx_pinmux {
	pinctrl-0 = <&clkout2_pin &pru_swd_pins>;

	pru_swd_pins: pru-swd-pins {
		pinctrl-single,pins = <
			AM33XX_PADCONF(AM335X_PIN_GPMC_AD13, PIN_INPUT_PULLUP, MUX_MODE7)	/* P8_11 SWDIO */
			AM33XX_PADCONF(AM335X_PIN_GPMC_AD12, PIN_OUTPUT, MUX_MODE7)	/* P8_12 SWCLK */
			AM33XX_PADCONF(AM335X_PIN_GPMC_AD15, PIN_INPUT_PULLUP, MUX_MODE7)	/* P8_15 nRST  */
		>;
	};
};
```

编译（宿主交叉）：

```sh
cd linux-6.12.94
PATH=<toolchain>/bin:$PATH make ARCH=arm CROSS_COMPILE=arm-openwrt-linux- \
     ti/omap/am335x-boneblack.dtb
# 注意目标是 ti/omap/am335x-boneblack.dtb，写全路径时别把 ti/omap 重复拼
```

部署：

```sh
# 板上（FAT 分区不会自动挂）：
mount -t vfat /dev/mmcblk1p1 /mnt/p1
# 宿主（scp 必须 -O，新版 scp 默认 sftp 协议板上没有）：
scp -O arch/arm/boot/dts/ti/omap/am335x-boneblack.dtb root@192.168.5.1:/mnt/p1/dtbs/
ssh root@192.168.5.1 "sync && reboot"
```

验证 pinmux（板上无 devmem，走 debugfs）：

```sh
grep -E '^pin (12|13|15) ' /sys/kernel/debug/pinctrl/44e10800.pinmux-pinctrl-single/pinmux-pins
# 应显示 function pru-swd-pins
grep -E '44e108(30|34|3c)' /sys/kernel/debug/pinctrl/44e10800.pinmux-pinctrl-single/pins
# 0x834(P8_11)=0x37  0x830(P8_12)=0x0d  0x83c(P8_15)=0x37
```

padconf 寄存器解码：`[2:0]` mux mode；bit3 pull-disable；bit4 pull-up；
bit5 input-enable。0x37 = 输入使能+上拉+mode7；0x0d = pull-disable+mode7
（纯输出）。

> **pinctrl hog 的坑**：pinmux 活在 dtb 里。改了 dts 但板子行为没变 →
> 十有八九 bootloader 没加载新 dtb（确认 p1 里的才是启动用的那份）。
> TRM/U-Boot 的 mmc0 = Linux 的 mmc1（命名错位，见记忆库），BBB 的
> 启动 FAT 分区在 Linux 下是 **mmcblk1p1**。

## OpenOCD 交叉构建

见 [04](04-driver.md#构建要点)。SDK/autotools 环境注意
`STAGING_DIR` 指向 toolchain 目录，strip 用
`arm-openwrt-linux-muslgnueabi-strip`（PATH + STAGING_DIR 对整条
复合命令 export）。

## prupoke（板级自检）

```sh
cc -O2 -o prupoke prupoke.c        # 或交叉编译后 scp -O
./prupoke                          # 装载 + 邮箱握手
./prupoke t <iters> <clocks>       # 计时模式（无需目标）
```

健康输出判据：

```
after load: CTRL=00008003          ← SOFT_RST_N|EN
  PC=0000xx (来回跳)               ← 主轮询循环活着
counter after=1 mbox cmd=00000000  ← 命令被取走（回执）
result0 bit13/bit12 置位           ← SWDIO/SWCLK 都被驱高
final SYSCFG=0000000a              ← bit4=0：OCP 主端口已开
GPCFG0=0                           ← （R30 实验（分支）新增打印）直接 GPO 模式
```

计时模式陷阱（重要）：老版 prupoke 轮询用 usleep(1000)，**单命令计时
地板 ~1ms**——低速档（5000 时钟 × 439ns ≈ 2.2ms）没事，快时钟（如
R30 实验 iters=0，290ns × 5000 = 1.45ms）会被地板污染读成 440ns。
现行版计时已改忙轮询（`exec_cmd_busy`），不受地板影响；用老版时规则：
**总时长必须 ≥3× 地板**（加大 clocks）。

## 板子怪癖速查

| 现象 | 原因/解法 |
|---|---|
| scp 报 subsystem 错 | 加 `-O`（板上无 sftp） |
| 重启后 /tmp 全空 | tmpfs，cfg/二进制要重传 |
| /mnt/p1 不在 | FAT 分区不自动挂，手工 mount |
| 无 devmem/od/bc | busybox 精简版，用 debugfs/awk 替代 |
| modprobe 找不到模块 | 只认平铺目录，见上 |
| dtb 改了没生效 | bootloader 加载路径确认；`md5sum` 两边对 |
| remoteproc 占着 PRU0 | `cat /sys/class/remoteproc/remoteproc0/state` 必须 offline |

## 目标连接与冒烟测试

STM32F401（SW-DP）：SWDIO↔P8_11、SWCLK↔P8_12、GND↔P9_1、（可选
nRST↔P8_15）、目标自供电或 P9_3/P9_4 取 3V3。

板上 cfg（`adapter speed` 必须在 `source target/...` **之后**，目标
cfg 自带 speed 会覆盖）：

```tcl
source [find interface/pruswd.cfg]
source [find target/stm32f2x.cfg]
adapter speed 1000
```

```sh
openocd -f /tmp/stm32.cfg -c "init; targets; shutdown"
# 期待: SWD DPIDR 0x2ba01477 / Examination succeed

# 全量倾倒 + md5 门禁（基线 424c5ecd）
openocd -f /tmp/stm32.cfg -c init -c "reset halt" \
    -c "flash read_bank 0 /tmp/dump.bin 0 0x40000" -c shutdown
md5sum /tmp/dump.bin
```
