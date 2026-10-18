// ETS2 telemetry plugin: streams truck state to rumble_helper over localhost
// UDP once per physics frame, and launches the helper.
// Built for x86_64 because the macOS game binary is x86_64 (runs on Rosetta).
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"

#include "protocol.h"

#ifndef HELPER_PATH
#error "HELPER_PATH must be defined to the absolute path of rumble_helper"
#endif

extern char **environ;

static struct {
    scs_telemetry_register_for_channel_t register_for_channel;
    scs_telemetry_unregister_from_channel_t unregister_from_channel;
    scs_log_t game_log;
    int sock;
    sockaddr_in addr;
    bool paused;
    unsigned registered_wheels;
    rumble_pkt_frame frame;
    rumble_pkt_substances substances;
    rumble_pkt_truck truck;
    pid_t helper;
} g;

static void log_msg(scs_log_type_t type, const char *fmt, ...) {
    if (!g.game_log)
        return;
    char buf[512] = "[ets2-rumble] ";
    size_t prefix = strlen(buf);
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + prefix, sizeof buf - prefix, fmt, args);
    va_end(args);
    g.game_log(type, buf);
}

static void send_pkt(const void *data, size_t size) {
    if (g.sock >= 0)
        sendto(g.sock, data, size, 0, (const sockaddr *)&g.addr, sizeof g.addr);
}

static void send_simple(uint32_t type) {
    rumble_pkt_header h = {RUMBLE_MAGIC, type};
    send_pkt(&h, sizeof h);
}

static void send_hook_stats(void);
static void log_hook_stats(void);

static void start_helper(void) {
    if (g.helper > 0 && waitpid(g.helper, NULL, WNOHANG) == 0)
        return; // still running from an earlier init
    g.helper = 0;

    // Steam injects its overlay through DYLD_INSERT_LIBRARIES; keep that and
    // the other DYLD_* variables away from the native arm64 helper. (The
    // helper drops Steam's SDL_* controller filters itself.)
    char *envp[512];
    size_t n = 0;
    for (char **e = environ; *e && n < 511; e++)
        if (strncmp(*e, "DYLD_", 5) != 0)
            envp[n++] = *e;
    envp[n] = NULL;

    char *argv[] = {(char *)HELPER_PATH, NULL};
    pid_t pid;
    int err = posix_spawn(&pid, HELPER_PATH, NULL, NULL, argv, envp);
    if (err == 0) {
        g.helper = pid;
        log_msg(SCS_LOG_TYPE_message, "started helper %s (pid %d)", HELPER_PATH, (int)pid);
    } else {
        log_msg(SCS_LOG_TYPE_error, "failed to start helper %s: %s", HELPER_PATH, strerror(err));
    }
}

// ---- channel callbacks: context points at the destination field ----

static SCSAPI_VOID store_float(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                               const scs_context_t context) {
    *static_cast<float *>(context) = value ? value->value_float.value : 0.0f;
}

static SCSAPI_VOID store_bool(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                              const scs_context_t context) {
    *static_cast<uint8_t *>(context) = value ? value->value_bool.value : 0;
}

static SCSAPI_VOID store_u32(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                             const scs_context_t context) {
    *static_cast<uint32_t *>(context) = value ? value->value_u32.value : 0;
}

static SCSAPI_VOID store_s32(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                             const scs_context_t context) {
    *static_cast<int32_t *>(context) = value ? value->value_s32.value : 0;
}

static SCSAPI_VOID store_substance(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                                   const scs_context_t context) {
    *static_cast<uint32_t *>(context) = value ? value->value_u32.value : RUMBLE_NO_SUBSTANCE;
}

static SCSAPI_VOID store_fvector(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                                 const scs_context_t context) {
    float *v = static_cast<float *>(context);
    v[0] = value ? value->value_fvector.x : 0.0f;
    v[1] = value ? value->value_fvector.y : 0.0f;
    v[2] = value ? value->value_fvector.z : 0.0f;
}

