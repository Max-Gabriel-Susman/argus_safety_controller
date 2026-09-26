#include "xil_printf.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "sleep.h"
#include "netif/xadapter.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "argus_net.h"
#include "argus_wire.h"
#include "argus_acq.h"
#include "argus_replay.h"

#define PUBLISH_PERIOD_MS 50

/* Bounded wait for the first replay request to leave. lwIP queues packets
 * for an unresolved destination when ARP_QUEUEING is on, so this normally
 * succeeds first try; it only matters if queueing is disabled. Pumping the
 * stack between attempts is what lets ARP complete either way. */
#define REPLAY_SEND_ATTEMPTS 200
#define REPLAY_SEND_GAP_US   10000

/* Telemetry frames between status lines. */
#define STATUS_EVERY_FRAMES  100

static struct netif server_netif;
static unsigned char mac_ethernet_address[] = {0x00,0x0a,0x35,0x00,0x01,0x02};

/* One buffer half in DDR, for the one-shot network check only. Streaming
 * fetches land in BRAM directly. */
static uint16_t g_fetch_buf[ARGUS_REPLAY_SAMPLES_PER_HALF * ARGUS_MAX_CHANNELS];

/* --- acquisition chain checks ------------------------------------------- */

/* Seqlock read of one frame, aligned to the bank swap.
 *
 * The assembler swaps banks every sweep (33.3 us). 96 GP0 reads from the A9
 * at -O0 come to roughly 32 us -- inside the window, but only just, so the
 * read must start the moment a swap happens rather than at a random point
 * in the window. Spin until FRAME_INDEX changes, then read; if it changed
 * again by the end, the read was too slow this time and is retried.
 *
 * A read that is consistently too slow for the window is a hardware
 * problem (the register block should hold the bank while the PS reads),
 * not something more attempts can fix. */
#define ACQ_READ_ATTEMPTS 8
#define ACQ_SWAP_SPIN_MAX 200000u

static int acq_read_frame(uint16_t *out, uint32_t *frame_index)
{
    for (int attempt = 0; attempt < ACQ_READ_ATTEMPTS; attempt++) {
        uint32_t fi_prev = argus_acq_rd(ARGUS_ACQ_FRAME_INDEX);
        uint32_t fi0     = fi_prev;
        uint32_t spins   = 0;

        while (fi0 == fi_prev) {
            fi0 = argus_acq_rd(ARGUS_ACQ_FRAME_INDEX);
            if (++spins > ACQ_SWAP_SPIN_MAX) {
                return -2;   /* frames not advancing at all */
            }
        }

        for (int n = 0; n < ARGUS_MAX_CHANNELS; n++) {
            out[n] = (uint16_t)argus_acq_rd(ARGUS_ACQ_FRAME_BASE + 4u * (uint32_t)n);
        }

        uint32_t fi1 = argus_acq_rd(ARGUS_ACQ_FRAME_INDEX);
        if (fi0 == fi1) {
            *frame_index = fi1;
            return 0;
        }
    }
    return -1;
}

/* Every word must carry its own chip and channel, and the whole frame must
 * share one sample index. That holds for both sources: the chips' built-in
 * IDENT pattern and the relay's synthetic one use the same layout.
 *
 * With the built-in source the index is the chips' sweep counter, which
 * must agree with the assembler's frame counter modulo 256. With the
 * external source it is the dataset sample number and that cross-check
 * does not apply. */
static void acq_frame_check(int ext)
{
    uint16_t frame[ARGUS_MAX_CHANNELS];
    uint32_t fi;

    int rc = acq_read_frame(frame, &fi);
    if (rc == -2) {
        xil_printf("acq frame: FRAME_INDEX not advancing\r\n");
        return;
    }
    if (rc != 0) {
        xil_printf("acq frame: torn %d times even aligned to the swap\r\n",
                   ACQ_READ_ATTEMPTS);
        return;
    }

    uint8_t idx = (uint8_t)(frame[0] & 0xFFu);
    int bad = 0;
    for (int n = 0; n < ARGUS_MAX_CHANNELS; n++) {
        uint16_t want = (uint16_t)(((n / ARGUS_ACQ_CH_PER_CHIP) << 14)
                                 | ((n % ARGUS_ACQ_CH_PER_CHIP) << 8)
                                 | idx);
        if (frame[n] != want) {
            bad++;
        }
    }

    if (ext) {
        xil_printf("acq frame %u: [0]=%04x [%d]=%04x idx=%02x (ext) bad=%d\r\n",
                   (unsigned)fi, frame[0],
                   ARGUS_MAX_CHANNELS - 1, frame[ARGUS_MAX_CHANNELS - 1],
                   idx, bad);
    } else {
        xil_printf("acq frame %u: [0]=%04x [%d]=%04x idx=%02x expect=%02x bad=%d\r\n",
                   (unsigned)fi, frame[0],
                   ARGUS_MAX_CHANNELS - 1, frame[ARGUS_MAX_CHANNELS - 1],
                   idx, (unsigned)((fi - 1u) & 0xFFu), bad);
    }
}

