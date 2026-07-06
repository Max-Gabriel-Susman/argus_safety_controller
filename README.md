# Argus Safety Controller

The **Argus Safety Controller** is a **micro-ROS client** that's currently targetting the Argus Neural Interface MCU on an Ollimex board. The long term plan is to:

- [ ] 1. Port this over to the Arty Z7's PS.

- [ ] 2. Migrate the current neural decoding logic from this firmware app to the gateware in the argus-neural-codec repo and provide safe access to that gateware to the Argus Cybernetics Stack's ROS graph. This will target the Arty Z7'sPL.

- [ ] 3. Port this FreeRTOS implementation over to baremetal. 

- [ ] 4. Modify the implementation to be closed loop(not sure what that's going to look like yet).

This firmware application is intended to let the embedded device publish telemetry into the Argus ROS 2 graph. Eventually efforts will be put towards safety and receive inbound commands from the host side.

This project currently targets the **Olimex STM32-E407** running the **FreeRTOS** RTOS.

## STM32 Repro steps

Follow these steps to configure a new project for this app in stm32 Cube MX:

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

## Board Power Configuration

For the Olimex STM32-E407:

- **`PWR_SEL = 3–4`** → power from **JTAG/SWD**

### Current topics

- **Publishes:** `/argus/neural_interface_bridge/neural_data`


## Build and Debug with JTAG

Examples below assume the workspace is located at:
```bash
~/Documents/microros_ws
```

Source the environment:
```bash
cd ~/Documents/microros_ws
source /opt/ros/humble/setup.bash
source ~/Documents/microros_ws/install/local_setup.bash
unset RMW_IMPLEMENTATION
```

Configure the firmware:
```bash
ros2 run micro_ros_setup configure_firmware.sh argus_safety_controller --transport serial-usb
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
cd ~/Documents/microros_ws
openocd -f interface/ftdi/olimex-arm-usb-tiny-h.cfg -f target/stm32f4x.cfg
```

If OpenOCD gets stuck in a bad state, it may help to hold the board’s Reset button while starting the OpenOCD or flash command, then release it after the debugger begins attaching.

Once OpenOCD is in a healthy state you can debug using GDB like so: 
```bash
cd ~/Documents/microros_ws

gdb-multiarch ~/Documents/microros_ws/firmware/freertos_apps/microros_olimex_e407_extensions/build/micro-ROS.elf
```

It should be noted that the hardware used for this project only supports the use of 6 breakpoints at a time.

### Run App with UART

Running the micro-ros agent over UART:
```bash
cd ~/Documents/microros_ws
source /opt/ros/humble/setup.bash
source install/local_setup.bash
ros2 run micro_ros_agent micro_ros_agent serial --dev /dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0 -v6
```
