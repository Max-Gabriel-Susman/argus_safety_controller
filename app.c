// app.c
#include <rmw_microros/rmw_microros.h>
#include <uxr/client/profile/transport/custom/custom_transport.h>
#include "stm32f4xx_hal.h"

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
#include <stdlib.h>
#include <limits.h>

#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"

#define CONTROL_BUFFER_LEN   32
#define PUBLISH_PERIOD_MS    500
#define NEURAL_CHANNELS      96
#define CSV_LINE_BUFFER_LEN  768
#define NEURAL_CSV_PATH      "0:/neural_96.csv"

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

static rcl_publisher_t neural_data_publisher;
static rcl_subscription_t control_subscriber;

static std_msgs__msg__String incoming_control;
static argus_core__msg__NeuralFrame outgoing_neural_frame;

static bool streaming_enabled = false;
static FATFS sd_fs;
static FIL neural_csv_file;
static bool sd_mounted = false;
static bool csv_open = false;

static char csv_line_buffer[CSV_LINE_BUFFER_LEN];

/*
 * Replace huart3 if your grep output shows a different UART handle.
 */
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

static void trim_newline(char * s)
{
  if (s == NULL) {
    return;
  }

  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[len - 1] = '\0';
    --len;
  }
}

static bool sd_readline(FIL * file, char * buffer, size_t buffer_len)
{
  if (file == NULL || buffer == NULL || buffer_len < 2) {
    return false;
  }

  size_t index = 0;

  while (index < (buffer_len - 1)) {
    UINT br = 0;
    char c = '\0';
    FRESULT fr = f_read(file, &c, 1, &br);

    if (fr != FR_OK) {
      printf("f_read failed: %d\n", (int)fr);
      return false;
    }

    if (br == 0) {
      break;
    }

    buffer[index++] = c;

    if (c == '\n') {
      break;
    }
  }

  buffer[index] = '\0';

  if (index == 0) {
    return false;
  }

  trim_newline(buffer);
  return true;
}

static bool parse_csv_line_to_neural_frame(
  const char * line,
  argus_core__msg__NeuralFrame * msg)
{
  if (line == NULL || msg == NULL) {
    return false;
  }

  char local[CSV_LINE_BUFFER_LEN];
  strncpy(local, line, sizeof(local) - 1);
  local[sizeof(local) - 1] = '\0';

  char * saveptr = NULL;
  char * token = strtok_r(local, ",", &saveptr);
  char * endptr = NULL;

  if (token == NULL) {
    printf("CSV parse failed: missing sample column\n");
    return false;
  }

  unsigned long sample_value = strtoul(token, &endptr, 10);
  if (endptr == token || *endptr != '\0' || sample_value > UINT32_MAX) {
    printf("CSV parse failed: invalid sample value '%s'\n", token);
    return false;
  }
  msg->sample = (uint32_t)sample_value;

  token = strtok_r(NULL, ",", &saveptr);
  if (token == NULL) {
    printf("CSV parse failed: missing time column\n");
    return false;
  }

  endptr = NULL;
  float t_value = strtof(token, &endptr);
  if (endptr == token || *endptr != '\0') {
    printf("CSV parse failed: invalid time value '%s'\n", token);
    return false;
  }
  msg->t = t_value;

  msg->channel_count = NEURAL_CHANNELS;

  for (size_t i = 0; i < NEURAL_CHANNELS; ++i) {
    token = strtok_r(NULL, ",", &saveptr);
    if (token == NULL) {
      printf("CSV parse failed: missing channel %lu\n", (unsigned long)i);
      return false;
    }

    endptr = NULL;
    unsigned long channel_value = strtoul(token, &endptr, 10);
    if (endptr == token || *endptr != '\0' || channel_value > UINT16_MAX) {
      printf("CSV parse failed: invalid channel %lu value '%s'\n",
             (unsigned long)i, token);
      return false;
    }

    msg->channels[i] = (uint16_t)channel_value;
  }

  return true;
}

static bool sd_open_and_prime_csv(void)
{
  FRESULT fr;

  if (!sd_mounted) {
    fr = f_mount(&sd_fs, "0:", 1);
    if (fr != FR_OK) {
      printf("f_mount failed: %d\n", (int)fr);
      return false;
    }
    sd_mounted = true;
    printf("SD mounted successfully\n");
  }

  if (csv_open) {
    f_close(&neural_csv_file);
    csv_open = false;
  }

  fr = f_open(&neural_csv_file, NEURAL_CSV_PATH, FA_READ);
  if (fr != FR_OK) {
    printf("f_open failed for %s: %d\n", NEURAL_CSV_PATH, (int)fr);
    return false;
  }

  csv_open = true;
  printf("Opened %s\n", NEURAL_CSV_PATH);

  if (!sd_readline(&neural_csv_file, csv_line_buffer, sizeof(csv_line_buffer))) {
    printf("Failed to read CSV header\n");
    f_close(&neural_csv_file);
    csv_open = false;
    return false;
  }

  printf("CSV header: %s\n", csv_line_buffer);
  return true;
}

static bool sd_reset_csv(void)
{
  return sd_open_and_prime_csv();
}

