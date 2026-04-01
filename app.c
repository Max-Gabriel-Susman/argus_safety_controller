// app.c
#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <std_msgs/msg/string.h>
#include <argus_core/msg/neural_frame.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#define CONTROL_BUFFER_LEN 32
#define PUBLISH_PERIOD_MS  500
#define NEURAL_CHANNELS    96

#define RCCHECK(fn)                                                         \
  do {                                                                      \
    rcl_ret_t temp_rc = (fn);                                               \
    if (temp_rc != RCL_RET_OK) {                                            \
      printf("Failed status on line %d: %d. Aborting.\n",                   \
             __LINE__, (int)temp_rc);                                       \
      return;                                                               \
    }                                                                       \
  } while (0)

#define RCSOFTCHECK(fn)                                                     \
  do {                                                                      \
    rcl_ret_t temp_rc = (fn);                                               \
    if (temp_rc != RCL_RET_OK) {                                            \
      printf("Failed status on line %d: %d. Continuing.\n",                 \
             __LINE__, (int)temp_rc);                                       \
    }                                                                       \
  } while (0)

typedef struct {
  uint32_t sample;
  float t;
  uint16_t channels[NEURAL_CHANNELS];
} replay_frame_t;

extern const replay_frame_t g_neural_replay[];
extern const size_t g_neural_replay_len;

static rcl_publisher_t neural_data_publisher;
static rcl_subscription_t control_subscriber;

static std_msgs__msg__String incoming_control;
static argus_core__msg__NeuralFrame outgoing_neural_frame;

static bool streaming_enabled = false;
static size_t replay_index = 0;

static bool command_equals(const std_msgs__msg__String * msg, const char * cmd)
{
  size_t cmd_len = strlen(cmd);

  if (msg == NULL || msg->data.data == NULL) {
    return false;
  }

  if (msg->data.size != cmd_len) {
    return false;
  }

  return strncmp(msg->data.data, cmd, cmd_len) == 0;
}

static bool load_next_replay_frame(argus_core__msg__NeuralFrame * msg)
{
  if (msg == NULL || g_neural_replay_len == 0) {
    return false;
  }

  const replay_frame_t * src = &g_neural_replay[replay_index];

  msg->sample = src->sample;
  msg->t = src->t;
  msg->channel_count = NEURAL_CHANNELS;

  for (size_t i = 0; i < NEURAL_CHANNELS; ++i) {
    msg->channels[i] = src->channels[i];
  }

  replay_index = (replay_index + 1) % g_neural_replay_len;
  return true;
}

static void publish_current_frame(void)
{
  if (!load_next_replay_frame(&outgoing_neural_frame)) {
    printf("No replay frames available\n");
    return;
  }

  RCSOFTCHECK(rcl_publish(&neural_data_publisher, &outgoing_neural_frame, NULL));

  printf("Published NeuralFrame sample=%lu t=%.3f channels=%u\n",
         (unsigned long)outgoing_neural_frame.sample,
         (double)outgoing_neural_frame.t,
         (unsigned int)outgoing_neural_frame.channel_count);
}

static void neural_data_timer_callback(rcl_timer_t * timer, int64_t last_call_time)
{
  (void)last_call_time;

  if (timer == NULL || !streaming_enabled) {
    return;
  }

  publish_current_frame();
}

static void control_subscription_callback(const void * msgin)
{
  const std_msgs__msg__String * msg = (const std_msgs__msg__String *)msgin;

  if (msg == NULL || msg->data.data == NULL) {
    return;
  }

  printf("Received control command: %s\n", msg->data.data);

  if (command_equals(msg, "start")) {
    streaming_enabled = true;
    printf("Streaming enabled\n");
  } else if (command_equals(msg, "stop")) {
    streaming_enabled = false;
    printf("Streaming disabled\n");
  } else if (command_equals(msg, "reset")) {
    replay_index = 0;
    printf("Replay reset\n");
  } else if (command_equals(msg, "read_once")) {
    printf("Publishing the current frame\n");
    publish_current_frame();
  } else {
    printf("Unknown control command\n");
  }
}

void appMain(void * argument)
{
  printf("Initializing Argus Neural Interface Bridge...\n");
  (void)argument;

  rcl_allocator_t allocator = rcl_get_default_allocator();
  rclc_support_t support;
  rcl_node_t node;
  rcl_timer_t timer;
  rclc_executor_t executor;

  memset(&neural_data_publisher, 0, sizeof(neural_data_publisher));
  memset(&control_subscriber, 0, sizeof(control_subscriber));
  memset(&incoming_control, 0, sizeof(incoming_control));
  memset(&outgoing_neural_frame, 0, sizeof(outgoing_neural_frame));

  static char incoming_control_buffer[CONTROL_BUFFER_LEN];
  incoming_control.data.data = incoming_control_buffer;
  incoming_control.data.capacity = CONTROL_BUFFER_LEN;
  incoming_control.data.size = 0;
  incoming_control.data.data[0] = '\0';

  RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));

  RCCHECK(rclc_node_init_default(
    &node,
    "argus_neural_interface_bridge",
    "",
    &support));

  RCCHECK(rclc_publisher_init_default(
    &neural_data_publisher,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(argus_core, msg, NeuralFrame),
    "/argus/neural_interface_bridge/neural_data"));

  RCCHECK(rclc_subscription_init_best_effort(
    &control_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/argus/neural_interface_bridge/control"));

  RCCHECK(rclc_timer_init_default(
    &timer,
    &support,
    RCL_MS_TO_NS(PUBLISH_PERIOD_MS),
    neural_data_timer_callback));

  RCCHECK(rclc_executor_init(&executor, &support.context, 2, &allocator));
  RCCHECK(rclc_executor_add_timer(&executor, &timer));
  RCCHECK(rclc_executor_add_subscription(
    &executor,
    &control_subscriber,
    &incoming_control,
    &control_subscription_callback,
    ON_NEW_DATA));

  printf("Argus Neural Interface Bridge online.\n");

  for (;;) {
    rclc_executor_spin_some(&executor, RCL_MS_TO_NS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}