/* Runs before the network stack on purpose: an AXI read against
 * unprogrammed PL hangs the A9 with no timeout, and if that happens it
 * should be the first thing after the banner, not buried under lwIP output.
 * Three lines prove the 125 MHz clock, reset release, the init sequence,
 * the SPI sweep, the assembler, and the AXI path. */
static void acq_smoke_test(void)
{
    uint32_t id = argus_acq_rd(ARGUS_ACQ_ID);
    xil_printf("acq id=%08x%s\r\n", (unsigned)id,
               (id == ARGUS_ACQ_ID_EXPECT) ? "" : "  (expected 41435131)");

    argus_acq_wr(ARGUS_ACQ_CTRL, ARGUS_ACQ_CTRL_ENABLE);
    usleep(50000);

    uint32_t f0 = argus_acq_rd(ARGUS_ACQ_FRAME_INDEX);
    usleep(1000000);
    uint32_t f1 = argus_acq_rd(ARGUS_ACQ_FRAME_INDEX);

    xil_printf("acq status=%08x frames/s=%u\r\n",
               (unsigned)argus_acq_rd(ARGUS_ACQ_STATUS),
               (unsigned)(f1 - f0));

    acq_frame_check(0);
}

/* --- main ----------------------------------------------------------------- */

int main(void)
{
    ip_addr_t ipaddr, netmask, gw;
    uint16_t channels[ARGUS_MAX_CHANNELS];
    uint32_t sample = 0;
    int attempt;

    Xil_DCacheDisable();   /* simplest correct choice for bring-up */

    xil_printf("Initializing Argus Safety Controller...\r\n");

    acq_smoke_test();

    IP4_ADDR(&ipaddr,  192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    lwip_init();

    if (!xemac_add(&server_netif, &ipaddr, &netmask, &gw,
                   mac_ethernet_address, XPAR_XEMACPS_0_BASEADDR)) {
        xil_printf("ERROR: xemac_add failed\r\n");
        return -1;
    }
    netif_set_default(&server_netif);
    netif_set_up(&server_netif);

    if (argus_net_init() != 0) {
        xil_printf("ERROR: argus_net_init failed\r\n");
        return -1;
    }

    if (argus_replay_init() != 0) {
        xil_printf("ERROR: argus_replay_init failed\r\n");
        return -1;
    }

    /* One-shot fetch into DDR: proves the host->PS path before anything
     * depends on it. */
    for (attempt = 0; attempt < REPLAY_SEND_ATTEMPTS; attempt++) {
        if (argus_replay_start_fetch(0, g_fetch_buf) == 0) {
            break;
        }
        xemacif_input(&server_netif);
        sys_check_timeouts();
        usleep(REPLAY_SEND_GAP_US);
    }

    if (attempt >= REPLAY_SEND_ATTEMPTS) {
        xil_printf("ERROR: replay fetch request never left the board\r\n");
    } else {
        while (!argus_replay_is_done()) {
            xemacif_input(&server_netif);
            sys_check_timeouts();
            argus_replay_service();
        }
        if (argus_replay_succeeded()) {
            xil_printf("replay ok: [0][0]=%04x [0][5]=%04x [1][0]=%04x [146][95]=%04x\r\n",
                g_fetch_buf[0], g_fetch_buf[5],
                g_fetch_buf[ARGUS_MAX_CHANNELS],
                g_fetch_buf[146 * ARGUS_MAX_CHANNELS + 95]);
        } else {
            xil_printf("replay FAILED\r\n");
        }
        argus_replay_report();
    }

    /* Streaming: prime both BRAM halves, switch the chips to the external
     * source, then keep the PL fed from the main loop. */
    argus_replay_stream_begin(0);
    xil_printf("stream: priming\r\n");

    xil_printf("Argus Safety Controller initialized. IP 192.168.1.10\r\n");

    while (1) {
        xemacif_input(&server_netif);   /* required even TX-only: ARP */
        sys_check_timeouts();
        argus_replay_stream_service();

        /* crude pacing for bring-up; replace with XTime_GetTime() */
        static volatile uint32_t tick = 0;
        if (++tick > 200000) {
            tick = 0;
            for (int i = 0; i < ARGUS_MAX_CHANNELS; i++) {
                channels[i] = (uint16_t)(2048 + (sample * 7 + i * 13) % 1500);
            }
            argus_send_frame(sample, (float)sample * 0.05f, channels);
            if ((sample % 20) == 0) {
                xil_printf("tx %d\r\n", (int)sample);
            }
            if ((sample % STATUS_EVERY_FRAMES) == 0) {
                argus_replay_stream_report();
                acq_frame_check(argus_acq_rd(ARGUS_ACQ_CTRL) & ARGUS_ACQ_CTRL_EXT_MODE);
            }
            sample++;
        }
    }
    return 0;
}
