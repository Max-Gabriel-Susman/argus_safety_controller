cmake_minimum_required(VERSION 3.8)
project(argus_neural_interface_bridge)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rcl REQUIRED)
find_package(rclc REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(argus_neural_interface_bridge_node src/argus_neural_interface_bridge.c)
ament_target_dependencies(argus_neural_interface_bridge_node rcl rclc std_msgs)

install(TARGETS argus_neural_interface_bridge_node
  DESTINATION lib/${PROJECT_NAME})

ament_package()