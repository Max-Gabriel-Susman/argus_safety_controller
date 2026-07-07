# Argus Safety Controller

The **Argus Safety Controller** is a **micro-ROS client** that's currently targetting the Argus Neural Interface MCU on an Ollimex board. The long term plan is to:

- [ ] 1. Port this over to the Arty Z7's PS.

- [ ] 2. Migrate the current neural decoding logic from this firmware app to the gateware in the argus-neural-codec repo and provide safe access to that gateware to the Argus Cybernetics Stack's ROS graph. This will target the Arty Z7'sPL.

- [ ] 3. Port this FreeRTOS implementation over to baremetal. 

- [ ] 4. Modify the implementation to be closed loop(not sure what that's going to look like yet).

This firmware application is intended to let the embedded device publish telemetry into the Argus ROS 2 graph. Eventually efforts will be put towards safety and receive inbound commands from the host side.

This project currently targets the **Olimex STM32-E407** running the **FreeRTOS** RTOS.
