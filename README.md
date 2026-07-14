# Argus Safety Controller

The **Argus Safety Controller** targets Cortex A9 MCU on the Arty Z7. This firmware application is intended to let the embedded device publish telemetry into the Argus ROS 2 graph. Eventually efforts will be put towards safety once we're receiving inbound commands for stimulus from the host side. The long term plan is to:

- [ ] 1. Migrate the neural decoding logic from the argus neural interface firmware project to the gateware in the argus-neural-codec repo and provide safe gateware access to the Argus Cybernetics Stack's ROS graph. This will target the Arty Z7'sPL.

- [ ] 2. Port the FreeRTOS implementation over to baremetal. 

- [ ] 3. Modify the implementation to be closed loop.

## Build System

This project currently uses CMake as its build system for the embedded software application. Source files for the embedded application are registered through the Cmake configuration in `UserConfig.cmake`. You'll need to update the `sources` section in `UserConfig.cmake to include new source files so that Vitis includes them in the build. 