// needs impl.

#ifndef ARGUS_REPLAY_H
    #define ARGUS_REPLAY_H

    #include <stdint.h>

    /* Local UDP port the PS binds for replay traffic. Not part of
     * the wire contract; the relay answers whatever source address
     * a request came from. Explicit binding keeps captures readable. */
    #define ARGUS_REPLAY_LOCAL_PORT 5011u

    /* Creates the lwIP Protocol Control Block(PCB), binds it,
     * registers the receive callback, initializes the client.
     * Returns 0 on success. */
    int argus_replay_init(void);

    /* Issues a request for one buffer half starting at
     * sample_offset. Returns 0 if request went out. */
    int argus_replay_start_fetch(uint32_t sample_offset);

    /* Drive from the main loop. handles retransmit deadlines. */
    void argus_replay_service(void);

    int argus_replay_is_done(void);
    int argus_replay_succeeded(void);

    /* Sample-major, channel-minor: buffer[s * ARGUS_MAX_CHANNELS + C]. */
    const uint16_t *argus_replay_buffer(void);

    /* Prints client counters over UART. */
    void argus_replay_report(void);
     // needs impl.

#endif /* ARGUS_REPLAY_H */