static SCSAPI_VOID store_cabin_offset(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                                      const scs_context_t) {
    const scs_value_fplacement_t *p = value ? &value->value_fplacement : NULL;
    g.frame.cabin_offset[0] = p ? p->position.x : 0.0f;
    g.frame.cabin_offset[1] = p ? p->position.y : 0.0f;
    g.frame.cabin_offset[2] = p ? p->position.z : 0.0f;
    g.frame.cabin_rot[0] = p ? p->orientation.heading : 0.0f;
    g.frame.cabin_rot[1] = p ? p->orientation.pitch : 0.0f;
    g.frame.cabin_rot[2] = p ? p->orientation.roll : 0.0f;
}

static SCSAPI_VOID store_world_placement(const scs_string_t, const scs_u32_t, const scs_value_t *const value,
                                         const scs_context_t) {
    const scs_value_dplacement_t *p = value ? &value->value_dplacement : NULL;
    g.frame.position[0] = p ? p->position.x : 0.0;
    g.frame.position[1] = p ? p->position.y : 0.0;
    g.frame.position[2] = p ? p->position.z : 0.0;
    g.frame.orientation[0] = p ? p->orientation.heading : 0.0f;
    g.frame.orientation[1] = p ? p->orientation.pitch : 0.0f;
    g.frame.orientation[2] = p ? p->orientation.roll : 0.0f;
}

// ---- channel tables ----

#define FRAME_FIELD(f) offsetof(rumble_pkt_frame, f)

struct channel_def {
    const char *name;
    scs_value_type_t type;
    scs_telemetry_channel_callback_t callback;
    size_t offset; // into g.frame
};

static const channel_def truck_channels[] = {
    {SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement, store_world_placement, 0},
    {SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_velocity, SCS_VALUE_TYPE_fvector, store_fvector, FRAME_FIELD(lin_vel)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_local_angular_velocity, SCS_VALUE_TYPE_fvector, store_fvector, FRAME_FIELD(ang_vel)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_acceleration, SCS_VALUE_TYPE_fvector, store_fvector, FRAME_FIELD(lin_acc)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_local_angular_acceleration, SCS_VALUE_TYPE_fvector, store_fvector, FRAME_FIELD(ang_acc)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_cabin_offset, SCS_VALUE_TYPE_fplacement, store_cabin_offset, 0},
    {SCS_TELEMETRY_TRUCK_CHANNEL_cabin_angular_velocity, SCS_VALUE_TYPE_fvector, store_fvector, FRAME_FIELD(cabin_ang_vel)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_cabin_angular_acceleration, SCS_VALUE_TYPE_fvector, store_fvector, FRAME_FIELD(cabin_ang_acc)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(speed)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(rpm)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_effective_throttle, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(throttle)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_effective_brake, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(brake)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_effective_clutch, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(clutch)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_effective_steering, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(steering)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_cruise_control, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(cruise_control)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wear_engine, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wear_engine)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wear_transmission, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wear_transmission)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wear_cabin, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wear_cabin)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wear_chassis, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wear_chassis)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wear_wheels, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wear_wheels)},
    {"trailer.0.wear.chassis", SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(trailer_wear_chassis)},
    {"trailer.0.wear.body", SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(trailer_wear_body)},
    {"trailer.0.cargo.damage", SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(trailer_cargo_damage)},
    {"trailer.0.connected", SCS_VALUE_TYPE_bool, store_bool, FRAME_FIELD(trailer_connected)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, SCS_VALUE_TYPE_s32, store_s32, FRAME_FIELD(gear)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_retarder_level, SCS_VALUE_TYPE_u32, store_u32, FRAME_FIELD(retarder)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_engine_enabled, SCS_VALUE_TYPE_bool, store_bool, FRAME_FIELD(engine_enabled)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake, SCS_VALUE_TYPE_bool, store_bool, FRAME_FIELD(parking_brake)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_motor_brake, SCS_VALUE_TYPE_bool, store_bool, FRAME_FIELD(motor_brake)},
};

