/* argus_acq.h
 *
 * PS-side view of argus_acq_top: register offsets, bit definitions, and the
 * replay BRAM layout. Mirrors the register map comment in argus_acq_axi.vhd;
 * if the two disagree, the hardware wins and this is wrong.
 *
 * Base addresses are pinned in the block design (tools/bd_add_acq.tcl and
 * tools/bd_add_bram.tcl) so they survive regeneration. They are hardcoded
 * here rather than taken from xparameters.h because module references get
 * no driver entry in the SDT flow.
 */

#ifndef ARGUS_ACQ_H
#define ARGUS_ACQ_H

#include <stdint.h>
#include "xil_io.h"
#include "argus_wire.h"

/* --- register block ------------------------------------------------------ */

#define ARGUS_ACQ_BASE            0x43C00000u

#define ARGUS_ACQ_CTRL            (ARGUS_ACQ_BASE + 0x000u)
#define ARGUS_ACQ_STATUS          (ARGUS_ACQ_BASE + 0x004u)
#define ARGUS_ACQ_FRAME_INDEX     (ARGUS_ACQ_BASE + 0x008u)
#define ARGUS_ACQ_ID              (ARGUS_ACQ_BASE + 0x00Cu)
#define ARGUS_ACQ_REPLAY_STATUS   (ARGUS_ACQ_BASE + 0x010u)
#define ARGUS_ACQ_REPLAY_ACK      (ARGUS_ACQ_BASE + 0x014u)
#define ARGUS_ACQ_FRAME_BASE      (ARGUS_ACQ_BASE + 0x100u)

#define ARGUS_ACQ_ID_EXPECT       0x41435131u   /* "ACQ1" */
#define ARGUS_ACQ_CH_PER_CHIP     32

/* CTRL */
#define ARGUS_ACQ_CTRL_ENABLE     (1u << 0)
#define ARGUS_ACQ_CTRL_SOFT_RESET (1u << 1)
#define ARGUS_ACQ_CTRL_EXT_MODE   (1u << 2)
/* Freeze the frame read bank. The assembler keeps sweeping and discards
 * what it completes, so hold for as long as a coherent read needs and no
 * longer. FRAME_INDEX freezes with it and names the frame being read. */
#define ARGUS_ACQ_CTRL_HOLD       (1u << 3)

/* STATUS */
#define ARGUS_ACQ_STATUS_READY    (1u << 0)
#define ARGUS_ACQ_STATUS_OVERRUN  (1u << 1)
/* The freeze is in effect and the bank has settled. Poll this after
 * setting CTRL.hold; do not assume the write took immediately. */
#define ARGUS_ACQ_STATUS_HELD     (1u << 2)

/* REPLAY_STATUS */
#define ARGUS_ACQ_RS_PLAY_HALF    (1u << 0)
#define ARGUS_ACQ_RS_CONSUMED0    (1u << 1)
#define ARGUS_ACQ_RS_CONSUMED1    (1u << 2)
#define ARGUS_ACQ_RS_UNDERRUN     (1u << 3)
#define ARGUS_ACQ_RS_ROW(v)       (((v) >> 8) & 0xFFFFu)

/* REPLAY_ACK -- write 1 to clear */
#define ARGUS_ACQ_RA_CONSUMED0    (1u << 0)
#define ARGUS_ACQ_RA_CONSUMED1    (1u << 1)
#define ARGUS_ACQ_RA_UNDERRUN     (1u << 2)

/* --- replay BRAM ---------------------------------------------------------- */

/* Two halves, sample-major channel-minor, matching the relay's chunk payload
 * so chunks are copied in unchanged. Sized for ARGUS_REPLAY_SAMPLES_PER_HALF
 * rows of ARGUS_MAX_CHANNELS 16-bit samples. */
#define ARGUS_BRAM_BASE           0x40000000u
#define ARGUS_BRAM_SIZE           0x10000u
#define ARGUS_BRAM_HALF_BYTES     (ARGUS_REPLAY_SAMPLES_PER_HALF * ARGUS_MAX_CHANNELS * 2u)
#define ARGUS_BRAM_HALF(h)        ((uint16_t *)(ARGUS_BRAM_BASE + (uint32_t)(h) * ARGUS_BRAM_HALF_BYTES))

#if (2 * ARGUS_REPLAY_SAMPLES_PER_HALF * ARGUS_MAX_CHANNELS * 2) > 0x10000
#error "two replay halves do not fit the 64 KB BRAM aperture"
#endif

/* --- helpers -------------------------------------------------------------- */

static inline uint32_t argus_acq_rd(uint32_t reg)              { return Xil_In32(reg); }
static inline void     argus_acq_wr(uint32_t reg, uint32_t v)  { Xil_Out32(reg, v); }

#endif /* ARGUS_ACQ_H */
