/* argus_replay.c -- see argus_replay.h */

#include <string.h>
#include <stdint.h>

#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "xil_printf.h"
#include "xtime_l.h"

#include "argus_wire.h"
#include "argus_replay_client.h"
#include "argus_acq.h"
#include "argus_replay.h"

/* Relay host. Same machine as the telemetry destination in argus_net.c;
 * if that address moves, both must move together. */
#define ARGUS_RELAY_IP_A 192
#define ARGUS_RELAY_IP_B 168
#define ARGUS_RELAY_IP_C 1
#define ARGUS_RELAY_IP_D 20

#define ARGUS_REPLAY_TIMEOUT_MS 200u
#define ARGUS_REPLAY_MAX_RETRIES 5u

static struct udp_pcb *g_pcb;
static ip_addr_t g_relay;
static argus_replay_client_t g_client;

/* Scratch for flattening a possibly-chained pbuf. A chunk is 1372 bytes,
 * which fits one MTU but not necessarily one pbuf: PBUF_POOL_BUFSIZE governs
 * that, and the Xilinx port does chain. Parsing p->payload directly would
 * read past the end of the first link and reject every full-size chunk. */
static uint8_t g_rx[sizeof(argus_replay_chunk_hdr_t) + ARGUS_REPLAY_MAX_PAYLOAD];

/* --- time ---------------------------------------------------------------- */

/* The Cortex-A9 global timer, not sys_now(). Independent of lwIP's timer
 * configuration, so a stalled tick cannot silently disable retransmits --
 * which would make a dropped chunk look like a dead relay. */
static uint32_t argus_now_ms(void)
{
    XTime t;
    XTime_GetTime(&t);
    return (uint32_t)(t / (COUNTS_PER_SECOND / 1000U));
}

/* --- transport ----------------------------------------------------------- */

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

static void replay_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
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

    pbuf_free(p);
}

/* --- one-shot -------------------------------------------------------------- */

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

int argus_replay_start_fetch(uint32_t sample_offset, uint16_t *dst)
{
    return argus_replay_client_fetch(&g_client, sample_offset,
                                     ARGUS_REPLAY_SAMPLES_PER_HALF,
                                     dst, argus_now_ms());
}

void argus_replay_service(void)
{
    argus_replay_client_poll(&g_client, argus_now_ms());
}

int argus_replay_is_done(void)
{
    return argus_replay_client_is_done(&g_client);
}