static const channel_def wheel_channels[] = {
    {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_susp_deflection, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(deflection)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_on_ground, SCS_VALUE_TYPE_bool, store_bool, FRAME_FIELD(on_ground)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_substance, SCS_VALUE_TYPE_u32, store_substance, FRAME_FIELD(substance)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_velocity, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wheel_ang_vel)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_steering, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wheel_steering)},
    {SCS_TELEMETRY_TRUCK_CHANNEL_wheel_lift, SCS_VALUE_TYPE_float, store_float, FRAME_FIELD(wheel_lift)},
};

static size_t value_size(scs_value_type_t type) {
    return type == SCS_VALUE_TYPE_bool ? sizeof(uint8_t) : sizeof(float); // u32 and float are both 4 bytes
}

static void *frame_ptr(size_t offset) { return reinterpret_cast<char *>(&g.frame) + offset; }

static void set_wheel_count(unsigned count) {
    if (count > RUMBLE_MAX_WHEELS)
        count = RUMBLE_MAX_WHEELS;
    while (g.registered_wheels > count) {
        unsigned i = --g.registered_wheels;
        for (const channel_def &c : wheel_channels) {
            g.unregister_from_channel(c.name, i, c.type);
            memset(frame_ptr(c.offset + i * value_size(c.type)), 0, value_size(c.type));
        }
        g.frame.substance[i] = RUMBLE_NO_SUBSTANCE;
    }
    while (g.registered_wheels < count) {
        unsigned i = g.registered_wheels++;
        for (const channel_def &c : wheel_channels)
            g.register_for_channel(c.name, i, c.type, SCS_TELEMETRY_CHANNEL_FLAG_none, c.callback,
                                   frame_ptr(c.offset + i * value_size(c.type)));
    }
    g.frame.wheel_count = (uint8_t)count;
}

// ---- events ----

static SCSAPI_VOID on_frame_start(const scs_event_t, const void *const event_info, const scs_context_t) {
    const auto *info = static_cast<const scs_telemetry_frame_start_t *>(event_info);
    g.frame.sim_time_us = info->simulation_time;
    g.frame.render_time_us = info->render_time;
    g.frame.paused_sim_time_us = info->paused_simulation_time;
    g.frame.frame_flags = (info->flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart) ? RUMBLE_FRAME_TIMER_RESTART : 0;
}

static SCSAPI_VOID on_frame_end(const scs_event_t, const void *const, const scs_context_t) {
    g.frame.frame_index++;
    if (!g.paused)
        send_pkt(&g.frame, sizeof g.frame);
    // Resend the static tables now and then in case the helper restarted.
    if (g.frame.frame_index % 120 == 0) {
        if (g.substances.count)
            send_pkt(&g.substances, sizeof g.substances);
        send_pkt(&g.truck, sizeof g.truck);
        send_hook_stats();
    }
    // Hook counters in game.log.txt: every ~10 s for the first minute, then every ~5 min.
    unsigned fi = g.frame.frame_index;
    if ((fi <= 3600 && fi % 600 == 0) || fi % 18000 == 0)
        log_hook_stats();
}

static SCSAPI_VOID on_pause(const scs_event_t event, const void *const, const scs_context_t) {
    g.paused = (event == SCS_TELEMETRY_EVENT_paused);
    if (g.paused)
        send_simple(RUMBLE_PKT_PAUSED);
}

static void copy_string(char *dst, size_t size, const scs_value_t &v) {
    if (v.type == SCS_VALUE_TYPE_string && v.value_string.value) {
        strncpy(dst, v.value_string.value, size - 1);
        dst[size - 1] = '\0';
    }
}

