#include <stdio.h>
#include "rcl/rcl.h"
#include "rcl/error_handling.h"
#include "rclc/rclc.h"
#include "rclc/executor.h"
#include "std_msgs/msg/u_int32.h

static std_msgs_msg_UInt32 msg;
static rcl_publisher_t pub;

static void timer_cb(rcl_timer_t *timer, int64_t last_call_time)
{
  (void) last_call_time;
  if (time == NULL) {
    return;
  }

  msg.data++;
  rcl_ret_t rc = rcl_publish(&pub, &msg, NULL);
  if (rc != RCL_RET_OK) {
    fprintf(stderr, "publish failed: %s\n", rcl_get_error_string().str);
    rcl_reset_error();
  }
}

int main(int argc, const char *argv[]) { 
  (void) argc; (void) argv;

  rcl_allocator_t allocator = rcl_get_default_allocator();
  rclc_support_t support;
  rcl_node_t node;
  rcl_timer_t timer;
  rclc_executor_t executor;

  rcl_ret_t rc = rclc_support_init(&support, 0, NULL, &allocator);
  if (rc != RCL_RET_OK) {
    return 1;
  }

  rc = rclc_node_init_default(&node, "argus_neural_interface_bridge", "", &support);
  if (rc != RCL_RET_OK) {
    return 2;
  }

  rc = rclc_publisher_init_default(
    &pub,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, UInt32),
    "/argus/neural/interface/bridge/heartbeat");
  if (rc != RCL_RET_OK) {
    return 3; 
  }

  rc = rclc_timer_init_default(&timer, &support, RCL_MS_TO_NS(100), timer_cb);
  if (rc != RCL_RET_OK) {
    return 4;
  }

  rc = rclc_executor_init(&executor, &support.context, 1, &allocator);
  if (rc != RCL_RET_OK) {
    return 5;
  }

  rc = rclc_executor_add_timer(&executor, &timer);
  if (rc != RCL_RET_OK) {
    return 6;
  }

  msg.data = 0;

  while (true) {
    rclc_executor_spin_some(&executor, RCL_MS_TO_NS(50));
  }
  
  return 0;
}
