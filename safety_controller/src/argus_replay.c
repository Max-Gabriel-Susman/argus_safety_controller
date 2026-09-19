/* argus_replay.c; TODO: file lvl documentation */
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

/* Destination for one fetch: 147 x 96 x 2 = 28224 bytes.
 * in Double Data Rate(DDR) Memory for now.
 *
 * Sized from ARGUS_REPLAY_SAMPLES_PER_HALF because a rquest is one
 * buffer half in the eventual PL design, where two BRAM apertures
 * alternate. There is no ping-pong here. No PL consumer exists yet,
 * so one buffer in DDR is the whole story. When the AXI BRAM
 * controller lands, this array goes away and the caller passes in
 * whicherver aperture is idle. */
static uint16_t g_buffer[
    ARGUS_REPLAY_SAMPLES_PER_HALF * ARGUS_MAX_CHANNELS];

/* Scratch for flattening a possibly-chained packet buffer(pbuf). A
 * chunk is 1372 bytes, which fits one MTU but not necessarily one
 * pbuf: PBUF_POOL_BUFSIZE governs that, and the Xilinx port does
 * chain(split one chunk acrross multiple packet buffers). Parsing
 * p->payload directly would read past the end of the first link and
 * reject every full-size chunk. */
static uint8_t g_rx[
    sizeof(argus_replay_chunk_hdr_t) + ARGUS_REPLAY_MAX_PAYLOAD];

/* Private routines */

/* The Cortex-A9 global timer, not sys_now(). Independent of lwIP's
 * timer configuration, so a stalled tick cannot silently disable
 * retransmits(which would've made a dropped chunk look like a dead
 * relay). */
static uint32_t argus_now_ms(void)
{
    XTime t;
    XTime_GetTime(&t);
    return (uint32_t)(t / (COUNTS_PER_SECOND / 1000U));
}

/* Transport hook for argus_replay_client_t: hands a fully framed
 * request to lwIP. Keeping this behind a function pointer is what
 * let the client be validated on the host against ReplayServer
 * before any of this existed.
 *
 * Returns 0 on success, non-zero if the packet could not be queued. */
static int replay_send(void *ctx, const void *data, uint16_t len)
{
    struct pbuf *p;
    err_t err;

    (void)ctx;

    p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == NULL) {
        return -1;
    }
    memcpy(p->payload, data, len);

    err = udp_sendto(g_pcb, p, &g_relay, ARGUS_REPLAY_PORT);
    pbuf_free(p);

    return (err == ERR_OK) ? 0 : -1;
}

/* lwIP raw-API receive callback for replay chunks.
 *
 * Flattens the pbuf into a contiguous scratch buffer before handing
 * it to argus_replay_client_on_packet(). A chunk is 1372 bytes(one
 * MTU), but not necessarily one pbuf, since PBUF_POOL_BUFSIZE
 * governs that and the Xilinx port does chain. Parsing p->payload
 * directly would read past the end of the first link and reject
 * every full-size chunk.
 *
 * NOTE: Owns the pbuf: mus pbuf_free() on every path, including
 * early returns. */
static void replay_recv(void *arg, struct udp_pcb *pcb,
    struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;

    if (p == NULL) {
        return;
    }

    if (p->tot_len <= sizeof(g_rx)) {
        pbuf_copy_partial(p, g_rx, p->tot_len, 0);
        argus_replay_client_on_packet(&g_client, g_rx, (uint16_t)p->tot_len);
    }
    /* Oversized datagrams are dropped without counting: they cannot be ours,
     * and the client's counters should reflect protocol faults only. */

    pbuf_free(p);
}

/* Global routines */

int argus_replay_init(void)
{
    g_pcb = udp_new();
    if (g_pcb == NULL) {
        return -1;
    }

    if (udp_bind(g_pcb, IP_ADDR_ANY, ARGUS_REPLAY_LOCAL_PORT) != ERR_OK) {
        return -1;
    }
    udp_recv(g_pcb, replay_recv, NULL);

    IP4_ADDR(&g_relay, ARGUS_RELAY_IP_A, ARGUS_RELAY_IP_B,
             ARGUS_RELAY_IP_C, ARGUS_RELAY_IP_D);

    return argus_replay_client_init(&g_client, replay_send, NULL,
                                    ARGUS_MAX_CHANNELS,
                                    ARGUS_REPLAY_TIMEOUT_MS,
                                    ARGUS_REPLAY_MAX_RETRIES);
}

int argus_replay_start_fetch(uint32_t sample_offset)
{
    return argus_replay_client_fetch(&g_client, sample_offset,
                                     ARGUS_REPLAY_SAMPLES_PER_HALF,
                                     g_buffer, argus_now_ms());
}

/* Drives the client's retransmit deadlines. Must be called from the
 * main loop since the client has no timer of its own and only
 * re-requests missing chunks when polled, so a dropped chunk stalls
 * the fetch until this runs.
 *
 * Uses the Cortex-A9 global timer rather than sys_now(), so lwIP's
 * timer configuration cannot silently disable recovery. */
void argus_replay_service(void)
{
    argus_replay_client_poll(&g_client, argus_now_ms());
}

/* True once the fetch has finished, either way: COMPLETE or FAILED.
 * Callers must check argus_replay_succeeded() to tell them apart: a
 * loop that stops here without checking will happily read a half
 * -filled buffer. */
int argus_replay_is_done(void)
{
    return argus_replay_client_is_done(&g_client);
}

/* True only for a fetch that completed. is_done() is also true
 * after the client gives up, so check this before reading the
 * buffer. A failed fetch leaves it holding whichever chunks did
 * arrive, which looks like valid data until you reach a gap. */
int argus_replay_succeeded(void)
{
    return g_client.state == ARGUS_REPLAY_COMPLETE;
}

const uint16_t *argus_replay_buffer(void)
{
    return g_buffer;
}

void argus_replay_report(void)
{
    xil_printf("replay: req=%d rtx=%d ok=%d rej=%d to=%d chunks=%d/%d\r\n",
               (int)g_client.requests_sent,
               (int)g_client.retransmits_sent,
               (int)g_client.chunks_accepted,
               (int)g_client.chunks_rejected,
               (int)g_client.timeouts,
               (int)g_client.arrived_count,
               (int)g_client.total_chunks);
}
