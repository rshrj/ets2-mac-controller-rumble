// Rumble model: turns telemetry frames into two motor levels. Pure logic with
// no I/O besides config loading, shared by rumble_helper (live) and replay
// (offline, on recorded drives) so both always run the same model.
#ifndef RUMBLE_EFFECTS_H
#define RUMBLE_EFFECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

struct fx_config {
    float master;
    float game;                              // gain for the game's own vibration (Steam Input hook)
    float bumps, surface, collisions, engine; // gains for our telemetry model, 0..2
    float low_min, high_min; // weakest motor level that is still felt (from --calibrate)
    int record;              // write recordings/drive-*.rec
};

// Per-frame internals, exposed for logging and offline analysis.
struct fx_debug {
    float bump_metric; // largest fast suspension movement among grounded wheels, m
    float jolt;        // horizontal acceleration change since last frame, m/s^2
    float crash_wear;  // crash-related damage gained this frame (0..1 scale)
    float dt;          // simulation step, s
    uint8_t reset;     // state was reset this frame (load, teleport, timer restart)
    uint8_t hit;       // a collision started this frame
};

struct fx_state {
    float roughness[RUMBLE_MAX_SUBSTANCES];

    bool have_prev;
    uint64_t prev_time_us;
    double prev_pos[3];
    float prev_crash_wear;
    float prev_acc[3];
    float defl_lp[RUMBLE_MAX_WHEELS];

    float settle;     // s left during which collisions are ignored (after resets)
    float refractory; // s left before another jolt-only collision can register

    // Effect envelopes, 0..1.
    float bump, collision, surface, engine;
    // Latest vibration command from the game itself, 0..1.
    float game_low, game_high;

    struct fx_debug dbg;
    uint32_t hits, resets;
};

void fx_config_defaults(struct fx_config *c);
// Returns true if the file was (re)loaded. Unknown keys are ignored.
bool fx_config_load(struct fx_config *c, const char *path);

void fx_init(struct fx_state *s);
// Returns the roughness assigned to substance `name`.
float fx_substance_roughness(const char *name);
void fx_set_substances(struct fx_state *s, const struct rumble_pkt_substances *p);
void fx_frame(struct fx_state *s, const struct rumble_pkt_frame *f);
void fx_game_vibration(struct fx_state *s, const struct rumble_pkt_game_vibration *p);
// Motor levels 0..1 including gains and deadband compensation.
void fx_output(const struct fx_state *s, const struct fx_config *c, float *low, float *high);
// Map a desired 0..1 strength onto the felt motor range [min, 1].
float fx_map_motor(float v, float min);

#endif
