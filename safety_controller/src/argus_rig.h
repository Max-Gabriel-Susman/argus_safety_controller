/* argus_rig.h
 *
 * Where each MISO lane comes from on this rig, applied by acq_smoke_test()
 * at boot. Edit by hand for now; argus_rig.py will generate this from a rig
 * profile once rig profiles exist. A -D on the compiler command line
 * overrides either value.
 *
 * ARGUS_LANE_SRC packs two bits per lane, lane c in bits 2c+1:2c:
 *   0  the chip model inside the fabric
 *   1  an external chip on Pmod JA
 *   2  off (the lane reads zeros)
 *
 * ARGUS_MISO_DELAY packs three bits per lane, lane c in bits 3c+2:3c: how
 * many fabric clocks (8 ns each) to delay that lane's MISO capture, 0 to 5.
 * Zero for model lanes. For a pins lane, boot once with the sweep below
 * enabled, read the console, and set the middle of the clean settings.
 *
 * Examples:
 *   all three lanes on the models (the default)    0x0, 0x0
 *   external chip on lane 0, delay 4                0x1, 0x4
 *   external chip on lane 0, lane 2 off             0x21, 0x4
 */

#ifndef ARGUS_RIG_H
#define ARGUS_RIG_H

#ifndef ARGUS_LANE_SRC
#define ARGUS_LANE_SRC   0x00000000u
#endif

#ifndef ARGUS_MISO_DELAY
#define ARGUS_MISO_DELAY 0x00000000u
#endif

/* Nonzero: at boot, before streaming, read every pins lane at each
 * MISO_DELAY from 0 to 5 and print how many words were wrong at each. The
 * clean settings form a window; pick its middle for ARGUS_MISO_DELAY. Has
 * no effect when no lane is on the pins. */
#ifndef ARGUS_MISO_SWEEP
#define ARGUS_MISO_SWEEP 1
#endif

#endif /* ARGUS_RIG_H */
