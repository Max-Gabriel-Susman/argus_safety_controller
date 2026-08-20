

#ifndef ARGUS_CORE_ARGUS_WIRE_H
    #define ARGUS_CORE_ARGUS_WIRE_H
    #include <stdint.h>
    #include <assert.h>
    #include <stddef.h>

    /* Wire contract betweeen the Zynq safety controller and argus_sensors.
     * Both ends must agree. Bump ARGUS_FRAME_VERSION on any layout change. */
    #define ARGUS_FRAME_MAGIC 0x41524753u /* 'A''R''G''S' */
    #define ARGUS_FRAME_VERSION 2
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

    /* --- Replay transport: host -> PS dataset delivery ---------------------
    * Separate port from telemetry so the two flows can be captured
    * independently and neither receiver sees traffic it cannot parse.
    * Both ends are little-endian; no byte-order conversion is performed. */
    #define ARGUS_REPLAY_PORT 5010u
    #define ARGUS_REPLAY_VERSION 1u

    #define ARGUS_MSG_REPLAY_REQUEST 0x0010u
    #define ARGUS_MSG_REPLAY_CHUNK 0x0011u

    /* 1500 MTU - 20 IP - 8 UDP - 28 chunk header */
    #define ARGUS_REPLAY_MAX_PAYLOAD 1444u
    #define ARGUS_REPLAY_SAMPLES_PER_CHUNK 7u /* at 96 channels */
    #define ARGUS_REPLAY_CHUNKS_PER_HALF 21u
    #define ARGUS_REPLAY_SAMPLES_PER_HALF (ARGUS_REPLAY_SAMPLES_PER_CHUNK * ARGUS_REPLAY_CHUNKS_PER_HALF)

    #define ARGUS_REPLAY_F_RETRANSMIT 0x0001u /* gap fill; does not advance play */
    #define ARGUS_REPLAY_F_LOOP       0x0002u /* wrap at end of dataset */

    typedef struct __attribute__((packed)) {
        uint32_t magic;         /* ARGUS_FRAME_MAGIC */
        uint16_t version;       /* ARGUS_REPLAY_VERSION */
        uint16_t type;          /* ARGUS_MSG_REPLAY_REQUEST */
        uint32_t seq;           /* echoed by every chunk of the response */
        uint32_t sample_offset; /* absolute index into the dataset */
        uint16_t sample_count;
        uint16_t flags;
        uint16_t crc;           /* CRC-16/CCITT bytes [0,crc) */
    } argus_replay_request_t;

    static_assert(sizeof(argus_replay_request_t) == 22, "replay request size drift");

    typedef struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t version;
        uint16_t type;          /* ARGUS_MSG_REPLAY_CHUNK */
        uint32_t seq;
        uint32_t sample_offset;  /* absolute index of first sample here */
        uint16_t sample_count;
        uint16_t chunk_index;   /* 0 .. chunk_total-1 */
        uint16_t chunk_total;   /* burst length, so gaps are detectable */
        uint16_t channel_count; /* PS validates rather than assumes */
        uint16_t payload_len;
        uint16_t crc;           /* header bytes [0,crc) then payload */
        /* uint16_t samples[sample_count][channel_count], sample-major */
    } argus_replay_chunk_hdr_t;

    static_assert(sizeof(argus_replay_chunk_hdr_t) == 28, "replay chunk header size drift");

    /* Resumable variant, so a CRC can span a header and a payload that are not
     * contiguous. crc16_ccitt() above is unchanged; existing callers are unaffected. */
     static inline uint16_t crc16_ccitt_update(uint16_t crc, const uint8_t *d, size_t n)
     {
        for (size_t i = 0; i < n; i++) {
            crc ^= (uint16_t)d[i] << 8;
            for (int b = 0; b < 8; b++) {
                crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
            }
        }
        return crc;
     }
#endif /* ARGUS_CORE_ARGUS_WIRE_H */