int argus_replay_succeeded(void)
{
    return g_client.state == ARGUS_REPLAY_COMPLETE;
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

/* --- streaming ------------------------------------------------------------- */

/* The PL plays one half while the PS refills the other. The PL reports a
 * half consumed when it flips out of it; the PS fetches the next block of
 * samples straight into that half -- the client's memcpy lands in BRAM
 * over AXI -- and acks. If the PL flips back into a half before its ack,
 * it sets underrun and keeps playing stale data; counted here, not fatal.
 *
 * At most one fetch is in flight. The request cadence is one half every
 * 147 sweeps (4.9 ms at 30 kS/s), and the measured delivery time for a
 * half is around 4.3 ms, so this is not generous. Underruns here are the
 * signal to widen the link, not a bug in this file. */

typedef enum {
    STREAM_OFF,
    STREAM_PRIME0,   /* fetching half 0 */
    STREAM_PRIME1,   /* fetching half 1 */
    STREAM_RUN
} stream_state_t;

static stream_state_t g_stream = STREAM_OFF;
static uint32_t g_next_sample;    /* dataset offset of the next fetch */
static int      g_in_flight;      /* half being fetched, or -1 */

static uint32_t g_halves_filled;
static uint32_t g_underruns;
static uint32_t g_fetch_failures;

static int stream_fetch_into(int half)
{
    if (argus_replay_start_fetch(g_next_sample, ARGUS_BRAM_HALF(half)) != 0) {
        g_fetch_failures++;
        return -1;
    }
    g_in_flight = half;
    return 0;
}

static void stream_enter_ext_mode(void)
{
    /* Soft reset with ext_mode set, then release: playback starts at row 0
     * of half 0 with no partial first frame. */
    argus_acq_wr(ARGUS_ACQ_CTRL, ARGUS_ACQ_CTRL_SOFT_RESET | ARGUS_ACQ_CTRL_EXT_MODE);
    argus_acq_wr(ARGUS_ACQ_REPLAY_ACK,
                 ARGUS_ACQ_RA_CONSUMED0 | ARGUS_ACQ_RA_CONSUMED1 | ARGUS_ACQ_RA_UNDERRUN);
    argus_acq_wr(ARGUS_ACQ_CTRL, ARGUS_ACQ_CTRL_ENABLE | ARGUS_ACQ_CTRL_EXT_MODE);
}

void argus_replay_stream_begin(uint32_t first_sample)
{
    g_next_sample    = first_sample;
    g_in_flight      = -1;
    g_halves_filled  = 0;
    g_underruns      = 0;
    g_fetch_failures = 0;

    if (stream_fetch_into(0) == 0) {
        g_stream = STREAM_PRIME0;
    } else {
        /* Retried from the service loop. */
        g_stream = STREAM_PRIME0;
        g_in_flight = -1;
    }
}

void argus_replay_stream_service(void)
{
    uint32_t rs;

    if (g_stream == STREAM_OFF) {
        return;
    }

    argus_replay_service();

    switch (g_stream) {

    case STREAM_PRIME0:
        if (g_in_flight < 0) {
            stream_fetch_into(0);
        } else if (argus_replay_is_done()) {
            if (argus_replay_succeeded()) {
                g_halves_filled++;
                g_next_sample += ARGUS_REPLAY_SAMPLES_PER_HALF;
                g_in_flight = -1;
                if (stream_fetch_into(1) == 0) {
                    g_stream = STREAM_PRIME1;
                }
            } else {
                g_fetch_failures++;
                g_in_flight = -1;   /* retry same offset next call */
            }
        }
        break;

    case STREAM_PRIME1:
        if (g_in_flight < 0) {
            stream_fetch_into(1);
        } else if (argus_replay_is_done()) {
            if (argus_replay_succeeded()) {
                g_halves_filled++;
                g_next_sample += ARGUS_REPLAY_SAMPLES_PER_HALF;
                g_in_flight = -1;
                stream_enter_ext_mode();
                g_stream = STREAM_RUN;
            } else {
                g_fetch_failures++;
                g_in_flight = -1;
            }
        }
        break;

    case STREAM_RUN:
        rs = argus_acq_rd(ARGUS_ACQ_REPLAY_STATUS);

        if (rs & ARGUS_ACQ_RS_UNDERRUN) {
            g_underruns++;
            argus_acq_wr(ARGUS_ACQ_REPLAY_ACK, ARGUS_ACQ_RA_UNDERRUN);
        }

        if (g_in_flight < 0) {
            /* Refill whichever half the PL has finished with. Half 0 first
             * if both are pending, which only happens after an underrun. */
            if (rs & ARGUS_ACQ_RS_CONSUMED0) {
                stream_fetch_into(0);
            } else if (rs & ARGUS_ACQ_RS_CONSUMED1) {
                stream_fetch_into(1);
            }
        } else if (argus_replay_is_done()) {
            if (argus_replay_succeeded()) {
                g_halves_filled++;
                g_next_sample += ARGUS_REPLAY_SAMPLES_PER_HALF;
                argus_acq_wr(ARGUS_ACQ_REPLAY_ACK,
                             (g_in_flight == 0) ? ARGUS_ACQ_RA_CONSUMED0
                                                : ARGUS_ACQ_RA_CONSUMED1);
            } else {
                /* Same half, same offset, next time round. */
                g_fetch_failures++;
            }
            g_in_flight = -1;
        }
        break;

    case STREAM_OFF:
    default:
        break;
    }
}

void argus_replay_stream_report(void)
{
    uint32_t rs = argus_acq_rd(ARGUS_ACQ_REPLAY_STATUS);

    xil_printf("stream: halves=%u underruns=%u failures=%u next=%u"
               " pl: half=%u row=%u c0=%u c1=%u\r\n",
               (unsigned)g_halves_filled,
               (unsigned)g_underruns,
               (unsigned)g_fetch_failures,
               (unsigned)g_next_sample,
               (unsigned)(rs & ARGUS_ACQ_RS_PLAY_HALF),
               (unsigned)ARGUS_ACQ_RS_ROW(rs),
               (unsigned)((rs >> 1) & 1u),
               (unsigned)((rs >> 2) & 1u));
}
