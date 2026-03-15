# Argus Neural Interface Bridge

The **Argus Neural Interface Bridge** is a **micro-ROS client** for the Argus Neural Interface MCU. It is intended to let the embedded device publish telemetry into the Argus ROS 2 graph and receive inbound commands from the host side.

This project currently targets the **Olimex STM32-E407** running the **Zephyr** RTOS.

## Overview

The development workflow uses two different connection modes on the STM32 board:

- **JTAG/SWD mode** for flashing firmware
- **USB OTG2 serial mode** for runtime communication with the micro-ROS Agent

Because the board selects its power source with a jumper, the **`PWR_SEL`** setting must be changed depending on whether you are flashing or running the firmware.

## Roadmap

## Board Power Configuration

For the Olimex STM32-E407:

- **`PWR_SEL = 3–4`** → power from **JTAG/SWD**
- **`PWR_SEL = 5–6`** → power from **USB OTG2**

### Power Mode Selection

- Use **`3–4`** when flashing firmware through the debugger
- Use **`5–6`** when running the firmware over **USB OTG2** with the micro-ROS Agent

## Prerequisites

Before using this project, make sure you have:

- ROS 2 installed
- a working `micro_ros_setup` workspace
- the **Olimex STM32-E407** board
- an **ARM-USB-TINY-H** or compatible JTAG debugger
- a USB connection to the board’s **OTG2** port for runtime serial transport

If you get lost during setup, a useful reference is the micro-ROS tutorial below. Note that this repo’s setup flow differs somewhat because this environment uses ARM rather than a typical x86 Linux machine:

https://micro.ros.org/docs/tutorials/core/first_application_linux/

## Current Implementation Status

The current implementation is **Part A** of a staged bring-up plan.

At this stage, the firmware does **not** yet read from the SD card. Instead, it provides:

- a **control** topic that accepts simple commands
- a **neural_data** topic that publishes dummy neural sample strings on a timer

This establishes the ROS 2 communication shape that will later be reused for SD-backed neural playback.

### Current topics

- **Publishes:** `/argus/neural_interface_bridge/neural_data`
- **Subscribes:** `/argus/neural_interface_bridge/control`

### Current control commands

- `start` → begin timed dummy neural-data publishing
- `stop` → stop publishing
- `reset` → reset the dummy sample counter to zero

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
cd ~/Documents/argus_embedded_ws
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash
unset RMW_IMPLEMENTATION
```

Build the firmware:
```bash
ros2 run micro_ros_setup build_firmware.sh
```

Next we flash with the JTAG debugger; before flashing:

* stop any running micro-ROS agent

* remove board power

* make sure the board power supply jumper (PWR_SEL) is in the 3–4 position

* connect the JTAG debugger

In this mode, the JTAG debugger provides board power while connected. Additional information about the board can be found in the board manual: https://www.olimex.com/Products/ARM/ST/STM32-E407/resources/STM32-E407.pdf

At this point you're ready to flash:
```bash
ros2 run micro_ros_setup flash_firmware.sh
```

If you have trouble flashing the board, you can assess the JTAG path with:
```bash
cd ~/Documents/argus_embedded_ws
openocd -f interface/ftdi/olimex-arm-usb-tiny-h.cfg -f target/stm32f4x.cfg -c "init; reset halt; shutdown"
```

If OpenOCD gets stuck in a bad state, it may help to hold the board’s Reset button while starting the OpenOCD or flash command, then release it after the debugger begins attaching.

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
* /argus/neural_interface_bridge/controld
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

## Next Steps

The planned next stage is to replace dummy timed sample generation with SD-backed dataset playback, while keeping the same ROS 2 topic structure:

* /argus/neural_interface_bridge/control

* /argus/neural_interface_bridge/neural_data

This will allow the board to behave more like a neural interface playback device for the rest of the Argus stack.