#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <rmw_microros/rmw_microros.h>
#include <microros_transports.h>

#include <std_msgs/msg/string.h>

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>

#include <zephyr.h>

#define STRING_BUFFER_LEN 128
#define PUBLISH_PERIOD_MS 500

#define RCCHECK(fn) { \
	rcl_ret_t temp_rc = fn; \
	if ((temp_rc != RCL_RET_OK)) { \
		printf("Failed status on line %d: %d. Aborting.\n", __LINE__, (int)temp_rc); \
		return; \
	} \
}

#define RCSOFTCHECK(fn) { \
	rcl_ret_t temp_rc = fn; \
	if ((temp_rc != RCL_RET_OK)) { \
		printf("Failed status on line %d: %d. Continuing.\n", __LINE__, (int)temp_rc); \
	} \
}

rcl_publisher_t neural_data_publisher;
rcl_subscription_t control_subscriber;

std_msgs__msg__String incoming_control;
std_msgs__msg__String outgoing_neural_data;

bool streaming_enabled = false;
int sample_idx = 0;

static bool command_equals(const std_msgs__msg__String * msg, const char * cmd)
{
	size_t cmd_len = strlen(cmd);

	if (msg->data.data == NULL) {
		return false;
	}

	if (msg->data.size != cmd_len) {
		return false;
	}

	return strncmp(msg->data.data, cmd, cmd_len) == 0;
}

void neural_data_timer_callback(rcl_timer_t * timer, int64_t last_call_time)
{
	(void) last_call_time;

	if (timer == NULL || !streaming_enabled) {
		return;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	int ch1 = sample_idx % 100;
	int ch2 = (sample_idx + 7) % 100;
	int ch3 = (sample_idx + 13) % 100;
	int ch4 = (sample_idx + 29) % 100;

	int written = snprintf(
		outgoing_neural_data.data.data,
		outgoing_neural_data.data.capacity,
		"sample=%d,t=%ld.%09ld,ch1=%d,ch2=%d,ch3=%d,ch4=%d",
		sample_idx,
		(long)ts.tv_sec,
		(long)ts.tv_nsec,
		ch1,
		ch2,
		ch3,
		ch4
	);

	if (written < 0) {
		printf("Failed to format outgoing neural data.\n");
		return;
	}

	if ((size_t)written >= outgoing_neural_data.data.capacity) {
		written = outgoing_neural_data.data.capacity - 1;
		outgoing_neural_data.data.data[written] = '\0';
	}

	outgoing_neural_data.data.size = (size_t)written;

	RCSOFTCHECK(rcl_publish(&neural_data_publisher, &outgoing_neural_data, NULL));
	printf("Published neural data: %s\n", outgoing_neural_data.data.data);

	sample_idx++;
}

void control_subscription_callback(const void * msgin)
{
	const std_msgs__msg__String * msg = (const std_msgs__msg__String *)msgin;

	if (msg->data.data == NULL) {
		return;
	}

	printf("Received control command: %s\n", msg->data.data);

	if (command_equals(msg, "start")) {
		streaming_enabled = true;
		printf("Streaming enabled.\n");
	} else if (command_equals(msg, "stop")) {
		streaming_enabled = false;
		printf("Streaming disabled.\n");
	} else if (command_equals(msg, "reset")) {
		sample_idx = 0;
		printf("Sample index reset.\n");
	} else {
		printf("Unknown control command.\n");
	}
}

void main(void)
{
	rmw_uros_set_custom_transport(
		MICRO_ROS_FRAMING_REQUIRED,
		(void *) &default_params,
		zephyr_transport_open,
		zephyr_transport_close,
		zephyr_transport_write,
		zephyr_transport_read
	);

	rcl_allocator_t allocator = rcl_get_default_allocator();
	rclc_support_t support;

	RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));

	rcl_node_t node;
	RCCHECK(rclc_node_init_default(&node, "argus_neural_interface_bridge", "", &support));

	RCCHECK(rclc_publisher_init_default(
		&neural_data_publisher,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
		"/argus/neural_interface_bridge/neural_data"));

	RCCHECK(rclc_subscription_init_best_effort(
		&control_subscriber,
		&node,
		ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
		"/argus/neural_interface_bridge/control"));

	rcl_timer_t timer;
	RCCHECK(rclc_timer_init_default(
		&timer,
		&support,
		RCL_MS_TO_NS(PUBLISH_PERIOD_MS),
		neural_data_timer_callback));

	rclc_executor_t executor;
	RCCHECK(rclc_executor_init(&executor, &support.context, 2, &allocator));
	RCCHECK(rclc_executor_add_timer(&executor, &timer));
	RCCHECK(rclc_executor_add_subscription(
		&executor,
		&control_subscriber,
		&incoming_control,
		&control_subscription_callback,
		ON_NEW_DATA));

	static char outgoing_neural_data_buffer[STRING_BUFFER_LEN];
	outgoing_neural_data.data.data = outgoing_neural_data_buffer;
	outgoing_neural_data.data.capacity = STRING_BUFFER_LEN;
	outgoing_neural_data.data.size = 0;
	outgoing_neural_data.data.data[0] = '\0';

	static char incoming_control_buffer[STRING_BUFFER_LEN];
	incoming_control.data.data = incoming_control_buffer;
	incoming_control.data.capacity = STRING_BUFFER_LEN;
	incoming_control.data.size = 0;
	incoming_control.data.data[0] = '\0';

	printf("argus_neural_interface_bridge ready. Waiting for control commands.\n");

	while (1) {
		rclc_executor_spin_some(&executor, RCL_MS_TO_NS(100));
		usleep(100000);
	}

	RCSOFTCHECK(rcl_publisher_fini(&neural_data_publisher, &node));
	RCSOFTCHECK(rcl_subscription_fini(&control_subscriber, &node));
	RCSOFTCHECK(rcl_node_fini(&node));
}