static void handle_truck_config(const scs_telemetry_configuration_t *info) {
    memset(&g.truck, 0, sizeof g.truck);
    g.truck.h = {RUMBLE_MAGIC, RUMBLE_PKT_TRUCK};
    for (const scs_named_value_t *a = info->attributes; a->name; ++a) {
        const scs_value_t &v = a->value;
        unsigned i = a->index;
        bool wheel = i < RUMBLE_MAX_WHEELS;
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count) && v.type == SCS_VALUE_TYPE_u32)
            g.truck.wheel_count = v.value_u32.value;
        else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand))
            copy_string(g.truck.brand, sizeof g.truck.brand, v);
        else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name))
            copy_string(g.truck.name, sizeof g.truck.name, v);
        else if (wheel && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_radius) && v.type == SCS_VALUE_TYPE_float)
            g.truck.wheel_radius[i] = v.value_float.value;
        else if (wheel && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_position) && v.type == SCS_VALUE_TYPE_fvector) {
            g.truck.wheel_position[i][0] = v.value_fvector.x;
            g.truck.wheel_position[i][1] = v.value_fvector.y;
            g.truck.wheel_position[i][2] = v.value_fvector.z;
        } else if (wheel && v.type == SCS_VALUE_TYPE_bool) {
            if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_powered))
                g.truck.wheel_powered[i] = v.value_bool.value;
            else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_steerable))
                g.truck.wheel_steerable[i] = v.value_bool.value;
            else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_liftable))
                g.truck.wheel_liftable[i] = v.value_bool.value;
            else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_simulated))
                g.truck.wheel_simulated[i] = v.value_bool.value;
        }
    }
    set_wheel_count(g.truck.wheel_count);
    send_pkt(&g.truck, sizeof g.truck);
    log_msg(SCS_LOG_TYPE_message, "truck '%s %s' has %u wheels", g.truck.brand, g.truck.name, g.truck.wheel_count);
}

static void handle_substances_config(const scs_telemetry_configuration_t *info) {
    memset(g.substances.names, 0, sizeof g.substances.names);
    g.substances.count = 0;
    for (const scs_named_value_t *a = info->attributes; a->name; ++a) {
        if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id) != 0 || a->index >= RUMBLE_MAX_SUBSTANCES)
            continue;
        copy_string(g.substances.names[a->index], RUMBLE_SUBSTANCE_NAME_LEN, a->value);
        if (a->index + 1 > g.substances.count)
            g.substances.count = a->index + 1;
    }
    send_pkt(&g.substances, sizeof g.substances);
    log_msg(SCS_LOG_TYPE_message, "got %u substances", g.substances.count);
}

static SCSAPI_VOID on_configuration(const scs_event_t, const void *const event_info, const scs_context_t) {
    const auto *info = static_cast<const scs_telemetry_configuration_t *>(event_info);
    if (!strcmp(info->id, SCS_TELEMETRY_CONFIG_truck))
        handle_truck_config(info);
    else if (!strcmp(info->id, SCS_TELEMETRY_CONFIG_substances))
        handle_substances_config(info);
}

static SCSAPI_VOID on_gameplay(const scs_event_t, const void *const event_info, const scs_context_t) {
    const auto *info = static_cast<const scs_telemetry_gameplay_event_t *>(event_info);
    rumble_pkt_event e;
    memset(&e, 0, sizeof e);
    e.h = {RUMBLE_MAGIC, RUMBLE_PKT_EVENT};
    strncpy(e.id, info->id, sizeof e.id - 1);
    for (const scs_named_value_t *a = info->attributes; a->name; ++a) {
        if (!strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_offence))
            copy_string(e.detail, sizeof e.detail, a->value);
        else if (a->value.type == SCS_VALUE_TYPE_s64 &&
                 (!strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_amount) ||
                  !strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_pay_amount)))
            e.amount = a->value.value_s64.value;
    }
    send_pkt(&e, sizeof e);
}

