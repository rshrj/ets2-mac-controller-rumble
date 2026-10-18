// ETS2 plugin that makes the game vibrate an 8BitDo Pro 3 in D mode.
//
// ETS2 only sends vibration to controllers whose Steam Input type it has a
// motor map for (def: steam_controller_vibration, types 1-5, 10, 13). The pad
// reports k_ESteamInputType_Unknown, so the game never vibrates it. We patch
// the vtable of the game's ISteamInput006 object so that Unknown pads report as
// generic gamepads (type 4); Steam then delivers the game's vibration itself.
//
// It uses the telemetry SDK only to get loaded. Built for x86_64 because the
// macOS game binary is x86_64 (runs on Rosetta).
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scssdk_telemetry.h"

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
// The game sets the pad up ~3 s after launch but plugins load at ~11 s, so we
// answer this many GetConnectedControllers polls (about one per second) with 0
// to make it set the pad up again, now asking our patched type query.
#define REDETECT_POLLS 2

typedef uint64_t InputHandle_t;
typedef void (*trigger_vibration_t)(void *, InputHandle_t, unsigned short, unsigned short);
typedef void (*trigger_vibration_ext_t)(void *, InputHandle_t, unsigned short, unsigned short, unsigned short,
                                        unsigned short);
typedef int (*get_input_type_t)(void *, InputHandle_t);
typedef int (*get_connected_t)(void *, InputHandle_t *);

static scs_log_t game_log;

static struct {
    void **object;
    void **orig_vtable;
    void *vtable[VT_PREFIX + VT_SLOTS];
    trigger_vibration_t trigger;
    trigger_vibration_ext_t trigger_ext;
    get_input_type_t get_type;
    get_connected_t get_connected;
    int hide_polls;
    unsigned type_queries, type_overrides, vibration_calls, hidden_polls;
} hook;

static void log_msg(scs_log_type_t type, const char *fmt, ...) {
    if (!game_log)
        return;
    char buf[512] = "[steam-input-fix] ";
    size_t prefix = strlen(buf);
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + prefix, sizeof buf - prefix, fmt, args);
    va_end(args);
    game_log(type, buf);
}

// The hooks run inside the game's own Steam Input calls (main thread), so they
// must not call back into the game; they only count and delegate.
static void hook_trigger_vibration(void *self, InputHandle_t handle, unsigned short l, unsigned short r) {
    hook.vibration_calls++;
    hook.trigger(self, handle, l, r);
}

static void hook_trigger_vibration_ext(void *self, InputHandle_t handle, unsigned short l, unsigned short r,
                                       unsigned short lt, unsigned short rt) {
    hook.vibration_calls++;
    hook.trigger_ext(self, handle, l, r, lt, rt);
}

static int hook_get_input_type(void *self, InputHandle_t handle) {
    int type = hook.get_type(self, handle);
    hook.type_queries++;
    if (type == STEAM_INPUT_TYPE_UNKNOWN) {
        hook.type_overrides++;
        type = STEAM_INPUT_TYPE_GENERIC_GAMEPAD;
    }
    return type;
}

static int hook_get_connected(void *self, InputHandle_t *out) {
    int count = hook.get_connected(self, out);
    if (hook.hide_polls > 0) {
        hook.hide_polls--;
        hook.hidden_polls++;
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
        log_msg(SCS_LOG_TYPE_warning, "Steam API not found");
        return;
    }
    void **object = static_cast<void **>(find(get_user(), STEAM_INPUT_VERSION));
    if (!object || !*object) {
        log_msg(SCS_LOG_TYPE_warning, "no %s interface", STEAM_INPUT_VERSION);
        return;
    }
    void **vtable = static_cast<void **>(*object);
    if (vtable == hook.vtable + VT_PREFIX)
        return; // already installed
    // Only patch if the slots we replace really are Steam client code.
    if (!in_steamclient(vtable[VT_TRIGGER_VIBRATION]) || !in_steamclient(vtable[VT_TRIGGER_VIBRATION_EXTENDED]) ||
        !in_steamclient(vtable[VT_GET_INPUT_TYPE_FOR_HANDLE]) || !in_steamclient(vtable[VT_GET_CONNECTED_CONTROLLERS])) {
        log_msg(SCS_LOG_TYPE_warning, "unexpected vtable layout, not installing");
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

    hook.hide_polls = REDETECT_POLLS;
    hook.orig_vtable = vtable;
    hook.object = object;
    *object = slots;
    log_msg(SCS_LOG_TYPE_message, "installed (redetect_polls=%d)", REDETECT_POLLS);
}

static void remove_steam_input_hook(void) {
    if (hook.object && *hook.object == hook.vtable + VT_PREFIX)
        *hook.object = hook.orig_vtable;
    hook.object = NULL;
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params) {
    if (version != SCS_TELEMETRY_VERSION_1_01)
        return SCS_RESULT_unsupported;
    const auto *p = static_cast<const scs_telemetry_init_params_v101_t *>(params);
    game_log = p->common.log;
    install_steam_input_hook();
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    log_msg(SCS_LOG_TYPE_message, "type queries=%u (overridden %u), vibration calls=%u, hidden polls=%u",
            hook.type_queries, hook.type_overrides, hook.vibration_calls, hook.hidden_polls);
    // Our hook functions live in this library, which the game may unload next.
    remove_steam_input_hook();
    game_log = NULL;
}
