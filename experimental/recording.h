// Drive recordings written by rumble_helper and read by replay. A file is a
// rec_file_header followed by records: rec_header + `size` payload bytes.
// Game packets are stored raw, so recordings can be replayed through any
// later version of the model (as long as protocol.h is unchanged).
#ifndef RUMBLE_RECORDING_H
#define RUMBLE_RECORDING_H

#include <stdint.h>

#define REC_FILE_MAGIC "RMBREC02"

struct rec_file_header {
    char magic[8];
    uint32_t protocol_magic; // RUMBLE_MAGIC of the packets inside
    uint32_t frame_size;     // sizeof(struct rumble_pkt_frame)
    int64_t start_unix_us;   // wall clock when recording started
};

enum {
    REC_PACKET = 1, // payload: one UDP packet from the plugin, as received
    REC_RUMBLE = 2, // payload: struct rec_rumble, levels actually sent to the pad
    REC_MARKER = 3, // payload: struct rec_marker, button pressed by the driver
};

enum {
    REC_MARKER_MISSED = 1, // PL: "should have rumbled here"
    REC_MARKER_FALSE = 2,  // PR: "shouldn't have rumbled"
};

struct rec_header {
    uint32_t kind;
    uint32_t size;
    uint64_t host_us; // helper monotonic clock
};

struct rec_rumble {
    float low, high;
};

struct rec_marker {
    uint32_t kind;
};

#endif