// ---- Steam Input hook ----
//
// ETS2 only sends vibration to controllers whose Steam Input type it has a
// motor map for (def: steam_controller_vibration, types 1-5, 10, 13). The
// 8BitDo Pro 3 in D mode reports k_ESteamInputType_Unknown, so the game never
// vibrates it. We patch the vtable of the game's ISteamInput006 object so that
// Unknown pads report as generic gamepads (type 4, which uses the linear
// ".vibration.xi" map), and forward every vibration command to the helper.
//
// Slots are from the flat API thunks in the game's own libsteam_api.dylib.
#define STEAM_INPUT_VERSION "SteamInput006"
#define VT_GET_CONNECTED_CONTROLLERS 6
#define VT_TRIGGER_VIBRATION 30
#define VT_TRIGGER_VIBRATION_EXTENDED 31
#define VT_GET_INPUT_TYPE_FOR_HANDLE 37
#define VT_SLOTS 56   // ISteamInput006 has 48+ methods; copy a little extra
#define VT_PREFIX 2   // offset-to-top and RTTI pointer before the first slot
#define STEAM_INPUT_TYPE_UNKNOWN 0
#define STEAM_INPUT_TYPE_GENERIC_GAMEPAD 4
// Default count of GetConnectedControllers calls answered with 0 after install.
// The game polls about once a second and sets the pad up again on the first
// poll that shows it, so each poll is ~1 s of "missing" controller.
#define REDETECT_POLLS 2

typedef uint64_t InputHandle_t;
typedef void (*trigger_vibration_t)(void *, InputHandle_t, unsigned short, unsigned short);
typedef void (*trigger_vibration_ext_t)(void *, InputHandle_t, unsigned short, unsigned short, unsigned short,
                                        unsigned short);
typedef int (*get_input_type_t)(void *, InputHandle_t);
typedef int (*get_connected_t)(void *, InputHandle_t *);

static struct {
    void **object;
    void **orig_vtable;
    void *vtable[VT_PREFIX + VT_SLOTS];
    trigger_vibration_t trigger;
    trigger_vibration_ext_t trigger_ext;
    get_input_type_t get_type;
    get_connected_t get_connected;
    int hide_polls;
    rumble_pkt_hook_stats stats;
} hook;

// Options read from plugin.conf next to the helper at each init.
static struct {
    bool steam_input_hook = true;
    bool force_redetect = false;
    int redetect_polls = REDETECT_POLLS;
} opts;

static void load_plugin_conf(void) {
    opts = {};
    char path[1100];
    snprintf(path, sizeof path, "%s", HELPER_PATH);
    char *slash = strrchr(path, '/');
    if (!slash)
        return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "plugin.conf");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256], key[64];
    int value;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %d", key, &value) != 2)
            continue;
        if (!strcmp(key, "steam_input_hook"))
            opts.steam_input_hook = value != 0;
        else if (!strcmp(key, "force_redetect"))
            opts.force_redetect = value != 0;
        else if (!strcmp(key, "redetect_polls"))
            opts.redetect_polls = value > 0 ? value : 1;
    }
    fclose(f);
}

static void send_game_vibration(unsigned short l, unsigned short r, unsigned short lt, unsigned short rt) {
    hook.stats.vibration_calls++;
    rumble_pkt_game_vibration p = {{RUMBLE_MAGIC, RUMBLE_PKT_GAME_VIBRATION}, l, r, lt, rt};
    send_pkt(&p, sizeof p);
}

// The hooks run inside the game's own Steam Input calls (main thread), so they
// must not call back into the game; they only count, forward and delegate.
static void hook_trigger_vibration(void *self, InputHandle_t handle, unsigned short l, unsigned short r) {
    send_game_vibration(l, r, 0, 0);
    hook.trigger(self, handle, l, r);
}

static void hook_trigger_vibration_ext(void *self, InputHandle_t handle, unsigned short l, unsigned short r,
                                       unsigned short lt, unsigned short rt) {
    send_game_vibration(l, r, lt, rt);
    hook.trigger_ext(self, handle, l, r, lt, rt);
}

