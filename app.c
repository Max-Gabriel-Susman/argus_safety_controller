#include <rmw_microros/rmw_microros.h>
#include <uxr/client/profile/transport/custom/custom_transport.h>
#include "stm32f4xx_hal.h"

#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <std_msgs/msg/string.h>
#include <argus_core/msg/neural_frame.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "replay_data.h"

#define CONTROL_BUFFER_LEN   32
#define PUBLISH_PERIOD_MS    500
#define NEURAL_CHANNELS      REPLAY_CHANNELS

/* Disable UART debug prints while using UART3 as the micro-ROS transport. */
#define DBG_PRINT(...) do {} while (0)

#define RCCHECK(fn)                                                         \
  do {                                                                      \
    rcl_ret_t temp_rc = (fn);                                               \
    if (temp_rc != RCL_RET_OK) {                                            \
      return;                                                               \
    }                                                                       \
  } while (0)

#define RCSOFTCHECK(fn)                                                     \
  do {                                                                      \
    rcl_ret_t temp_rc = (fn);                                               \
    (void)temp_rc;                                                          \
  } while (0)

static rcl_publisher_t neural_data_publisher;
static rcl_subscription_t control_subscriber;

static std_msgs__msg__String incoming_control;
static argus_core__msg__NeuralFrame outgoing_neural_frame;

static bool streaming_enabled = false;
static size_t replay_index = 0;

static volatile int g_sub_rc = 0;
static volatile const char * g_sub_err_ptr = NULL;
static char g_sub_err_copy[160];

/* Verified from your extension tree */
extern UART_HandleTypeDef huart3;

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

static bool replay_reset(void)
{
  replay_index = 0;
  return true;
}

static bool replay_frame_to_neural_msg(
  const replay_frame_t * frame,
  argus_core__msg__NeuralFrame * msg)
{
  if (frame == NULL || msg == NULL) {
    return false;
  }

  msg->sample = frame->sample;
  msg->t = frame->t;
  msg->channel_count = NEURAL_CHANNELS;

  for (size_t i = 0; i < NEURAL_CHANNELS; ++i) {
    msg->channels[i] = (uint16_t)frame->channels[i];
  }

  return true;
}

static bool load_next_replay_frame(argus_core__msg__NeuralFrame * msg)
{
  if (msg == NULL) {
    return false;
  }

  if (replay_index >= REPLAY_FRAME_COUNT) {
    return false;
  }

  if (!replay_frame_to_neural_msg(&g_replay_frames[replay_index], msg)) {
    return false;
  }

  replay_index++;
  return true;
}

static void publish_current_frame(void)
{
  if (!load_next_replay_frame(&outgoing_neural_frame)) {
    streaming_enabled = false;
    return;
  }

  RCSOFTCHECK(rcl_publish(&neural_data_publisher, &outgoing_neural_frame, NULL));
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

  if (command_equals(msg, "start")) {
    streaming_enabled = true;
  } else if (command_equals(msg, "stop")) {
    streaming_enabled = false;
  } else if (command_equals(msg, "reset")) {
    streaming_enabled = false;
    (void)replay_reset();
  } else if (command_equals(msg, "read_once")) {
    publish_current_frame();
  }
}

static bool stm32_uart_transport_open(struct uxrCustomTransport * transport)
{
  (void)transport;
  return true;
}

static bool stm32_uart_transport_close(struct uxrCustomTransport * transport)
{
  (void)transport;
  return true;
}

static size_t stm32_uart_transport_write(
  struct uxrCustomTransport * transport,
  const uint8_t * buffer,
  size_t length,
  uint8_t * errcode)
{
  UART_HandleTypeDef * huart = (UART_HandleTypeDef *)transport->args;

  if (errcode != NULL) {
    *errcode = 0;
  }

  if (huart == NULL || buffer == NULL) {
    if (errcode != NULL) {
      *errcode = 1;
    }
    return 0;
  }

  HAL_StatusTypeDef ret =
    HAL_UART_Transmit(huart, (uint8_t *)buffer, (uint16_t)length, HAL_MAX_DELAY);

  if (ret != HAL_OK) {
    if (errcode != NULL) {
      *errcode = 1;
    }
    return 0;
  }

  return length;
}

static size_t stm32_uart_transport_read(
  struct uxrCustomTransport * transport,
  uint8_t * buffer,
  size_t length,
  int timeout,
  uint8_t * errcode)
{
  UART_HandleTypeDef * huart = (UART_HandleTypeDef *)transport->args;

  if (errcode != NULL) {
    *errcode = 0;
  }

  if (huart == NULL || buffer == NULL) {
    if (errcode != NULL) {
      *errcode = 1;
    }
    return 0;
  }

  if (timeout < 0) {
    HAL_StatusTypeDef ret =
      HAL_UART_Receive(huart, buffer, (uint16_t)length, HAL_MAX_DELAY);

    if (ret != HAL_OK) {
      if (errcode != NULL) {
        *errcode = 1;
      }
      return 0;
    }

    return length;
  }

  size_t total_read = 0;
  uint32_t start = HAL_GetTick();
  uint32_t timeout_ms = (uint32_t)timeout;

  while (total_read < length) {
    HAL_StatusTypeDef ret =
      HAL_UART_Receive(huart, &buffer[total_read], 1, 1);

    if (ret == HAL_OK) {
      total_read++;
      continue;
    }

    if ((HAL_GetTick() - start) >= timeout_ms) {
      break;
    }
  }

  return total_read;
}

void appMain(void * argument)
{
  (void)argument;

  rmw_uros_set_custom_transport(
    true,
    (void *)&huart3,
    stm32_uart_transport_open,
    stm32_uart_transport_close,
    stm32_uart_transport_write,
    stm32_uart_transport_read);

  rcl_allocator_t allocator = rcl_get_default_allocator();
  rclc_support_t support;
  rcl_node_t node;
  rcl_timer_t timer;
  rclc_executor_t executor;

  memset(&neural_data_publisher, 0, sizeof(neural_data_publisher));
  memset(&control_subscriber, 0, sizeof(control_subscriber));
  memset(&incoming_control, 0, sizeof(incoming_control));
  memset(&outgoing_neural_frame, 0, sizeof(outgoing_neural_frame));
  streaming_enabled = false;
  replay_index = 0;

  static char incoming_control_buffer[CONTROL_BUFFER_LEN];
  incoming_control.data.data = incoming_control_buffer;
  incoming_control.data.capacity = CONTROL_BUFFER_LEN;
  incoming_control.data.size = 0;
  incoming_control.data.data[0] = '\0';

  (void)replay_reset();

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

  rcl_ret_t sub_rc = rclc_subscription_init_best_effort(
    &control_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/argus/neural_interface_bridge/control");

  g_sub_rc = (int)sub_rc;

  if (sub_rc != RCL_RET_OK) {
    const char * err = rcl_get_error_string().str;

    g_sub_err_ptr = err;
    memset(g_sub_err_copy, 0, sizeof(g_sub_err_copy));

    if (err != NULL) {
      strncpy(g_sub_err_copy, err, sizeof(g_sub_err_copy) - 1);
      g_sub_err_copy[sizeof(g_sub_err_copy) - 1] = '\0';
    } else {
      strncpy(g_sub_err_copy, "<null>", sizeof(g_sub_err_copy) - 1);
    }

    __asm volatile("bkpt 0");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

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

  for (;;) {
    rclc_executor_spin_some(&executor, RCL_MS_TO_NS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
