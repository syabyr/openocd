# 05 — 板级部署（BBB + 自编内核 6.12.94）

从零到 `openocd -f ... -c init` 成功的完整清单。板子：BeagleBone Black
（AM3358），自编内核 + OpenWrt 风格 musl 用户态，IP 192.168.5.1。

## 部件清单

| 部件 | 去向 | 校验 |
|---|---|---|
| `openocd` 二进制 | `/usr/bin/openocd` | 板上跑 `-v` |
| `pru-swd.bin` | `/lib/firmware/pru-swd.bin` | md5（v2 = b43a8fcd，v3 = 6f770311） |
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

## 设备树（V3 硬改方案）

硬改接线（[08](08-hardware-roadmap.md)）后引脚全换 PRU 本地功能：

```dts
pru_swd_pins: pru-swd-pins {
	pinctrl-single,pins = <
		AM33XX_PADCONF(AM335X_PIN_GPMC_AD13, PIN_OUTPUT, MUX_MODE6)	/* gpmc_ad13.pru0_r30_15 P8_11 SWDIO 驱动（串 1k） */
		AM33XX_PADCONF(AM335X_PIN_GPMC_AD12, PIN_OUTPUT, MUX_MODE6)	/* gpmc_ad12.pru0_r30_14 P8_12 SWCLK */
		AM33XX_PADCONF(AM335X_PIN_GPMC_AD15, PIN_INPUT_PULLUP, MUX_MODE6)	/* gpmc_ad15.pru0_r31_15 P8_15 SWDIO 采样 */
		AM33XX_PADCONF(AM335X_PIN_GPMC_WEN, PIN_INPUT_PULLUP, MUX_MODE7)	/* gpmc_wen.gpio1_29 P8_26 nRST */
	>;
};
```

注意 GPMC 球的 pruout/pruin 是 **MUX_MODE6**（不是 5；config-pin
工具给的序号是位置序号，别信，信 BeagleBoard 的 cape 矩阵表）。

验证（V3 期望值）：

```sh
grep -E '44e10(830|834|83c|898) ' /sys/kernel/debug/pinctrl/44e10800.pinmux-pinctrl-single/pins
# 0x830(P8_12 SWCLK)=0x0e  0x834(P8_11 SWDIO-o)=0x0e
# 0x83c(P8_15 SWDIO-i)=0x36  0x898(P8_26 nRST)=0x37
```

**V3 电路上绝不能拉 srst**：老方案的 nRST 脚 P8_15 现在是 SWDIO
采样输入，v2 固件会把它驱动到 nRST 电平、直接把 SWDIO 节点顶死。
stm32f2x.cfg 里带 `reset_config srst_nogate`，命令行必须
`-c "reset_config none"` 覆盖（nRST 只在 `adapter assert/reset`
时经 GPIO1_29/P8_26 走，openocd 不主动碰它）。

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
./prupoke dio                      # SWDIO 节点驱高/驱低读回自检（v3 固件）
./prupoke parklow                  # SWDIO 停靠弱低，配万用表排查接线
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

**DRAM 延迟字坑**：PRU DRAM 内容跨 PRU 复位存活，冷启动后是垃圾。
`MBOX_DELAY`（w20）只有计时模式和 openocd 驱动会写；老版 prupoke 的
dio/parklow 不写 → 继承到 2.37 亿次迭代的垃圾延迟 → SIG_IDLE 超时
"PRU did not complete"。现行版装载固件后统一先写 `w20=2` 兜底。
症状识别：握手（CMD_GPIO_IN）正常但任何走 half_phase 的命令超时。

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
| ssh 突然 pubkey 全拒 | ext4 错误态/元数据打架导致 dropbear 读不了 authorized_keys；**断电重启**让 journal 重放，别在挂载态跑 fsck |
| scp 报 Read-only file system | 突然断电/硬重启把 ext4 弄出错误位 → 内核按 ro 挂载；先 `e2fsck -p`（**必须先降级到 ro 或救援环境**）再 remount,rw，修完老实重启 |

## 目标连接与冒烟测试

STM32F401（SW-DP）接线：

| BBB | 目标 | 说明 |
|---|---|---|
| P8_12 | SWCLK | 直连 |
| P8_11 | SWDIO | **经 1kΩ 电阻**串入节点（V3；stock 方案直连） |
| P8_15 | SWDIO 节点 | V3 采样脚，直连目标侧节点（不串电阻） |
| P8_26 | nRST | V3 的复位脚（stock 方案用 P8_15） |
| P9_1 | GND | 共地必须 |
| P9_3/P9_4 | 3V3 | 目标自供电时不用 |

**数好目标侧的脚**：STM32F401 的 SWDIO 是 PA13、SWCLK 是 PA14。
bring-up 时真实踩过的坑——SWDIO 误插 PA15（JTAG TDI）：线一样通、
dio 自检一样过（主机侧无辜），但 DP 全沉默。两种沉默签名可鉴别：
v3 固件 ack=**000**（节点跟主机走，目标没应答）、v2 固件 ack=**7**
（三态读全 1）。排查看 [07](07-r30-experiment.md) 的判别树思路：
dio 过 + 枚举挂 = 目标侧问题（线序/供电/目标死），与主机侧无关。

板上 cfg（`adapter speed` 必须在 `source target/...` **之后**，目标
cfg 自带 speed 会覆盖）：

```tcl
source [find interface/pruswd.cfg]
source [find target/stm32f2x.cfg]
adapter speed 1000
```

```sh
openocd -s /usr/share/openocd/scripts -f /root/stm32.cfg \
    -c "reset_config none" -c "init; targets; shutdown"
# 期待: SWD DPIDR 0x2ba01477 / Examination succeed
# V3 电路 reset_config none 必须（见上文 srst 警告）

# 全量倾倒 + md5 门禁（基线 424c5ecd）
openocd -s /usr/share/openocd/scripts -f /root/stm32.cfg \
    -c "reset_config none" -c "adapter speed 5000" -c init \
    -c "dump_image /tmp/dump.bin 0x08000000 0x40000" -c shutdown
md5sum /tmp/dump.bin    # V3@5000kHz → 0.547s / 468 KiB/s / 424c5ecd…
```