static int hook_get_input_type(void *self, InputHandle_t handle) {
    int type = hook.get_type(self, handle);
    hook.stats.type_queries++;
    if (type == STEAM_INPUT_TYPE_UNKNOWN) {
        hook.stats.type_overrides++;
        type = STEAM_INPUT_TYPE_GENERIC_GAMEPAD;
    }
    return type;
}

// Pretending the controllers vanished for a moment makes the game set the
// device up again, this time asking our patched GetInputTypeForHandle.
static int hook_get_connected(void *self, InputHandle_t *out) {
    int count = hook.get_connected(self, out);
    if (hook.hide_polls > 0) {
        hook.hide_polls--;
        hook.stats.hidden_polls++;
        return 0;
    }
    return count;
}

static bool in_steamclient(void *fn) {
    Dl_info info;
    return fn && dladdr(fn, &info) && info.dli_fname && strstr(info.dli_fname, "steamclient");
}

static void install_steam_input_hook(void) {
    auto find = (void *(*)(int32_t, const char *))dlsym(RTLD_DEFAULT, "SteamInternal_FindOrCreateUserInterface");
    auto get_user = (int32_t(*)(void))dlsym(RTLD_DEFAULT, "SteamAPI_GetHSteamUser");
    if (!find || !get_user) {
        log_msg(SCS_LOG_TYPE_warning, "steam input hook: Steam API not found");
        return;
    }
    void **object = static_cast<void **>(find(get_user(), STEAM_INPUT_VERSION));
    if (!object || !*object) {
        log_msg(SCS_LOG_TYPE_warning, "steam input hook: no %s interface", STEAM_INPUT_VERSION);
        return;
    }
    void **vtable = static_cast<void **>(*object);
    if (vtable == hook.vtable + VT_PREFIX)
        return; // already installed
    // Only patch if the slots we replace really are Steam client code.
    if (!in_steamclient(vtable[VT_TRIGGER_VIBRATION]) || !in_steamclient(vtable[VT_TRIGGER_VIBRATION_EXTENDED]) ||
        !in_steamclient(vtable[VT_GET_INPUT_TYPE_FOR_HANDLE]) || !in_steamclient(vtable[VT_GET_CONNECTED_CONTROLLERS])) {
        log_msg(SCS_LOG_TYPE_warning, "steam input hook: unexpected vtable layout, not installing");
        return;
    }

    memcpy(hook.vtable, vtable - VT_PREFIX, sizeof hook.vtable);
    void **slots = hook.vtable + VT_PREFIX;
    hook.trigger = reinterpret_cast<trigger_vibration_t>(slots[VT_TRIGGER_VIBRATION]);
    hook.trigger_ext = reinterpret_cast<trigger_vibration_ext_t>(slots[VT_TRIGGER_VIBRATION_EXTENDED]);
    hook.get_type = reinterpret_cast<get_input_type_t>(slots[VT_GET_INPUT_TYPE_FOR_HANDLE]);
    hook.get_connected = reinterpret_cast<get_connected_t>(slots[VT_GET_CONNECTED_CONTROLLERS]);
    slots[VT_TRIGGER_VIBRATION] = reinterpret_cast<void *>(hook_trigger_vibration);
    slots[VT_TRIGGER_VIBRATION_EXTENDED] = reinterpret_cast<void *>(hook_trigger_vibration_ext);
    slots[VT_GET_INPUT_TYPE_FOR_HANDLE] = reinterpret_cast<void *>(hook_get_input_type);
    slots[VT_GET_CONNECTED_CONTROLLERS] = reinterpret_cast<void *>(hook_get_connected);

    hook.hide_polls = opts.force_redetect ? opts.redetect_polls : 0;
    hook.orig_vtable = vtable;
    hook.object = object;
    *object = slots;
    hook.stats.installed = 1;
    log_msg(SCS_LOG_TYPE_message, "steam input hook installed (force_redetect=%d, redetect_polls=%d)",
            opts.force_redetect, hook.hide_polls);
}

static void remove_steam_input_hook(void) {
    if (hook.object && *hook.object == hook.vtable + VT_PREFIX)
        *hook.object = hook.orig_vtable;
    hook.object = NULL;
    hook.stats.installed = 0;
}

