# Argus Neural Interface Bridge

The **Argus Neural Interface Bridge** is a **micro-ROS client** for the Argus Neural Interface MCU. It is intended to let the embedded device publish telemetry into the Argus ROS 2 graph and receive inbound commands from the host side.

This project currently targets the **Olimex STM32-E407** using **Zephyr RTOS** and **micro-ROS**.

## Overview

The development workflow uses two different connection modes on the STM32 board:

- **JTAG/SWD mode** for flashing firmware
- **USB OTG2 serial mode** for runtime communication with the micro-ROS Agent

Because the board selects its power source with a jumper, the **`PWR_SEL`** setting must be changed depending on whether you are flashing or running the firmware.

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

If you get lost in this in the setup a great resource is the "First micro-ROS Application on Linux" tutorial. Note that the instructions here are for intel machines as opposed to ARM like this repo's instructions: https://micro.ros.org/docs/tutorials/core/first_application_linux/

## Usage

Examples below assume the workspace is located at:

```bash
~/Documents/argus_embedded_ws
```

Source the environment: 
```
cd ~/Documents/argus_embedded_ws
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash
unset RMW_IMPLEMENTATION
```

Configure the firmware: 
```
ros2 run micro_ros_setup configure_firmware.sh argus_neural_interface_bridge --transport serial-usb
```

Build the firmware
```
ros2 run micro_ros_setup build_firmware.sh
```

Next we're going to start with flashing the board with the JTAG debugger. Before flashing stop any running micro-ROS agent with the following command and then remove board power. Make sure that the board power supply jumper (PWR_SEL) is in the 3-4 position in order to power the board from the JTAG debugger; then connect the JTAG debugger(the JTAG debugger will provide board power while connected). Additional information can be found in this boards user manual: https://www.olimex.com/Products/ARM/ST/STM32-E407/resources/STM32-E407.pdf
 
Flash the firmware: 
```
ros2 run micro_ros_setup flash_firmware.sh
```

After flashing disconnect or depower the board, move PWR_SEL to 5–6 and connect the board through the USB OTG2 port. 

Check that the board enumerates; you should see someting like /dev/ttyACM0: 
```
ls /dev/ttyACM*
```

Start the micro-ROS Agent:
```
cd ~/Documents/argus_embedded_ws
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash

ros2 run micro_ros_agent micro_ros_agent serial --dev /dev/ttyACM0
```

In a second terminal:
```
source /opt/ros/$ROS_DISTRO/setup.bash
source ~/Documents/argus_embedded_ws/install/local_setup.bash

ros2 topic list
```

You should see the topics exposed by the firmware. To inspect a topic:
```
ros2 topic echo /microROS/ping
```

When testing the application, you can also publish manually:
```
ros2 topic pub --once /microROS/ping std_msgs/msg/Header "{stamp: {sec: 1, nanosec: 0}, frame_id: 'test'"
```

