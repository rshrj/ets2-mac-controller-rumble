// Packets sent from ets2_rumble_plugin (inside the game) to rumble_helper
// over localhost UDP. Both sides are little-endian 64-bit builds of the same
// struct definitions, so the structs are sent as-is. Bump RUMBLE_MAGIC when
// any struct changes so mismatched plugin/helper builds ignore each other.
#ifndef RUMBLE_PROTOCOL_H
#define RUMBLE_PROTOCOL_H

#include <stdint.h>

#define RUMBLE_PORT 47823
#define RUMBLE_MAGIC 0x32424d52u // "RMB2"
#define RUMBLE_MAX_WHEELS 16
#define RUMBLE_MAX_SUBSTANCES 64
#define RUMBLE_SUBSTANCE_NAME_LEN 32
#define RUMBLE_NO_SUBSTANCE 0xffffffffu

#define RUMBLE_FRAME_TIMER_RESTART 0x1u // SCS_TELEMETRY_FRAME_START_FLAG_timer_restart

enum {
    RUMBLE_PKT_FRAME = 1,      // one physics frame of truck state
    RUMBLE_PKT_SUBSTANCES = 2, // substance index -> name table
    RUMBLE_PKT_PAUSED = 3,     // game paused, stop rumbling
    RUMBLE_PKT_QUIT = 4,       // telemetry shut down, helper should exit
    RUMBLE_PKT_TRUCK = 5,      // truck configuration (wheel layout)
    RUMBLE_PKT_EVENT = 6,      // gameplay event (fine, ferry, ...)
    RUMBLE_PKT_GAME_VIBRATION = 7, // the game's own vibration command, intercepted from Steam Input
    RUMBLE_PKT_HOOK_STATS = 8,     // Steam Input hook counters, for diagnostics
};

struct rumble_pkt_header {
    uint32_t magic;
    uint32_t type;
};

// Vectors are vehicle space unless noted: x right, y up, z backward.
struct rumble_pkt_frame {
    struct rumble_pkt_header h;
    uint64_t sim_time_us;
    uint64_t render_time_us;
    uint64_t paused_sim_time_us;
    uint32_t frame_flags; // RUMBLE_FRAME_*
    uint32_t frame_index; // counts frame_end events since telemetry init

    double position[3];    // world space, m
    float orientation[3];  // heading, pitch, roll in rotations (0..1)
    float lin_vel[3];      // m/s
    float ang_vel[3];      // rotations/s
    float lin_acc[3];      // m/s^2
    float ang_acc[3];      // rotations/s^2
    float cabin_offset[3]; // cabin position relative to its default, m
    float cabin_rot[3];    // cabin heading, pitch, roll relative to default, rotations
    float cabin_ang_vel[3];
    float cabin_ang_acc[3];

    float speed; // m/s, negative when reversing
    float rpm;
    float throttle, brake, clutch, steering; // effective (after assists), 0..1 / -1..1
    float cruise_control;                    // m/s, 0 when off
    float wear_engine, wear_transmission, wear_cabin, wear_chassis, wear_wheels;
    float trailer_wear_chassis, trailer_wear_body, trailer_cargo_damage;
    int32_t gear; // engine gear, >0 forward, <0 reverse, 0 neutral
    uint32_t retarder;
    uint8_t engine_enabled;
    uint8_t parking_brake;
    uint8_t motor_brake;
    uint8_t trailer_connected;
    uint8_t wheel_count;
    uint8_t reserved[3];

    uint8_t on_ground[RUMBLE_MAX_WHEELS];
    float deflection[RUMBLE_MAX_WHEELS]; // suspension deflection, m
    float wheel_ang_vel[RUMBLE_MAX_WHEELS]; // rotations/s
    float wheel_steering[RUMBLE_MAX_WHEELS]; // rotations
    float wheel_lift[RUMBLE_MAX_WHEELS];     // 0 down .. 1 lifted
    uint32_t substance[RUMBLE_MAX_WHEELS];
};

struct rumble_pkt_substances {
    struct rumble_pkt_header h;
    uint32_t count;
    char names[RUMBLE_MAX_SUBSTANCES][RUMBLE_SUBSTANCE_NAME_LEN];
};

struct rumble_pkt_truck {
    struct rumble_pkt_header h;
    uint32_t wheel_count;
    char brand[32];
    char name[32];
    float wheel_radius[RUMBLE_MAX_WHEELS];
    float wheel_position[RUMBLE_MAX_WHEELS][3]; // vehicle space, m
    uint8_t wheel_powered[RUMBLE_MAX_WHEELS];
    uint8_t wheel_steerable[RUMBLE_MAX_WHEELS];
    uint8_t wheel_liftable[RUMBLE_MAX_WHEELS];
    uint8_t wheel_simulated[RUMBLE_MAX_WHEELS];
};

struct rumble_pkt_event {
    struct rumble_pkt_header h;
    char id[48];     // gameplay event id, e.g. "player.fined"
    char detail[64]; // e.g. fine offence
    int64_t amount;  // fine / payment amount
};

// Arguments of ISteamInput::TriggerVibration(Extended), 0..65535 each.
// Left is the heavy low-frequency motor, right the light high-frequency one.
struct rumble_pkt_game_vibration {
    struct rumble_pkt_header h;
    uint16_t left, right;
    uint16_t left_trigger, right_trigger; // 0 unless the Extended variant was used
};

struct rumble_pkt_hook_stats {
    struct rumble_pkt_header h;
    uint8_t installed;
    uint8_t reserved[3];
    uint32_t type_queries;     // GetInputTypeForHandle calls
    uint32_t type_overrides;   // ... of which we turned Unknown into a generic gamepad
    uint32_t vibration_calls;  // TriggerVibration(Extended) calls
    uint32_t hidden_polls;     // GetConnectedControllers calls answered with 0 (forced re-detection)
};

#endif
