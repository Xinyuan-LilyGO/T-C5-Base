<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align="center">LilyGo T-C5-Base</h1>

<p align="center">
  <a href="README.md">English</a>
</p>


## 目录

- [目录](#目录)
- [产品说明](#产品说明)
- [快速开始](#快速开始)
  - [PlatformIO 快速开始](#platformio-快速开始)
  - [Arduino IDE 快速开始](#arduino-ide-快速开始)
  - [ESP-IDF 快速开始](#esp-idf-快速开始)
- [示例列表](#示例列表)
- [硬件说明](#硬件说明)
  - [CAN 引脚](#can-引脚)
  - [RS485 引脚](#rs485-引脚)
  - [LED 引脚](#led-引脚)
- [接线注意](#接线注意)
- [测速摘要](#测速摘要)
- [电气参数](#电气参数)
- [LED 描述](#led-描述)
- [常见问题](#常见问题)
  - [为什么不能用官方 PlatformIO 的 `platform = espressif32`？](#为什么不能用官方-platformio-的-platform--espressif32)
  - [编译提示找不到 `t-c5-base` 开发板](#编译提示找不到-t-c5-base-开发板)
  - [无法下载/烧录](#无法下载烧录)
  - [CAN 出现 `bus_off`、`state=passive` 或 `RX=0`](#can-出现-bus_offstatepassive-或-rx0)
  - [CAN 能不能超过 1 Mbit/s？](#can-能不能超过-1-mbits)
  - [RS485 最高速率为什么只有 5 Mbps？](#rs485-最高速率为什么只有-5-mbps)
  - [RS485 没有数据或持续坏帧](#rs485-没有数据或持续坏帧)
  - [启动报错 **psram\_mspi: Failed to allocate dummy cacheline for PSRAM memory barrier!**](#启动报错-psram_mspi-failed-to-allocate-dummy-cacheline-for-psram-memory-barrier)
- [资源](#资源)
  - [原理图](#原理图)
  - [PCB 尺寸图](#pcb-尺寸图)

## 产品说明

这个仓库提供 LilyGo T-C5-Base 的 Arduino 和 ESP-IDF 测试示例，覆盖双路 CAN、双路 RS485、CAN 测速、RS485 测速和 Modbus RTU over RS485.

主要特性：

- 2.4&5 GHz 双频 Wi-Fi 6、Bluetooth 5 (LE) 和 IEEE 802.15.4 (Zigbee, Thread) 连接性能的 RISC-V MCU
- 非隔离型RS485和CAN收发器
- 支持 7-40 V 直流输入，使用 XT30 接口
- 最多支持 RS485x2 和 CANx2 扩展共存，堆叠设计
- PCB预留 0.96 寸 OLED 显示接口
- 板载 BOOT、RST 按键
- 2x12Pin 2.54 mm 排针
- CAN 最大 1Mbps速率
- RS485 最大 5Mbps 速率
- 可移除终端电阻

## 快速开始

### PlatformIO 快速开始

> [!IMPORTANT]
>
> 不要使用官方 PlatformIO 的 `platform = espressif32`.常见官方 PlatformIO Espressif32 发布版本不能正确支持 ESP32-C5 Arduino 环境.需要使用 **pioarduino** Espressif32 platform.
>

1. 安装 [Visual Studio Code](https://code.visualstudio.com/) 和 **pioarduino** 插件
2. 按 [pioarduino](https://github.com/pioarduino/pioarduino-vscode-ide) 当前说明安装 pioarduino Espressif32 platform.
3. 确认 `platformio.ini` 使用 pioarduino platform，并启用本仓库的本地板卡定义：

   ```ini
   [platformio]
   src_dir = examples/can_dual_speed_test
   boards_dir = board

   [env:t-c5-base]
   board = t-c5-base
   platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
   framework = arduino
   upload_speed = 921600
   monitor_speed = 115200
   ```

4. 通过修改 `platformio.ini` 里的 `src_dir` 选择要编译的示例.
5. 编译：

   ```bash
   pio run
   ```

6. 烧录：

   ```bash
   pio run -t upload --upload-port COM76
   ```

7. 查看串口日志：

   ```bash
   pio device monitor -p COM76 -b 115200
   ```

### Arduino IDE 快速开始

1. 安装 [Arduino IDE](https://www.arduino.cc/en/software)
2. 安装包含 ESP32-C5 支持的 [ESP32 Arduino core](https://docs.espressif.com/projects/arduino-esp32/en/latest/)
3. 选择 `ESP32C5 Dev Module`开发板,其他选项根据下方列表中进行选择

    | Name                                 | Value                                                |
    | ------------------------------------ | ---------------------------------------------------- |
    | Board                                | **ESP32C5 Dev Module**                               |
    | Port                                 | Your port                                            |
    | USB CDC On Boot                      | Enable                                               |
    | CPU Frequency                        | 240MHZ(WiFi)                                         |
    | Core Debug Level                     | None                                                 |
    | Erase All Flash Before Sketch Upload | Disable                                              |
    | Flash Mode                           | QIO                                                  |
    | Flash Frequency                      | 80MHz                                                |
    | Flash Size                           | **16MB(128Mb)**                                      |
    | Arduino Runs On                      | Core1                                                |
    | Partition Scheme                     | **16M Flash (3MB APP/9.9MB FATFS)**                  |
    | PSRAM                                | CAN/普通示例可 Enabled；双路 RS485 测速建议 Disabled |
    | Upload Speed                         | 921600                                               |

4. 打开 `examples/` 下任意 `.ino` 文件.
5. 选择板子的串口端口.
6. 编译并上传.

`arduino-cli` 编译示例：

```bash
arduino-cli compile -b esp32:esp32:esp32c5:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled examples/can_basic_echo/can_basic_echo.ino
```

> [!IMPORTANT]
>
> 1. 双路 RS485 测试需要 USB CDC 输出日志，因为 UART0/Serial0 被 RS4851 使用.本仓库的 `board/t-c5-base.json` 已开启 `ARDUINO_USB_CDC_ON_BOOT=1`.
>
> 2. 如果单独使用 **T-C5-Base**请注意这个仓库只有 **blink 和 wifi_scan** 示例可用,CAN 示例 和 RS485 示例需要搭配配套Shield使用
>
>

### ESP-IDF 快速开始

当前 ESP-IDF 示例位于 `idf-examples/`.本项目使用的 ESP-IDF 版本：

- 分支：`release/v6.1`
- `ESP-IDF v6.1-dev-6068-g6a9c44fe7e`
- Git commit: `6a9c44fe7e`
- 测试目标：`esp32c5`
- 如果你是第一次使用esp-idf，请查看 [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32c5/index.html)

在 WSL/Linux 中编译：

```bash
git clone --recursive -b release/v6.1 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c5
. ./export.sh
cd ..
git clone https://github.com/Xinyuan-LilyGO/T-C5-Base.git
cd T-C5-Base/idf-examples/idf_can_dual_basic_echo
idf.py set-target esp32c5
idf.py build
```

如果 ESP-IDF 环境可以直接访问串口，可以直接烧录并查看日志：

```bash
idf.py -p COM76 flash monitor
```

如果在 WSL 中无法直接打开 Windows 的 `COMx` 串口，可以先在 WSL 中完成 `idf.py build`，再在 Windows PowerShell 中使用 `esptool.exe` 烧录：

```powershell
esptool.exe --chip esp32c5 -p COM76 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x2000 .\build\bootloader\bootloader.bin 0x8000 .\build\partition_table\partition-table.bin 0x10000 .\build\idf_can_dual_basic_echo.bin
```

查看日志的默认串口波特率为 `115200`.

## 示例列表

Arduino 示例：

| 示例                                    | 类型     | 说明                                                                                                                             |
| --------------------------------------- | -------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `examples/blink`                        | 基础     | GPIO8 绿色 LED 闪烁示例，不需要 CAN/RS485 Shield                                                                                 |
| `examples/wifi_scan`                    | 基础     | Wi-Fi 扫描示例，不需要 CAN/RS485 Shield                                                                                          |
| `examples/can_basic_echo`               | CAN      | 单路 CAN echo.通过 `ACTIVE_CAN` 选择 CAN1 或 CAN2，收到帧后原样发回.需要两个板子或另一个 CAN 节点提供 ACK                        |
| `examples/can_dual_basic_echo`          | CAN      | 单板双路 CAN echo-back. CAN1 发送到 CAN2，CAN2 回传到 CAN1；支持 OLED 显示 PASS/FAIL，测试 OK 时 GPIO8 LED 闪烁 300 ms           |
| `examples/can_speed_test`               | CAN      | 单路 CAN 速率测试.普通模式需要两个板子或另一个 CAN 节点                                                                          |
| `examples/can_dual_speed_test`          | CAN      | 双路 CAN 互发测速.单板 CAN1/CAN2 接到同一条 CAN 总线后互相收发，支持 OLED 状态显示                                               |
| `examples/can_advanced`                 | CAN      | 双 CAN 转发和诊断示例，包含过滤和 bus-off 恢复逻辑                                                                               |
| `examples/rs485_basic_echo`             | RS485    | 单路 RS485 echo.通过 `ACTIVE_RS485` 选择 RS4851 或 RS4852，串口监视器输入发到 RS485，总线数据打印回串口                          |
| `examples/rs485_dual_basic_echo`        | RS485    | 单板双路 RS485 echo-back. RS4851 发送到 RS4852，RS4852 回传到 RS4851；支持 OLED 显示 PASS/FAIL，测试 OK 时 GPIO8 LED 闪烁 300 ms |
| `examples/rs485_speed_test`             | RS485    | 单路 RS485 速率测试.                                                                                                             |
| `examples/rs485_dual_speed_test`        | RS485    | 双路 RS485 互发测速，使用 Arduino `HardwareSerial` 和手动 DE 控制                                                                |
| `examples/rs485_dual_driver_speed_test` | RS485    | 双路 RS485 Arduino API 测速，用于验证纯 Arduino 写法的稳定速率                                                                   |
| `examples/rs485_modbus_module`          | RS485    | Modbus RTU 主机示例，默认功能码 `0x03`                                                                                           |
| `examples/can_rs485_dual_echo_back`     | 综合测试 | 单板同时测试双路 CAN 和双路 RS485 echo-back，用于验证 CAN/RS485 Shield 同时工作                                                  |

> [!NOTE]
>
> CAN 和 RS485 示例需要配套 Shield. `can_dual_basic_echo`、`can_dual_speed_test`、`can_rs485_dual_echo_back` 需要将 CAN1_H/CAN2_H、CAN1_L/CAN2_L 接到同一条 CAN 总线，并连接120R电阻跳线帽.
>
> `rs485_dual_basic_echo`、`rs485_dual_speed_test`、`rs485_dual_driver_speed_test` 需要将 RS4851_A/RS4852_A、RS4851_B/RS4852_B 对接.高速或长线测试，需要将120R终端电阻跳线帽连接

- 更多的esp32-c5示例请跳转 [arduino-esp32-libraries](https://github.com/espressif/arduino-esp32/tree/master/libraries)

ESP-IDF 示例：

| 示例                                       | 类型  | 说明                                                                                                                |
| ------------------------------------------ | ----- | ------------------------------------------------------------------------------------------------------------------- |
| `idf-examples/idf_can_dual_basic_echo`     | CAN   | ESP-IDF 双路 CAN echo-back 示例.CAN1 发送到 CAN2，CAN2 回传到 CAN1，默认 `1 Mbit/s`，纯 ESP-IDF TWAI on-chip API    |
| `idf-examples/idf_rs485_dual_speed_test`   | RS485 | ESP-IDF 双路 RS485 速率测试，使用 ESP-IDF UART RS485 driver，实测 25 m 线材加 120R 终端电阻可稳定到 `5 Mbps`        |

> [!NOTE]
>
> `idf_can_dual_basic_echo` 测试时使用 `ESP-IDF v6.1-dev-6940-g08e0d30a74`，目标芯片 `esp32c5`，COM76 烧录运行 18 秒内 9 次 echo-back 全部 OK，`fail=0`、`busErr=0/0`、`isrDrop=0/0`.

## 硬件说明

### CAN 引脚

| ESP32-C5 | SN65HVD231DR |
| -------- | ------------ |
| GPIO26   | CAN1_TX      |
| GPIO25   | CAN1_RX      |
| GPIO0    | CAN2_TX      |
| GPIO1    | CAN2_RX      |

### RS485 引脚

| ESP32-C5 | SP3485EEN        |
| -------- | ---------------- |
| GPIO4    | RS4851_RX        |
| GPIO5    | RS4851_TX        |
| GPIO27   | RS4851_DTR / RTS |
| GPIO24   | RS4852_RX        |
| GPIO23   | RS4852_TX        |
| GPIO7    | RS4852_DTR / RTS |

### LED 引脚

| ESP32-C5 | LED  |
| -------- | ---- |
| GPIO8    | 绿色 |

## 接线注意

- CAN 普通模式需要至少两个 CAN 节点.单个 CAN 节点没有 ACK，可能出现发送失败或 bus-off
- `can_dual_speed_test` 把同一块板上的 CAN1 和 CAN2 作为两个 CAN 节点使用.需要将 CAN1_H 与 CAN2_H、CAN1_L 与 CAN2_L 接到同一条 CAN 总线
- CAN 总线需要在物理两端做合适终端匹配.正常测试建议使用 120R 终端电阻，尤其是 500 kbit/s 和 1 Mbit/s
- 默认板载已经通过跳线帽短路了120R终端电阻
- RS485 A/B 必须接对.如果完全没有数据或持续坏帧，先交换 A/B 重新测试
- RS485 需要按现场情况考虑共地、终端电阻、线材长度和速率
- CAN 和 RS485 3P座子的 GND 一般可以不接,如果有屏蔽需求的可以接上GND
- GPIO15不可用,没有将GPIO15接到 12Pin 连接器上,软件上不要控制GPIO15,因为GPIO15已经连接了内部的PSRAM，如果控制GPIO15可能导致奔溃

## 测速摘要

| 接口  | 条件                                             | 最高稳定结果 |
| ----- | ------------------------------------------------ | ------------ |
| CAN   | 25 m 双绞线，已加 120R 终端电阻                  | 1 Mbit/s     |
| CAN   | 20 cm 杜邦线，无 120R 终端电阻                   | 125 kbit/s   |
| RS485 | 25 m 线材，已加 120R, Arduino-ESP32 4.0.0-alpha1 | 5 Mbps       |
| RS485 | 25 m 线材，已加 120R，ESP-IDF UART RS485 driver  | 5 Mbps       |

CAN 已按标准 Classic CAN 速率确认到 **1 Mbit/s**

## 电气参数

| 特点                   | 详情  |
| ---------------------- | ----- |
| 🔗USB-C 输入电压        | 5V    |
| 📍XT30 DC 输入范围      | 7-40V |
| 📍3.3V 排针电流输出能力 | 500mA |

> [!IMPORTANT]
>
> 1. 不推荐5V接口处长时间带负载2A,如果需要长时间带负载请做好散热处理
>
> 2. 不推荐XT30接口接入40V电源,推荐输入小于40V,如果使用40V输入电源,一定要使用稳定的电压源,因为40V是电源芯片的最大值
>
> 3. 5V 排针的电流带载能力当使用USB-C的时候取决于接入电源，当使用XT30接口时最大2A
>

## LED 描述

| 描述          | 颜色 |
| ------------- | ---- |
| 3.3V 电源指示 | 红色 |
| 5V 电源指示   | 红色 |
| GPIO8 控制    | 绿色 |

## 常见问题

### 为什么不能用官方 PlatformIO 的 `platform = espressif32`？

常见官方 PlatformIO Espressif32 发布版本不能正确支持本项目使用的 ESP32-C5 Arduino 环境.请使用 pioarduino Espressif32 platform.

### 编译提示找不到 `t-c5-base` 开发板

确认 `platformio.ini` 包含：

```ini
boards_dir = board
board = t-c5-base
```

本地开发板定义文件位于 `board/t-c5-base.json`

### 无法下载/烧录

可以尝试手动进入下载模式：

1. 按住 BOOT
2. 短按 RST
3. 松开 BOOT
4. 重新执行烧录命令

同时确认上传端口是否正确，例如 Windows 下的 `COM76`

### CAN 出现 `bus_off`、`state=passive` 或 `RX=0`

检查 CAN-H/CAN-L 是否接对，确认总线上至少有两个 CAN 节点，并在总线两端加 120R 终端电阻.实测中，20 cm 线材无 120R 只能稳定到 125 kbit/s，加 120R 后可到 1 Mbit/s.

### CAN 能不能超过 1 Mbit/s？

Classic CAN 标准速率通常最高为 1 Mbit/s. ESP32C5最高只支持1Mbps的通讯速率

### RS485 最高速率为什么只有 5 Mbps？

ESP32C5最高只支持5Mbps的通讯速率

### RS485 没有数据或持续坏帧

先检查 A/B 极性，再检查共地、终端电阻、波特率，以及示例里是否启用了正确的 RS485 通道.

### 启动报错 **psram_mspi: Failed to allocate dummy cacheline for PSRAM memory barrier!**

  ```sh
  ESP-ROM:esp32c5-eco3-20250704
  Build:Jul  4 2025
  rst:0x15 (USB_UART_HPSYS),boot:0x1e (SPI_FAST_FLASH_BOOT)
  SPI mode:DIO, clock div:1
  load:0x408556c0,len:0x1254
  load:0x4084bba0,len:0xc20
  load:0x4084e5a0,len:0x3270
  entry 0x4084bba0
  E (1023) psram_mspi: Failed to allocate dummy cacheline for PSRAM memory barrier!
  ```

- 请参见官方说明 [arduino-esp32/issues/12587](https://github.com/espressif/arduino-esp32/issues/12587)

## 资源

### 原理图

- [T-C5-Base](./schematic/T-C5-Base.pdf)
- [T-C5-CAN-Shield](./schematic/T-C5-CAN-Shield.pdf)
- [T-C5-RS485-Shield](./schematic/T-C5-RS485-Shield.pdf)

### PCB 尺寸图

- [T-C5-Base SVG](./dimensions/T-C5-Base.svg)
- [T-C5-CAN-Shield SVG](./dimensions/T-C5-CAN-Shield.svg)
- [T-C5-RS485-Shield SVG](./dimensions/T-C5-RS485-Shield.svg)
- [T-C5-Base DXF](./dimensions/T-C5-Base.dxf)
- [T-C5-CAN-Shield DXF](./dimensions/T-C5-CAN-Shield.dxf)
- [T-C5-RS485-Shield DXF](./dimensions/T-C5-RS485-Shield.dxf)
