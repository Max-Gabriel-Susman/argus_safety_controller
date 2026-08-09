/* VENDORED — do not edit here.
 * Canonical source: argus_core/include/argus_core/argus_wire.h
 * Re-vendor with:
 *   cp ../../../argus_core/include/argus_core/argus_wire.h .
 * Any change must land in argus_core first, and argus_sensors on the Orin
 * must be updated in the same change — all three parse the same frames.
 */
#ifndef ARGUS_CORE_ARGUS_WIRE_H
    #define ARGUS_CORE_ARGUS_WIRE_H

    #include <stdint.h>
    #include <assert.h>

    /* Wire contract betweeen the Zynq safety controller and argus_sensors.
     * Both ends must agree. Bump ARGUS_FRAME_VERSION on any layout change. */

    #define ARGUS_FRAME_MAGIC 0x41524753u /* 'A''R''G''S' */
    #define ARGUS_FRAME_VERSION 1
    #define ARGUS_MAX_CHANNELS 96
    #define ARGUS_UDP_PORT 5005

    typedef struct __attribute__((packed)) {
        uint32_t magic;
        uint32_t sample;
        float    t;                      /* s since fw start */              
        uint8_t  version;
        uint8_t  reserved;
        uint16_t channel_count;
        uint16_t channels[ARGUS_MAX_CHANNELS];
        uint16_t crc;                    /* CRC-16/CCITT bytes [0,crc) */
    } argus_frame_packet_t;
    
    static_assert(sizeof(argus_frame_packet_t) == 210, "frame size drift");

    
    // cyclic redundancy check
    static inline uint16_t crc16_ccitt(const uint8_t *d, size_t n)
    {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < n; i++) {
            crc ^= (uint16_t)d[i] << 8;
            for (int b = 0; b < 8; b++) {
                crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
            }
        }
        return crc;
    }
#endif /* ARGUS_CORE_ARGUS_WIRE_H */