#include "xil_printf.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "netif/xadapter.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "argus_net.h"
#include "argus_wire.h"

#define PUBLISH_PERIOD_MS 50

static struct netif server_netif;
static unsigned char mac_ethernet_address[] = {0x00,0x0a,0x35,0x00,0x01,0x02};

int main(void)
{
    ip_addr_t ipaddr, netmask, gw;
    uint16_t channels[ARGUS_MAX_CHANNELS];
    uint32_t sample = 0;

    Xil_DCacheDisable();   /* simplest correct choice for bring-up */

    xil_printf("Initializing Argus Safety Controller...\r\n");

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
                xil_printf("tx %lu\r\n", (unsigned long)sample);
            }
            sample++;
        }
    }
    return 0;
}