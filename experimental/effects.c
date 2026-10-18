#include "effects.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// Bumps: suspension deflection is split into a slow part (body dive under
// braking, roll in corners, load) and a fast part (potholes, curbs, joints).
// Only the fast part rumbles.
#define BUMP_LOWPASS_S 0.10f // deflection slower than this counts as body motion
#define BUMP_DEADZONE 0.005f // fast deflection (m) below this is ignored
#define BUMP_RANGE 0.025f    // fast deflection above the deadzone for full strength
#define BUMP_DECAY_S 0.08f

// Collisions: new crash damage is the reliable signal; a big horizontal jolt
// alone also counts, once per refractory period.
#define CRASH_WEAR_MIN 0.0005f
#define JOLT_MIN 40.0f // m/s^2 change in one frame
#define JOLT_RANGE 80.0f
#define COLLISION_REFRACTORY_S 0.4f
#define COLLISION_DECAY_S 0.18f
#define SETTLE_S 1.0f // ignore collisions this long after a reset

// Surface rumble ramps up with speed, and is smoothed so wheels flickering
// across substance borders don't stutter.
#define SURFACE_FULL_KMH 60.0f
#define SURFACE_ATTACK_S 0.03f
#define SURFACE_RELEASE_S 0.08f

#define ENGINE_BASE 0.10f
#define MAX_STEP_S 0.25f        // longer gaps reset the state
#define TELEPORT_MIN_M 3.0f     // position jumps beyond this (plus travel) are teleports
#define OUTPUT_EPSILON 0.02f    // below this a motor stays off

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Exponential approach factor for time constant tau over step dt.
static float approach(float dt, float tau) { return 1.0f - expf(-dt / tau); }

void fx_config_defaults(struct fx_config *c) {
    c->master = c->game = c->bumps = c->surface = c->collisions = 1.0f;
    c->engine = 0.0f;
    c->low_min = c->high_min = 0.0f;
    c->record = 1;
}

bool fx_config_load(struct fx_config *c, const char *path) {
    static time_t last_mtime;
    static char last_path[1024];
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    if (st.st_mtime == last_mtime && !strcmp(path, last_path))
        return false;
    last_mtime = st.st_mtime;
    snprintf(last_path, sizeof last_path, "%s", path);

    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256], key[64];
    float value;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %f", key, &value) != 2)
            continue;
        float gain = clampf(value, 0.0f, 2.0f);
        if (!strcmp(key, "master")) c->master = gain;
        else if (!strcmp(key, "game")) c->game = gain;
        else if (!strcmp(key, "bumps")) c->bumps = gain;
        else if (!strcmp(key, "surface")) c->surface = gain;
        else if (!strcmp(key, "collisions")) c->collisions = gain;
        else if (!strcmp(key, "engine")) c->engine = gain;
        else if (!strcmp(key, "low_min")) c->low_min = clampf(value, 0.0f, 0.9f);
        else if (!strcmp(key, "high_min")) c->high_min = clampf(value, 0.0f, 0.9f);
        else if (!strcmp(key, "record")) c->record = value != 0.0f;
    }
    fclose(f);
    return true;
}

void fx_init(struct fx_state *s) { memset(s, 0, sizeof *s); }

float fx_substance_roughness(const char *name) {
    char lower[RUMBLE_SUBSTANCE_NAME_LEN];
    size_t i;
    for (i = 0; name[i] && i < sizeof lower - 1; i++)
        lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';

    // First match wins, so specific names go before generic ones.
    static const struct { const char *match; float value; } table[] = {
        {"rumble", 0.75f},
        {"road_dirt", 0.40f}, {"road_snow", 0.20f}, {"road_coarse", 0.08f},
        {"gravel", 0.45f}, {"dirt", 0.45f}, {"mud", 0.45f}, {"sand", 0.40f},
        {"grass", 0.30f}, {"snow", 0.25f}, {"field", 0.30f},
        {"cobble", 0.30f}, {"stone", 0.20f}, {"brick", 0.20f},
        {"metal", 0.15f}, {"wood", 0.15f}, {"plank", 0.20f},
    };
    for (size_t t = 0; t < sizeof table / sizeof *table; t++)
        if (strstr(lower, table[t].match))
            return table[t].value;
    return 0.0f;
}

void fx_set_substances(struct fx_state *s, const struct rumble_pkt_substances *p) {
    uint32_t count = p->count < RUMBLE_MAX_SUBSTANCES ? p->count : RUMBLE_MAX_SUBSTANCES;
    for (uint32_t i = 0; i < count; i++) {
        char name[RUMBLE_SUBSTANCE_NAME_LEN];
        memcpy(name, p->names[i], sizeof name);
        name[sizeof name - 1] = '\0';
        s->roughness[i] = fx_substance_roughness(name);
    }
}

static float crash_wear(const struct rumble_pkt_frame *f) {
    // Engine/transmission/wheel wear grows slowly with driving; these only
    // jump on impacts.
    return f->wear_cabin + f->wear_chassis + f->trailer_wear_chassis + f->trailer_wear_body +
           f->trailer_cargo_damage;
}

static void reset_state(struct fx_state *s, const struct rumble_pkt_frame *f, unsigned wheels) {
    s->have_prev = true;
    s->prev_time_us = f->sim_time_us;
    memcpy(s->prev_pos, f->position, sizeof s->prev_pos);
    memcpy(s->prev_acc, f->lin_acc, sizeof s->prev_acc);
    s->prev_crash_wear = crash_wear(f);
    for (unsigned i = 0; i < wheels; i++)
        s->defl_lp[i] = f->deflection[i];
    s->bump = s->collision = s->surface = 0.0f;
    s->settle = SETTLE_S;
    s->refractory = 0.0f;
    s->dbg.reset = 1;
    s->resets++;
}

