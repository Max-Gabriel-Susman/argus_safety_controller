#include "xil_printf.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "sleep.h"
#include "netif/xadapter.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "argus_net.h"
#include "argus_wire.h"
#include "argus_replay.h"

#define PUBLISH_PERIOD_MS 50

/* Bounded wait for the first replay request to leave. lwIP queues packets
 * for an unresolved destination when ARP_QUEUEING is on, so this normally
 * succeeds first try; it only matters if queueing is disabled. Pumping the
 * stack between attempts is what lets ARP complete either way. */
#define REPLAY_SEND_ATTEMPTS 200
#define REPLAY_SEND_GAP_US   10000

/* Acquisition chain register block, argus_acq_top on M_AXI_GP0. The base
 * is pinned in the block design (tools/bd_add_acq.tcl) so this stays valid
 * across regenerations. Hardcoded rather than taken from xparameters.h:
 * module references get no driver entry in the SDT flow, so there is
 * nothing there to take it from. */
#define ARGUS_ACQ_BASE    0x43C00000u
#define ARGUS_ACQ_CTRL    (ARGUS_ACQ_BASE + 0x000u)
#define ARGUS_ACQ_STATUS  (ARGUS_ACQ_BASE + 0x004u)
#define ARGUS_ACQ_FRAME   (ARGUS_ACQ_BASE + 0x008u)
#define ARGUS_ACQ_ID      (ARGUS_ACQ_BASE + 0x00Cu)

#define ARGUS_ACQ_ID_EXPECT 0x41435131u   /* "ACQ1" */

static struct netif server_netif;
static unsigned char mac_ethernet_address[] = {0x00,0x0a,0x35,0x00,0x01,0x02};

/* Acquisition chain smoke test. Runs before the network stack on purpose:
 * an AXI read against unprogrammed PL hangs the A9 with no timeout, and if
 * that happens it should be the first thing after the banner, not buried
 * under lwIP output. Three lines prove the 125 MHz clock, reset release,
 * the init sequence, the SPI sweep, the assembler, and the AXI path. */
static void acq_smoke_test(void)
{
    uint32_t id = Xil_In32(ARGUS_ACQ_ID);
    xil_printf("acq id=%08x%s\r\n", (unsigned)id,
               (id == ARGUS_ACQ_ID_EXPECT) ? "" : "  (expected 41435131)");

    Xil_Out32(ARGUS_ACQ_CTRL, 1u);   /* enable, no soft reset */
    usleep(50000);

    uint32_t f0 = Xil_In32(ARGUS_ACQ_FRAME);
    usleep(1000000);
    uint32_t f1 = Xil_In32(ARGUS_ACQ_FRAME);

    xil_printf("acq status=%08x frames/s=%u\r\n",
               (unsigned)Xil_In32(ARGUS_ACQ_STATUS),
               (unsigned)(f1 - f0));
}

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

    /* Creates the replay PCB, binds it, registers the receive callback and
     * initialises the client. Without this the client has no send hook and
     * every fetch fails immediately. */
    if (argus_replay_init() != 0) {
        xil_printf("ERROR: argus_replay_init failed\r\n");
        return -1;
    }

    /* One-shot fetch, before the telemetry loop. Proves the host->PS path:
     * request out, chunks in, identity pattern intact. */
    for (attempt = 0; attempt < REPLAY_SEND_ATTEMPTS; attempt++) {
        if (argus_replay_start_fetch(0) == 0) {
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
            const uint16_t *b = argus_replay_buffer();
            xil_printf("replay ok: [0][0]=%04x [0][5]=%04x [1][0]=%04x [146][95]=%04x\r\n",
                b[0], b[5],
                b[ARGUS_MAX_CHANNELS],
                b[146 * ARGUS_MAX_CHANNELS + 95]);
        } else {
            xil_printf("replay FAILED\r\n");
        }
        argus_replay_report();
    }

    xil_printf("Argus Safety Controller initialized. IP 192.168.1.10\r\n");

    while (1) {
        xemacif_input(&server_netif);   /* required even TX-only: ARP */
        sys_check_timeouts();

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
            sample++;
        }
    }
    return 0;
}