static bool load_next_sd_frame(argus_core__msg__NeuralFrame * msg)
{
  if (msg == NULL) {
    return false;
  }

  if (!csv_open) {
    if (!sd_open_and_prime_csv()) {
      return false;
    }
  }

  if (!sd_readline(&neural_csv_file, csv_line_buffer, sizeof(csv_line_buffer))) {
    printf("Reached EOF or failed reading next CSV row\n");
    return false;
  }

  return parse_csv_line_to_neural_frame(csv_line_buffer, msg);
}

static void publish_current_frame(void)
{
  if (!load_next_sd_frame(&outgoing_neural_frame)) {
    printf("No SD-backed replay frame available\n");
    streaming_enabled = false;
    return;
  }

  RCSOFTCHECK(rcl_publish(&neural_data_publisher, &outgoing_neural_frame, NULL));

  printf("Published NeuralFrame sample=%lu t=%.3f channels=%u ch0=%u ch95=%u\n",
         (unsigned long)outgoing_neural_frame.sample,
         (double)outgoing_neural_frame.t,
         (unsigned int)outgoing_neural_frame.channel_count,
         (unsigned int)outgoing_neural_frame.channels[0],
         (unsigned int)outgoing_neural_frame.channels[95]);
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

  printf("Received control command: %.*s\n",
         (int)msg->data.size,
         msg->data.data);

  if (command_equals(msg, "start")) {
    streaming_enabled = true;
    printf("Streaming enabled\n");
  } else if (command_equals(msg, "stop")) {
    streaming_enabled = false;
    printf("Streaming disabled\n");
  } else if (command_equals(msg, "reset")) {
    streaming_enabled = false;
    if (sd_reset_csv()) {
      printf("Replay reset to start of CSV\n");
    } else {
      printf("Replay reset failed\n");
    }
  } else if (command_equals(msg, "read_once")) {
    printf("Publishing the current frame\n");
    publish_current_frame();
  } else {
    printf("Unknown control command\n");
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
  printf("Initializing Argus Neural Interface Bridge...\n");
  (void)argument;

  printf("Configuring micro-ROS UART transport...\n");
  rmw_ret_t trc = rmw_uros_set_custom_transport(
    true,
    (void *)&huart3,
    stm32_uart_transport_open,
    stm32_uart_transport_close,
    stm32_uart_transport_write,
    stm32_uart_transport_read);
  printf("rmw_uros_set_custom_transport rc=%d\n", (int)trc);

  rcl_allocator_t allocator = rcl_get_default_allocator();
  rclc_support_t support;
  rcl_node_t node;
  rcl_timer_t timer;
  rclc_executor_t executor;

  memset(&neural_data_publisher, 0, sizeof(neural_data_publisher));
  memset(&control_subscriber, 0, sizeof(control_subscriber));
  memset(&incoming_control, 0, sizeof(incoming_control));
  memset(&outgoing_neural_frame, 0, sizeof(outgoing_neural_frame));
  memset(&sd_fs, 0, sizeof(sd_fs));
  memset(&neural_csv_file, 0, sizeof(neural_csv_file));
  sd_mounted = false;
  csv_open = false;
  streaming_enabled = false;

  /*
   * Keep SD startup fenced off for now so we can isolate transport/node bringup.
   * Re-enable after the node and topic appear correctly.
   */
  /*
  if (!sd_open_and_prime_csv()) {
    printf("Warning: SD CSV source not ready at startup\n");
  }
  */

  static char incoming_control_buffer[CONTROL_BUFFER_LEN];
  incoming_control.data.data = incoming_control_buffer;
  incoming_control.data.capacity = CONTROL_BUFFER_LEN;
  incoming_control.data.size = 0;
  incoming_control.data.data[0] = '\0';

  printf("startup: before rclc_support_init\n");
  RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));
  printf("startup: support ok\n");

  RCCHECK(rclc_node_init_default(
    &node,
    "argus_neural_interface_bridge",
    "",
    &support));
  printf("startup: node ok\n");

  RCCHECK(rclc_publisher_init_default(
    &neural_data_publisher,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(argus_core, msg, NeuralFrame),
    "/argus/neural_interface_bridge/neural_data"));
  printf("startup: publisher ok\n");

  RCCHECK(rclc_subscription_init_best_effort(
    &control_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
    "/argus/neural_interface_bridge/control"));
  printf("startup: subscription ok\n");

  RCCHECK(rclc_timer_init_default(
    &timer,
    &support,
    RCL_MS_TO_NS(PUBLISH_PERIOD_MS),
    neural_data_timer_callback));
  printf("startup: timer ok\n");

  RCCHECK(rclc_executor_init(&executor, &support.context, 2, &allocator));
  printf("startup: executor init ok\n");

  RCCHECK(rclc_executor_add_timer(&executor, &timer));
  printf("startup: executor add timer ok\n");

  RCCHECK(rclc_executor_add_subscription(
    &executor,
    &control_subscriber,
    &incoming_control,
    &control_subscription_callback,
    ON_NEW_DATA));
  printf("startup: executor add subscription ok\n");

  printf("Argus Neural Interface Bridge online.\n");

  for (;;) {
    rclc_executor_spin_some(&executor, RCL_MS_TO_NS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}