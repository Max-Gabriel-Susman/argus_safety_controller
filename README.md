# Argus Neural Interface Bridge

The **Argus Neural Interface Bridge** is a **micro-ROS client** for the Argus Neural Interface MCU. It is intended to let the embedded device publish telemetry into the Argus ROS 2 graph. Future efforts will be towards safely and receive inbound commands from the host side.

This project currently targets the **Olimex STM32-E407** running the **Zephyr** RTOS.

## Overview

The development workflow uses three different connection modes on the STM32 board:

- **JTAG/SWD mode** for debugger-based flashing
- **USB OTG2 serial mode** for runtime communication with the micro-ROS Agent
- **USB OTG1 DFU mode** for USB bootloader flashing through the STM32 system bootloader

Because the board selects both its power source and boot mode with jumpers, the **`PWR_SEL`** and **boot jumper** settings must match the mode you are using.

## Roadmap

- [x] **Part B — SD Card Read Validation**
  - Enable Zephyr filesystem support for FAT-formatted microSD storage
  - Mount the SD card from the MCU
  - Add a `read_once` control command
  - Read one line from `neural_test.csv`
  - Publish that line on `/argus/neural_interface_bridge/neural_data`

- [ ] **Part C — SD-Backed Neural Playback**
  - Replace dummy neural sample generation with file-backed playback from the SD card
  - Add looped playback behavior at end-of-file
  - Support control commands:
    - `start`
    - `stop`
    - `reset`
    - `read_once`
  - Publish detailed SD status messages to simplify debugging

## Board Power Configuration

For the Olimex STM32-E407:

- **`PWR_SEL = 3–4`** → power from **JTAG/SWD**
- **`PWR_SEL = 5–6`** → power from **USB OTG2**
- **`PWR_SEL = 7–8`** → power from **USB OTG1**

### Power Mode Selection

- Use **`3–4`** when flashing firmware through the JTAG debugger
- Use **`5–6`** when running the firmware over **USB OTG2** with the micro-ROS Agent
- Use **`7–8`** when flashing through the STM32 **USB DFU bootloader** on **USB OTG1**

## Boot Jumper Configuration

The Olimex STM32-E407 uses two boot-selection jumpers:

- **`B0_0 + B1_0`** → normal boot from user flash
- **`B0_1 + B1_0`** → boot from STM32 system memory (DFU bootloader)

Use **`B0_1 + B1_0`** only when you want to flash over DFU. After flashing, switch back to **`B0_0 + B1_0`** and reset the board.

## Current Implementation Status

The firmware is currently at the end of **Part B**.

At this stage, it provides:

- a **control** topic that accepts simple commands
- a **neural_data** topic that publishes dummy neural sample strings on a timer
- a **`read_once`** command that mounts the SD card, opens `neural_test.csv`, and publishes one data line

This means the ROS 2 communication path is working, and one-shot SD read validation is in place. The next stage is replacing dummy timed publishing with continuous SD-backed playback.

### Current topics

- **Publishes:** `/argus/neural_interface_bridge/neural_data`
- **Subscribes:** `/argus/neural_interface_bridge/control`

### Current control commands

- `start` → begin timed dummy neural-data publishing
- `stop` → stop publishing
- `reset` → reset the dummy sample counter to zero
- `read_once` → mount the SD card, open the test file, and publish one data line

## DFU Flashing (USB OTG1)

DFU flashing uses the STM32 system bootloader over the board’s **USB_OTG1** port.

Before flashing over DFU:

- stop any running micro-ROS agent
- remove board power
- set boot jumpers to **`B0_1 + B1_0`**
- set **`PWR_SEL = 7–8`** if the board will be powered from **USB_OTG1**
- connect the USB cable to **USB_OTG1**
- reset or power-cycle the board

List DFU-capable devices:

```bash
dfu-util -l
```

Find the built Zephyr binary:
```bash
find ~/Documents/argus_embedded_ws -name zephyr.bin
```

If dfu-util -l shows Internal Flash on alt=0, flash with:
```bash
dfu-util -a 0 -s 0x08000000:leave -D /full/path/to/zephyr.bin
```

It might looks something like this:
```bash
dfu-util -a 0 -s 0x08000000:leave -D /home/argus/Documents/argus_embedded_ws/firmware/build/zephyr/zephyr.bin
```

After flashing:

remove power
set boot jumpers back to B0_0 + B1_0
move PWR_SEL back to the desired runtime mode
reset the board

## Usage

Examples below assume the workspace is located at:
```bash
~/Documents/argus_embedded_ws
```

Source the environment:
```bash
cd ~/Documents/argus_embedded_ws
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash
unset RMW_IMPLEMENTATION
```

Configure the firmware:
```bash
ros2 run micro_ros_setup configure_firmware.sh argus_neural_interface_bridge --transport serial-usb
```

Build the firmware:
```bash
ros2 run micro_ros_setup build_firmware.sh
```

Next we flash with the JTAG debugger(or DFU, instructions above); before flashing:

