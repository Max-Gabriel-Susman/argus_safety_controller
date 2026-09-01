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

     // needs impl.

#endif /* ARGUS_REPLAY_H */
