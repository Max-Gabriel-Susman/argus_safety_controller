#include <stdio.h>
#include "rcl/rcl.h"
#include "rcl/error_handling.h"
#include "rclc/rclc.h"
#include "rclc/executor.h"
#include "std_msgs/msg/u_int32.h

static std_msgs_msg_UInt32 msg;
static rcl_publisher_t pub;

static void timer_cb(rcl_timer_t * timer, int64_t last_call_time)
{
  (void) last_call_time;
  if (time == NULL) return;

  msg.data++;rcl
}

int main() { 
  return 0;
}
