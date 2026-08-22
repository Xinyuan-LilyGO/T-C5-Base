<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align="center">LilyGo T-C5-Base</h1>

<p align="center">
  <a href="README_CN.md">中文</a>
</p>

## Contents

- [Contents](#contents)
- [Product Description](#product-description)
- [Quick Start](#quick-start)
  - [PlatformIO Quick Start](#platformio-quick-start)
  - [Arduino IDE Quick Start](#arduino-ide-quick-start)
  - [ESP-IDF Quick Start](#esp-idf-quick-start)
- [Examples](#examples)
- [Hardware Notes](#hardware-notes)
  - [CAN Pins](#can-pins)
  - [RS485 Pins](#rs485-pins)
  - [LED Pin](#led-pin)
- [Wiring Notes](#wiring-notes)
- [Speed Summary](#speed-summary)
- [Electrical Parameters](#electrical-parameters)
- [LED Description](#led-description)
- [FAQ](#faq)
  - [Why not use the official PlatformIO `platform = espressif32`?](#why-not-use-the-official-platformio-platform--espressif32)
  - [Build reports that the `t-c5-base` board cannot be found](#build-reports-that-the-t-c5-base-board-cannot-be-found)
  - [Unable to upload or flash](#unable-to-upload-or-flash)
  - [CAN reports `bus_off`, `state=passive`, or `RX=0`](#can-reports-bus_off-statepassive-or-rx0)
  - [Can CAN go above 1 Mbit/s?](#can-can-go-above-1-mbits)
  - [Why is the RS485 maximum speed 5 Mbps?](#why-is-the-rs485-maximum-speed-5-mbps)
  - [RS485 has no data or persistent bad frames](#rs485-has-no-data-or-persistent-bad-frames)
  - [Boot log reports **psram_mspi: Failed to allocate dummy cacheline for PSRAM memory barrier!**](#boot-log-reports-psram_mspi-failed-to-allocate-dummy-cacheline-for-psram-memory-barrier)
- [Resources](#resources)
  - [Schematic](#schematic)
  - [PCB Dimensions](#pcb-dimensions)

## Product Description

This repository provides Arduino and ESP-IDF test examples for the LilyGo T-C5-Base. It covers dual CAN, dual RS485, CAN speed tests, RS485 speed tests, and Modbus RTU over RS485.

Main features:

- RISC-V MCU with 2.4 and 5 GHz dual-band Wi-Fi 6, Bluetooth 5 (LE), and IEEE 802.15.4 (Zigbee, Thread)
- Non-isolated RS485 and CAN transceivers
- 7 to 40 V DC input through an XT30 connector
- Stackable expansion design, supporting up to RS485 x2 and CAN x2 at the same time
- Reserved PCB connector for a 0.96 inch OLED display
- Onboard BOOT and RST buttons
- 2 x 12 pin 2.54 mm headers
- CAN up to 1 Mbps
- RS485 up to 5 Mbps
- Removable termination resistor

## Quick Start

### PlatformIO Quick Start

> [!IMPORTANT]
>
> Do not use the official PlatformIO `platform = espressif32`. Common official PlatformIO Espressif32 releases do not correctly support the ESP32-C5 Arduino environment used by this project. Use the **pioarduino** Espressif32 platform instead.
>

1. Install [Visual Studio Code](https://code.visualstudio.com/) and the **pioarduino** extension.
2. Follow the current [pioarduino](https://github.com/pioarduino/pioarduino-vscode-ide) instructions to install the pioarduino Espressif32 platform.
3. Make sure `platformio.ini` uses the pioarduino platform and enables this repository's local board definition:

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

4. Change `src_dir` in `platformio.ini` to select the example you want to build.
5. Build:

   ```bash
   pio run
   ```

6. Upload:

   ```bash
   pio run -t upload --upload-port COM76
   ```

7. Open the serial monitor:

   ```bash
   pio device monitor -p COM76 -b 115200
   ```

### Arduino IDE Quick Start

1. Install [Arduino IDE](https://www.arduino.cc/en/software).
2. Install [ESP32 Arduino core](https://docs.espressif.com/projects/arduino-esp32/en/latest/) with ESP32-C5 support.
3. Select the `ESP32C5 Dev Module` board, and set the other options according to the table below.

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
    | PSRAM                                | CAN and basic examples can use Enabled. Dual RS485 speed tests are recommended to use Disabled. |
    | Upload Speed                         | 921600                                               |

4. Open any `.ino` file under `examples/`.
5. Select the board serial port.
6. Compile and upload.

Build an example with `arduino-cli`:

```bash
arduino-cli compile -b esp32:esp32:esp32c5:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled examples/can_basic_echo/can_basic_echo.ino
```

> [!IMPORTANT]
>
> 1. Dual RS485 tests need USB CDC for logs because UART0/Serial0 is used by RS4851. `ARDUINO_USB_CDC_ON_BOOT=1` is already enabled in `board/t-c5-base.json`.
>
> 2. If you use only **T-C5-Base**, only the **blink** and **wifi_scan** examples in this repository are usable. CAN and RS485 examples require the matching Shield.
>

### ESP-IDF Quick Start

ESP-IDF examples are located in `idf-examples/`. The ESP-IDF version used by this project is:

- Branch: `release/v6.1`
- `ESP-IDF v6.1-dev-6068-g6a9c44fe7e`
- Git commit: `6a9c44fe7e`
- Test target: `esp32c5`
- If this is your first time using ESP-IDF, see the [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32c5/index.html).

Build on WSL/Linux:

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

If the ESP-IDF environment can access the serial port directly, flash and monitor with:

```bash
idf.py -p COM76 flash monitor
```

If WSL cannot open the Windows `COMx` port directly, build with `idf.py build` in WSL first, then flash from Windows PowerShell with `esptool.exe`:

```powershell
esptool.exe --chip esp32c5 -p COM76 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x2000 .\build\bootloader\bootloader.bin 0x8000 .\build\partition_table\partition-table.bin 0x10000 .\build\idf_can_dual_basic_echo.bin
```

The default serial monitor baud rate is `115200`.

## Examples

Arduino examples:

| Example                                 | Type      | Description                                                                                                                        |
| --------------------------------------- | --------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `examples/blink`                        | Basic     | GPIO8 green LED blinking example. CAN/RS485 Shield is not required.                                                                |
| `examples/wifi_scan`                    | Basic     | Wi-Fi scan example. CAN/RS485 Shield is not required.                                                                              |
| `examples/can_basic_echo`               | CAN       | Single CAN echo. Select CAN1 or CAN2 with `ACTIVE_CAN`; received frames are sent back unchanged. Requires two boards or another CAN node for ACK. |
| `examples/can_dual_basic_echo`          | CAN       | Single-board dual CAN echo-back. CAN1 sends to CAN2, then CAN2 echoes back to CAN1. Supports OLED PASS/FAIL display, and GPIO8 LED blinks for 300 ms when the test passes. |
| `examples/can_speed_test`               | CAN       | Single CAN speed test. Normal mode requires two boards or another CAN node.                                                        |
| `examples/can_dual_speed_test`          | CAN       | Dual CAN mutual speed test. Connect CAN1/CAN2 on the same board to the same CAN bus for mutual TX/RX. Supports OLED status display. |
| `examples/can_advanced`                 | CAN       | Dual CAN forwarding and diagnostics example, including filtering and bus-off recovery logic.                                       |
| `examples/rs485_basic_echo`             | RS485     | Single RS485 echo. Select RS4851 or RS4852 with `ACTIVE_RS485`; serial monitor input is sent to the RS485 bus, and bus data is printed back to serial. |
| `examples/rs485_dual_basic_echo`        | RS485     | Single-board dual RS485 echo-back. RS4851 sends to RS4852, then RS4852 echoes back to RS4851. Supports OLED PASS/FAIL display, and GPIO8 LED blinks for 300 ms when the test passes. |
| `examples/rs485_speed_test`             | RS485     | Single RS485 speed test.                                                                                                          |
| `examples/rs485_dual_speed_test`        | RS485     | Dual RS485 mutual speed test using Arduino `HardwareSerial` and manual DE control.                                                |
| `examples/rs485_dual_driver_speed_test` | RS485     | Dual RS485 Arduino API speed test, used to verify the stable speed of a pure Arduino implementation.                              |
| `examples/rs485_modbus_module`          | RS485     | Modbus RTU host example. The default function code is `0x03`.                                                                      |
| `examples/can_rs485_dual_echo_back`     | Combined  | Single-board dual CAN plus dual RS485 echo-back test, used to verify CAN/RS485 Shield operation at the same time.                 |

> [!NOTE]
>
> CAN and RS485 examples require the matching Shield. `can_dual_basic_echo`, `can_dual_speed_test`, and `can_rs485_dual_echo_back` need CAN1_H/CAN2_H and CAN1_L/CAN2_L connected to the same CAN bus, with the 120R resistor jumper installed.
>
> `rs485_dual_basic_echo`, `rs485_dual_speed_test`, and `rs485_dual_driver_speed_test` need RS4851_A/RS4852_A and RS4851_B/RS4852_B connected together. For high-speed or long-cable tests, install the 120R termination resistor jumper.

- For more ESP32-C5 examples, see [arduino-esp32-libraries](https://github.com/espressif/arduino-esp32/tree/master/libraries).

ESP-IDF examples:

| Example                                  | Type  | Description                                                                                                      |
| ---------------------------------------- | ----- | ---------------------------------------------------------------------------------------------------------------- |
| `idf-examples/idf_can_dual_basic_echo`   | CAN   | ESP-IDF dual CAN echo-back example. CAN1 sends to CAN2, then CAN2 echoes back to CAN1. Default `1 Mbit/s`, pure ESP-IDF TWAI on-chip API. |
| `idf-examples/idf_rs485_dual_speed_test` | RS485 | ESP-IDF dual RS485 speed test using the ESP-IDF UART RS485 driver. Tested stable at `5 Mbps` with 25 m cable and 120R termination. |

> [!NOTE]
>
> `idf_can_dual_basic_echo` was tested with `ESP-IDF v6.1-dev-6940-g08e0d30a74`, target chip `esp32c5`, and COM76. During an 18-second run, 9 echo-back tests passed with `fail=0`, `busErr=0/0`, and `isrDrop=0/0`.

## Hardware Notes

### CAN Pins

| ESP32-C5 | SN65HVD231DR |
| -------- | ------------ |
| GPIO26   | CAN1_TX      |
| GPIO25   | CAN1_RX      |
| GPIO0    | CAN2_TX      |
| GPIO1    | CAN2_RX      |

### RS485 Pins

| ESP32-C5 | SP3485EEN        |
| -------- | ---------------- |
| GPIO4    | RS4851_RX        |
| GPIO5    | RS4851_TX        |
| GPIO27   | RS4851_DTR / RTS |
| GPIO24   | RS4852_RX        |
| GPIO23   | RS4852_TX        |
| GPIO7    | RS4852_DTR / RTS |

### LED Pin

| ESP32-C5 | LED   |
| -------- | ----- |
| GPIO8    | Green |

## Wiring Notes

- Normal CAN mode requires at least two CAN nodes. A single CAN node has no ACK and may report TX failure or bus-off.
- `can_dual_speed_test` uses CAN1 and CAN2 on the same board as two CAN nodes. Connect CAN1_H to CAN2_H and CAN1_L to CAN2_L on the same CAN bus.
- The CAN bus needs proper termination at the physical ends. A 120R termination resistor is recommended for normal tests, especially at 500 kbit/s and 1 Mbit/s.
- By default, the onboard 120R termination resistor is already shorted by the jumper cap.
- RS485 A/B must be connected with the correct polarity. If there is no data or bad frames persist, swap A/B and test again.
- RS485 wiring should consider common ground, termination, cable length, and baud rate according to the actual site.
- The GND pin on the CAN and RS485 3-pin connector usually does not need to be connected. Connect GND if shielding or grounding is required.
- GPIO15 is not available and is not routed to the 12-pin connector. Do not control GPIO15 in software, because GPIO15 is connected to the internal PSRAM. Controlling GPIO15 may cause a crash.

## Speed Summary

| Interface | Condition                                              | Highest stable result |
| --------- | ------------------------------------------------------ | --------------------- |
| CAN       | 25 m twisted pair cable, with 120R termination          | 1 Mbit/s              |
| CAN       | 20 cm Dupont wire, without 120R termination             | 125 kbit/s            |
| RS485     | 25 m cable, with 120R, Arduino-ESP32 4.0.0-alpha1       | 5 Mbps                |
| RS485     | 25 m cable, with 120R, ESP-IDF UART RS485 driver        | 5 Mbps                |

CAN has been confirmed at the standard Classic CAN rate of **1 Mbit/s**.

## Electrical Parameters

| Feature                  | Details |
| ------------------------ | ------- |
| USB-C input voltage      | 5V      |
| XT30 DC input range      | 7-40V   |
| 3.3V header output current capability | 500mA |

> [!IMPORTANT]
>
> 1. It is not recommended to draw 2A from the 5V interface for a long time. If long-term high load is required, provide proper heat dissipation.
>
> 2. It is not recommended to connect a 40V power supply to the XT30 input. Use less than 40V. If a 40V input supply is used, make sure it is a stable voltage source, because 40V is the maximum rating of the power chip.
>
> 3. The current capability of the 5V header depends on the connected power source when USB-C is used. When XT30 is used, the maximum is 2A.
>

## LED Description

| Description          | Color |
| -------------------- | ----- |
| 3.3V power indicator | Red   |
| 5V power indicator   | Red   |
| GPIO8 control        | Green |

## FAQ

### Why not use the official PlatformIO `platform = espressif32`?

Common official PlatformIO Espressif32 releases do not correctly support the ESP32-C5 Arduino environment used by this project. Use the pioarduino Espressif32 platform.

### Build reports that the `t-c5-base` board cannot be found

Make sure `platformio.ini` contains:

```ini
boards_dir = board
board = t-c5-base
```

The local board definition file is located at `board/t-c5-base.json`.

### Unable to upload or flash

Try entering download mode manually:

1. Hold BOOT.
2. Press RST briefly.
3. Release BOOT.
4. Run the upload command again.

Also make sure the upload port is correct, for example `COM76` on Windows.

### CAN reports `bus_off`, `state=passive`, or `RX=0`

Check that CAN-H/CAN-L are connected correctly. Make sure there are at least two CAN nodes on the bus and add 120R termination resistors at both ends. In testing, a 20 cm cable without 120R was only stable up to 125 kbit/s. With 120R, it reached 1 Mbit/s.

### Can CAN go above 1 Mbit/s?

The standard Classic CAN rate is usually up to 1 Mbit/s. ESP32-C5 supports a maximum CAN communication rate of 1 Mbps.

### Why is the RS485 maximum speed 5 Mbps?

ESP32-C5 supports a maximum communication rate of 5 Mbps.

### RS485 has no data or persistent bad frames

Check A/B polarity first, then common ground, termination resistor, baud rate, and whether the correct RS485 channel is enabled in the example.

### Boot log reports **psram_mspi: Failed to allocate dummy cacheline for PSRAM memory barrier!**

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

- See the official note: [arduino-esp32/issues/12587](https://github.com/espressif/arduino-esp32/issues/12587)

## Resources

### Schematic

- [T-C5-Base](./schematic/T-C5-Base.pdf)
- [T-C5-CAN-Shield](./schematic/T-C5-CAN-Shield.pdf)
- [T-C5-RS485-Shield](./schematic/T-C5-RS485-Shield.pdf)

### PCB Dimensions

- [T-C5-Base SVG](./dimensions/T-C5-Base.svg)
- [T-C5-CAN-Shield SVG](./dimensions/T-C5-CAN-Shield.svg)
- [T-C5-RS485-Shield SVG](./dimensions/T-C5-RS485-Shield.svg)
- [T-C5-Base DXF](./dimensions/T-C5-Base.dxf)
- [T-C5-CAN-Shield DXF](./dimensions/T-C5-CAN-Shield.dxf)
- [T-C5-RS485-Shield DXF](./dimensions/T-C5-RS485-Shield.dxf)
