/* argus_console.h
 *
 * Non-blocking console output for the main loop.
 *
 * xil_printf waits on the UART: at 115200 8N1 a character takes about
 * 87 us, and the main loop has about 2.1 ms of slack per replay half, so
 * any line longer than about 24 characters costs the stream an underrun.
 * Here a message is formatted into a RAM ring at once and
 * argus_console_service() moves bytes into the 64-byte transmit FIFO only
 * while it has room, so no call ever waits on the wire.
 *
 * Main-context only: neither function is safe from an interrupt.
 */

#ifndef ARGUS_CONSOLE_H
#define ARGUS_CONSOLE_H

#include <stdint.h>

#define ARGUS_CONSOLE_RING_BYTES 4096u

/* Longest single message; anything longer is dropped and counted. */
#define ARGUS_CONSOLE_MSG_MAX 320u

/* Formats into the ring. If the whole message does not fit, none of it is
 * queued and the drop counter goes up. */
void argus_console_printf(const char *fmt, ...);

/* Writes queued bytes to the UART while its transmit FIFO has room. Call
 * once per main-loop pass. */
void argus_console_service(void);

/* Blocks until the ring is empty. For use before the main loop only, to
 * keep buffered lines in order with xil_printf output that follows. */
void argus_console_flush(void);

/* Messages dropped because they did not fit, since boot. */
uint32_t argus_console_drops(void);

/* Longest message queued since the last call, in bytes; resets it. */
uint32_t argus_console_take_max_msg(void);

#endif /* ARGUS_CONSOLE_H */
