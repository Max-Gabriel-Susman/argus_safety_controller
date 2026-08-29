/* Compiles the vendored wire header under the ARM toolchain so its
 * static_asserts fire for the Cortex-A9 ABI. Deliberately has no
 * main: this is a translation unit, not a program. The symbols
 * exist only to stop the compiler discarding the file wholesale. */
#include "argus_wire.h"

const size_t argus_selftest_frame_size   = sizeof(argus_frame_packet_t);
const size_t argus_selftest_request_size = sizeof(argus_replay_request_t);
const size_t argus_selftest_chunk_size   = sizeof(argus_replay_chunk_hdr_t);
