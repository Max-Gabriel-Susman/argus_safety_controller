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

  return 0;
}
