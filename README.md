# Argus Neural Interface Bridge

The Argus Neural Interface Bridge provides a micro-ROS client to allow the Argus Neural Interface's MCU to publish telemetry to the Argus ROS graph and receive stim commands.

## Usage

Build and run the micro-ROS client:
```
cd ~/microros_client_ws
source /opt/ros/humble/setup.bash
source ~/microros_client_ws/install/setup.bash
colcon build --symlink-install --packages-select argus_neural_interface_bridge
source install/setup.bash
```

Start an agent:
```
source /opt/ros/humble/setup.bash
ros2 run micro_ros_agent micro_ros_agent udp4 --port 8888 -v6
```

Run the node:
```
source /opt/ros/humble/setup.bash
source ~/microros_client_ws/install/setup.bash

export RMW_IMPLEMENTATION=rmw_microxrcedds
export RMW_UXRCE_TRANSPORT=udp
export RMW_UXRCE_IPV4_ADDRESS=127.0.0.1
export RMW_UXRCE_PORT=8888

ros2 run argus_neural_interface_bridge argus_neural_interface_bridge_node
```

Verify:
```
source /opt/ros/humble/setup.bash
ros2 topic echo /argus/neural/codec/heartbeat
```