static void send_hook_stats(void) {
    hook.stats.h = {RUMBLE_MAGIC, RUMBLE_PKT_HOOK_STATS};
    send_pkt(&hook.stats, sizeof hook.stats);
}

static void log_hook_stats(void) {
    log_msg(SCS_LOG_TYPE_message, "steam input hook: installed=%u type queries=%u (overridden %u), vibration calls=%u, hidden polls=%u",
            hook.stats.installed, hook.stats.type_queries, hook.stats.type_overrides, hook.stats.vibration_calls,
            hook.stats.hidden_polls);
}

// ---- init / shutdown ----

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params) {
    if (version != SCS_TELEMETRY_VERSION_1_01)
        return SCS_RESULT_unsupported;
    const auto *p = static_cast<const scs_telemetry_init_params_v101_t *>(params);

    memset(&g.frame, 0, sizeof g.frame);
    memset(&g.substances, 0, sizeof g.substances);
    memset(&g.truck, 0, sizeof g.truck);
    g.frame.h = {RUMBLE_MAGIC, RUMBLE_PKT_FRAME};
    g.substances.h = {RUMBLE_MAGIC, RUMBLE_PKT_SUBSTANCES};
    g.truck.h = {RUMBLE_MAGIC, RUMBLE_PKT_TRUCK};
    for (unsigned i = 0; i < RUMBLE_MAX_WHEELS; i++)
        g.frame.substance[i] = RUMBLE_NO_SUBSTANCE;
    g.register_for_channel = p->register_for_channel;
    g.unregister_from_channel = p->unregister_from_channel;
    g.game_log = p->common.log;
    g.paused = true;
    g.registered_wheels = 0;

    g.sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&g.addr, 0, sizeof g.addr);
    g.addr.sin_family = AF_INET;
    g.addr.sin_port = htons(RUMBLE_PORT);
    g.addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g.sock < 0) {
        log_msg(SCS_LOG_TYPE_error, "socket() failed: %s", strerror(errno));
        return SCS_RESULT_generic_error;
    }

    bool ok = p->register_for_event(SCS_TELEMETRY_EVENT_frame_start, on_frame_start, NULL) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, on_frame_end, NULL) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_paused, on_pause, NULL) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_started, on_pause, NULL) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_configuration, on_configuration, NULL) == SCS_RESULT_ok;
    if (!ok) {
        log_msg(SCS_LOG_TYPE_error, "unable to register event callbacks");
        close(g.sock);
        g.sock = -1;
        return SCS_RESULT_generic_error;
    }
    p->register_for_event(SCS_TELEMETRY_EVENT_gameplay, on_gameplay, NULL);

    unsigned missing = 0;
    for (const channel_def &c : truck_channels)
        if (p->register_for_channel(c.name, SCS_U32_NIL, c.type, SCS_TELEMETRY_CHANNEL_FLAG_none, c.callback,
                                    frame_ptr(c.offset)) != SCS_RESULT_ok) {
            log_msg(SCS_LOG_TYPE_warning, "channel %s not available", c.name);
            missing++;
        }

    log_msg(SCS_LOG_TYPE_message, "initialized for '%s' %u.%u (%u channels missing)", p->common.game_id,
            SCS_GET_MAJOR_VERSION(p->common.game_version), SCS_GET_MINOR_VERSION(p->common.game_version), missing);
    start_helper();
    load_plugin_conf();
    if (opts.steam_input_hook)
        install_steam_input_hook();
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    // Our hook functions live in this library, which the game may unload next.
    remove_steam_input_hook();
    send_simple(RUMBLE_PKT_QUIT);
    if (g.sock >= 0)
        close(g.sock);
    g.sock = -1;
    // The game drops all registrations itself on shutdown.
    g.registered_wheels = 0;
    g.register_for_channel = NULL;
    g.unregister_from_channel = NULL;
    g.game_log = NULL;
}