* stop any running micro-ROS agent

* remove board power

* make sure the board power supply jumper (PWR_SEL) is in the 3–4 position

* connect the JTAG debugger

In this mode, the JTAG debugger provides board power while connected. Additional information about the board can be found in the board manual: https://www.olimex.com/Products/ARM/ST/STM32-E407/resources/STM32-E407.pdf

At this point you're ready to flash:
```bash
ros2 run micro_ros_setup flash_firmware.sh
```

If you have trouble flashing the board, you can debug using GDB, first you'll need to launch a GDB server like OpenOCD:
```bash
cd ~/Documents/argus_embedded_ws
openocd -f interface/ftdi/olimex-arm-usb-tiny-h.cfg -f target/stm32f4x.cfg
```

If OpenOCD gets stuck in a bad state, it may help to hold the board’s Reset button while starting the OpenOCD or flash command, then release it after the debugger begins attaching.

Once OpenOCD is in a healthy state you can debug using GDB like so: 
```bash
cd ~/Documents/microros_ws

gdb-multiarch ~/Documents/microros_ws/firmware/freertos_apps/microros_olimex_e407_extensions/build/micro-ROS.elf
```

After successfully flashing switch back to runtime mode:

* depower the board

* move PWR_SEL to 5–6

* disconnect JTAG

* connect the board through the USB OTG2 port

Check that the board enumerates(You should see something like /dev/ttyACM0):
```bash
ls /dev/ttyACM*
```

Start the micro-ROS Agent:
```bash
cd ~/Documents/argus_embedded_ws
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash

ros2 run micro_ros_agent micro_ros_agent serial --dev /dev/ttyACM0
```

Verification In a second terminal:
```bash
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash

ros2 topic list
```

You should see the topics exposed by the firmware:
* /argus/neural_interface_bridge/control
* /argus/neural_interface_bridge/neural_data

Inspect the neural data stream:
```bash
ros2 topic echo /argus/neural_interface_bridge/neural_data
```

Start publishing dummy neural data:
```bash
ros2 topic pub --once /argus/neural_interface_bridge/control std_msgs/msg/String "{data: 'start'}"
```

Stop publishing:
```bash
ros2 topic pub --once /argus/neural_interface_bridge/control std_msgs/msg/String "{data: 'stop'}"
```

Reset the sample counter:
```bash
ros2 topic pub --once /argus/neural_interface_bridge/control std_msgs/msg/String "{data: 'reset'}"
```

Read one line from the SD-backed CSV:
```bash
ros2 topic pub --once /argus/neural_interface_bridge/control std_msgs/msg/String "{data: 'read_once'}"
```

## Overlay

the `olimex_stm32_e407.overlay` file included at root level needs to be placed on this path inside of your workspace: `/firmware/zephyr_apps/apps/argus_neural_interface_bridge/boards/olimex_stm32_e407.overlay`

## Next Steps

The planned next stage is to replace dummy timed sample generation with SD-backed dataset playback, while keeping the same ROS 2 topic structure:

* /argus/neural_interface_bridge/control

* /argus/neural_interface_bridge/neural_data

This will allow the board to behave more like a neural interface playback device for the rest of the Argus stack.

## local build

Build and flash: 
```bash
cd ~/Documents/microros_ws
source /opt/ros/humble/setup.bash
source install/local_setup.bash

ros2 run micro_ros_setup configure_firmware.sh argus_neural_interface_bridge --transport serial
ros2 run micro_ros_setup build_firmware.sh
ros2 run micro_ros_setup flash_firmware.sh
```

Test: 
```bash
ros2 topic echo /argus/neural_interface_bridge/neural_data
ros2 topic pub --once /argus/neural_interface_bridge/control std_msgs/msg/String '{data: start}'
ros2 topic pub --once /argus/neural_interface_bridge/control std_msgs/msg/String '{data: read_once}'
```

## STM32 Repro steps

1. Install STM32CubeIDE 2.1.1 and STM32CubeMX.
2. In CubeMX, create `olimex_e407_sd_fatfs_test` for STM32F407ZGTx.
3. Set RCC HSE to Crystal/Ceramic Resonator.
4. Enable SDIO in 4-bit mode.
5. Enable FATFS with SD Card backend.
6. Generate code for STM32CubeIDE.
7. Copy:
   - `Middlewares/Third_Party/FatFs/src/*`
   - `FATFS/App/*`
   - `FATFS/Target/*`
   into `firmware/freertos_apps/microros_olimex_e407_extensions/`.
8. Patch the extension Makefile to add FatFs and SD sources/includes.
9. Enable `HAL_SD_MODULE_ENABLED`.
10. Port `MX_SDIO_SD_Init()` and `HAL_SD_MspInit()/DeInit()`.
11. Build with `ros2 run micro_ros_setup build_firmware.sh`.
12. Flash with `ros2 run micro_ros_setup flash_firmware.sh`.
13. Start the micro-ROS agent and test `read_once`, `reset`, `start`, `stop`.g

## FreeRTOS

## UART