void fx_frame(struct fx_state *s, const struct rumble_pkt_frame *f) {
    unsigned wheels = f->wheel_count < RUMBLE_MAX_WHEELS ? f->wheel_count : RUMBLE_MAX_WHEELS;
    memset(&s->dbg, 0, sizeof s->dbg);

    if (s->have_prev && f->sim_time_us == s->prev_time_us)
        return; // same physics step again

    float dt = s->have_prev ? (float)((double)(f->sim_time_us - s->prev_time_us) / 1e6) : 0.0f;
    if (!s->have_prev || (f->frame_flags & RUMBLE_FRAME_TIMER_RESTART) || f->sim_time_us < s->prev_time_us ||
        dt > MAX_STEP_S) {
        reset_state(s, f, wheels);
        return;
    }

    double dx = f->position[0] - s->prev_pos[0], dy = f->position[1] - s->prev_pos[1],
           dz = f->position[2] - s->prev_pos[2];
    float moved = (float)sqrt(dx * dx + dy * dy + dz * dz);
    if (moved > TELEPORT_MIN_M + fabsf(f->speed) * dt * 4.0f) {
        reset_state(s, f, wheels);
        return;
    }
    s->dbg.dt = dt;
    s->settle = fmaxf(0.0f, s->settle - dt);
    s->refractory = fmaxf(0.0f, s->refractory - dt);

    // Bumps.
    float bump_metric = 0.0f;
    float k = approach(dt, BUMP_LOWPASS_S);
    for (unsigned i = 0; i < wheels; i++) {
        s->defl_lp[i] += (f->deflection[i] - s->defl_lp[i]) * k;
        if (!f->on_ground[i])
            continue;
        float fast = fabsf(f->deflection[i] - s->defl_lp[i]);
        if (fast > bump_metric)
            bump_metric = fast;
    }
    s->dbg.bump_metric = bump_metric;
    float bump = clampf((bump_metric - BUMP_DEADZONE) / BUMP_RANGE, 0.0f, 1.0f);
    s->bump = fmaxf(bump, s->bump * expf(-dt / BUMP_DECAY_S));

    // Collisions.
    float wear = crash_wear(f);
    float dwear = wear - s->prev_crash_wear;
    float jx = f->lin_acc[0] - s->prev_acc[0], jz = f->lin_acc[2] - s->prev_acc[2];
    float jolt = sqrtf(jx * jx + jz * jz);
    s->dbg.jolt = jolt;
    s->dbg.crash_wear = dwear > 0.0f ? dwear : 0.0f;

    float hit = 0.0f;
    if (s->settle <= 0.0f) {
        if (dwear > CRASH_WEAR_MIN)
            hit = clampf(0.55f + dwear * 20.0f, 0.55f, 1.0f);
        if (jolt > JOLT_MIN && s->refractory <= 0.0f)
            hit = fmaxf(hit, clampf(0.4f + (jolt - JOLT_MIN) / JOLT_RANGE, 0.4f, 1.0f));
    }
    if (hit > 0.0f) {
        if (hit > s->collision + 0.1f && s->refractory <= 0.0f) {
            s->dbg.hit = 1;
            s->hits++;
        }
        s->refractory = COLLISION_REFRACTORY_S;
    }
    s->collision = fmaxf(hit, s->collision * expf(-dt / COLLISION_DECAY_S));

    // Surface.
    float rough = 0.0f;
    unsigned grounded = 0;
    for (unsigned i = 0; i < wheels; i++) {
        if (!f->on_ground[i])
            continue;
        grounded++;
        if (f->substance[i] < RUMBLE_MAX_SUBSTANCES)
            rough += s->roughness[f->substance[i]];
    }
    float kmh = fabsf(f->speed) * 3.6f;
    float surface = grounded ? (rough / grounded) * powf(clampf(kmh / SURFACE_FULL_KMH, 0.0f, 1.0f), 0.7f) : 0.0f;
    s->surface += (surface - s->surface) * approach(dt, surface > s->surface ? SURFACE_ATTACK_S : SURFACE_RELEASE_S);

    // Engine.
    s->engine = f->engine_enabled ? clampf(0.5f + f->rpm / 2500.0f, 0.0f, 1.5f) : 0.0f;

    memcpy(s->prev_pos, f->position, sizeof s->prev_pos);
    memcpy(s->prev_acc, f->lin_acc, sizeof s->prev_acc);
    s->prev_crash_wear = wear;
    s->prev_time_us = f->sim_time_us;
}

void fx_game_vibration(struct fx_state *s, const struct rumble_pkt_game_vibration *p) {
    s->game_low = p->left / 65535.0f;
    s->game_high = p->right / 65535.0f;
}

float fx_map_motor(float v, float min) {
    if (v < OUTPUT_EPSILON)
        return 0.0f;
    return clampf(min + (1.0f - min) * v, 0.0f, 1.0f);
}

void fx_output(const struct fx_state *s, const struct fx_config *c, float *low, float *high) {
    float l = c->bumps * s->bump * 0.9f + c->collisions * s->collision + c->engine * ENGINE_BASE * s->engine;
    float h = c->surface * s->surface + c->collisions * s->collision * 0.8f + c->bumps * s->bump * 0.3f;
    // Game and model combine like the game's own "combining_mode: max".
    l = fmaxf(l, c->game * s->game_low);
    h = fmaxf(h, c->game * s->game_high);
    *low = fx_map_motor(clampf(c->master * l, 0.0f, 1.0f), c->low_min);
    *high = fx_map_motor(clampf(c->master * h, 0.0f, 1.0f), c->high_min);
}
