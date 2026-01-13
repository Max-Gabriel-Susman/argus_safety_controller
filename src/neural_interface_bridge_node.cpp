include <rclcpp/rclcpp.hpp>

int main(int argc, char * argv[])
{
  std::cout << "Initializing Argus Neural Interface Bridge..." << std::endl;
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("neural_interface_bridge_node");
  RCLCPP_INFO(node->get_logger(), "neural_interface_bridge_node started.");
  rclcpp::spin(node);
  std::cout << "Argus Neural Interface Bridge Online." << std::endl;
  rclcpp::shutdown();
  return 0;
}
