/* argus_replay.c; file lvl documentation Needs impl.*/
/* Includes */
#include <string.h>
#include <stdint.h>

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "xil_printf.h"
#include "xtime_l.h"

#include "argus_wire.h"
#include "argus_replay_client.h"
#include "argus_replay.h"

/* Private macros */

/* Relay host. Same machine as the telemetry destination in
 * argus_net.c; if that address moves, both must move together. */
#define ARGUS_RELAY_IP_A 192
#define ARGUS_RELAY_IP_B 168
#define ARGUS_RELAY_IP_C 1
#define ARGUS_RELAY_IP_D 20

#define ARGUS_REPLAY_TIMEOUT_MS 200u
#define ARGUS_REPLAY_MAX_RETRIES 5u

/* Private variables */

static struct udp_pcb *g_pcb; /* Protocol Control Block pointer for the UDP session */
static ip_addr_t g_relay;
static argus_replay_client_t g_client;

/* One buffer half. 147 x 96 x 2 = 28224 bytes,
in Double Data Rate(DDR) Memory for now. */
static uint16_t g_buffer[
    ARGUS_REPLAY_SAMPLES_PER_HALF * ARGUS_MAX_CHANNELS];

// Needs imple.


/* Private routines */
// Needs imple.


/* Global routines */
// Needs imple.

int argus_replay_init(void)
{
    // Needs Impl.
}

int argus_replay_start_fetch(uint32_t sample_offset)
{
    // Needs Impl.
}

// needs impl.
