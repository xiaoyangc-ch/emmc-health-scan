# eMMC健康扫描 (eMMC Health Scan)

扫描局域网网段内的 Android 设备（网络 ADB），逐台读取 **eMMC 健康度**并汇总成表格。

Windows 桌面程序（Electron，基于 [liriliri/aya](https://github.com/liriliri/aya) 架构改编）+ 命令行扫描器 + Android 设备端检测工具，三件套全部开源。

## 功能

- 自动探测本机所在网段（可指定网段或 IP 列表），并行探测 adb 5555 端口
- 逐台读取 eMMC 健康字段（JEDEC Ext CSD）：
  - **预留块状态 pre_eol_info**（0x01 正常 / 0x02 警告 / 0x03 紧急）
  - **寿命估计 life_time_estimation A/B**
  - 容量、厂商、型号、CID/CSD、生产日期
- 汇总表格（深色 UI，结论红黄绿标识）+ 每台中文明细 + 汇总 CSV
- 可选「内存快测」：memtester 方式检测内存当前坏区/位翻转
- 内置 adb 与双架构设备端工具（armv7 / arm64），装完即用

## 为什么需要它

eMMC 是机顶盒、广告机、收银机这类设备的标配存储，寿命耗尽前芯片会通过
`pre_eol_info` 提前告警——但很多运维根本不知道这个字段的存在。本工具把"逐台登录
adb 手工查看"变成"一键扫全段、红黄绿一眼看出哪台该换"。

## Ext CSD 读取途径（按优先级）

1. `MMC_IOC_CMD` ioctl 发送 CMD8 读取 512 字节 Ext CSD（需 root）
2. debugfs `ext_csd`（裸 512 字节 / 文本转储 / 连续 hex 流三种格式自适应，老内核常见）
3. debugfs `health` 文本文件（部分 vendor 内核）

实测说明：多数 RK / Allwinner 盒子内核对 ioctl 返回 EINVAL，debugfs 兜底路径是实际可用通道。

## 使用

### Windows 桌面程序（推荐）

从 [Releases](../../releases) 下载 `eMMC健康扫描-x.y.z-win-x64.exe` 安装即可：

- 默认自动探测本机网段并扫描，也可指定网段（`192.168.1.1` 即扫 192.168.1.1~254）或 IP 列表
- 健康字段需要设备 root（依次尝试 `su 0` → `adb root`）；无 root 时仅展示 eMMC 身份信息

### 命令行扫描器

```sh
sh build.sh          # 构建设备端工具 + 控制台扫描器 + GUI
build/eMMC健康扫描.exe --subnet 192.168.1.1        # 或 --hosts a.b.c.d,e.f.g.h
```

### Android 设备端工具（可单独使用）

```sh
adb push device/memtest_v8a /data/local/tmp/memtest     # 内存快测
adb shell su 0 /data/local/tmp/memtest --mb 64 --json
adb push device/emmc_health_v8a /data/local/tmp/emmc_health
adb shell su 0 /data/local/tmp/emmc_health --json
```

## 构建

依赖：[zig](https://ziglang.org)（交叉编译）、.NET SDK 8（控制台）、Node 18+（GUI）。

```sh
sh build.sh            # 设备端单测 + 交叉编译 + ELF 校验 + 控制台构建
cd gui-app && npm install --legacy-peer-deps && npm run build && npm run pack
sh test/run-e2e.sh     # 宿主端到端测试（假 adb + 回环监听器）
```

## 目录结构

```
device/    Android 设备端工具（C，zig 交叉编译静态链接）
host/      Windows 控制台扫描器（C# / .NET 8，单文件发布）
gui-app/   Windows GUI（Electron + React + Luna 组件，基于 aya 架构改编）
test/      假 adb + 回环监听器端到端测试
build.sh   一键构建脚本
```

## 健康/测试字段含义

- `pre_eol_info`（预留块）：0x01 正常 / 0x02 警告（备用块已耗 80-90%）/ 0x03 紧急（>90%）
- `life_time_estimation A/B`：0x01=0-10% … 0x0A=90-100%，0x0B=超过设计寿命；
  0x00 = 芯片未实现该区域计数（正常现象），显示为「未定义」
- 内存快测通过 / 报错：检测的是当前坏区，不是剩余寿命（DRAM 无损耗机制）

## License

[AGPL-3.0](LICENSE)。GUI 部分基于 [liriliri/aya](https://github.com/liriliri/aya)
（AGPL-3.0）架构改编。
