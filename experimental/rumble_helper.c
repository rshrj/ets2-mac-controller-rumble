// Turns ETS2 telemetry from ets2_rumble_plugin into controller rumble via SDL3.
//
//   rumble_helper              live mode, normally started by the plugin
//   rumble_helper --calibrate  find the weakest level each motor can be felt at
//
// Live mode exits when the game shuts telemetry down, or after two minutes
// without packets. It logs to rumble_helper.log next to itself, re-reads
// rumble.conf (also next to itself) whenever it changes, records each drive
// to recordings/ when `record = 1`, and stamps PL/PR paddle presses into the
// recording (paddles are ignored when not recording).
#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "effects.h"
#include "protocol.h"
#include "recording.h"

#define NO_DATA_TIMEOUT_MS 150 // stop rumbling when telemetry stalls (menus, loading)
#define EXIT_TIMEOUT_MS 120000 // exit when the game is gone
#define SEND_MIN_MS 25         // don't flood the Bluetooth link
#define SEND_KEEPALIVE_MS 40   // refresh an ongoing rumble this often
#define RUMBLE_DURATION_MS 150 // each command lasts this long unless refreshed
#define MARKER_TICK_MS 160     // length of the confirmation tick after a marker

#define MARKER_MISSED_BUTTON SDL_GAMEPAD_BUTTON_LEFT_PADDLE2 // PL on the 8BitDo Pro 3
#define MARKER_FALSE_BUTTON SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2 // PR

static FILE *log_file;
static char base_dir[1024];

static void log_line(const char *fmt, ...) {
    if (!log_file)
        return;
    time_t now = time(NULL);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&now));
    fprintf(log_file, "%s ", stamp);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fputc('\n', log_file);
}

static uint64_t host_us(void) { return SDL_GetTicksNS() / 1000; }

extern char **environ;

// Steam hides physical controllers from games it launches with Steam Input
// (SDL_GAMECONTROLLER_IGNORE_DEVICES and friends). The helper inherits the
// game's environment, so drop every SDL_* variable before initializing SDL.
static void clear_sdl_env(void) {
    char names[32][128];
    int n = 0;
    for (char **e = environ; *e && n < 32; e++) {
        if (strncmp(*e, "SDL_", 4) != 0)
            continue;
        const char *eq = strchr(*e, '=');
        size_t len = eq ? (size_t)(eq - *e) : strlen(*e);
        if (len >= sizeof names[0])
            continue;
        memcpy(names[n], *e, len);
        names[n][len] = '\0';
        log_line("ignoring inherited %.200s", *e);
        n++;
    }
    for (int i = 0; i < n; i++)
        unsetenv(names[i]);
}

static void log_joysticks(void) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    log_line("no gamepad opened yet; SDL sees %d joystick(s)", count);
    for (int i = 0; i < count; i++)
        log_line("  %s (vid=0x%04x pid=0x%04x) gamepad=%s", SDL_GetJoystickNameForID(ids[i]),
                 SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i]),
                 SDL_IsGamepad(ids[i]) ? "yes" : "no");
    SDL_free(ids);
}

static SDL_Gamepad *open_pad(SDL_JoystickID id) {
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (pad) {
        bool can = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        log_line("gamepad: %s (vid=0x%04x pid=0x%04x) rumble=%s", SDL_GetGamepadName(pad), SDL_GetGamepadVendor(pad),
                 SDL_GetGamepadProduct(pad), can ? "yes" : "no");
    }
    return pad;
}

static bool init_sdl(void) {
    clear_sdl_env();
    // The helper never has a focused window; let paddle presses through anyway.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        log_line("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

// ---- recording ----

static FILE *rec_file;

// Latest static tables, written at the start of each recording because the
// plugin sends them before the first frame.
static struct rumble_pkt_substances last_substances;
static struct rumble_pkt_truck last_truck;

static void rec_write(uint32_t kind, const void *data, uint32_t size) {
    if (!rec_file)
        return;
    struct rec_header h = {kind, size, host_us()};
    fwrite(&h, sizeof h, 1, rec_file);
    fwrite(data, size, 1, rec_file);
}

static void rec_start(void) {
    char dir[1100], path[1200], stamp[32];
    snprintf(dir, sizeof dir, "%srecordings", base_dir);
    mkdir(dir, 0755);
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof path, "%s/drive-%s.rec", dir, stamp);
    rec_file = fopen(path, "wb");
    if (!rec_file) {
        log_line("could not create %s: %s", path, strerror(errno));
        return;
    }
    setvbuf(rec_file, NULL, _IOFBF, 1 << 16);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct rec_file_header fh = {{0}, RUMBLE_MAGIC, sizeof(struct rumble_pkt_frame),
                                 (int64_t)tv.tv_sec * 1000000 + tv.tv_usec};
    memcpy(fh.magic, REC_FILE_MAGIC, sizeof fh.magic);
    fwrite(&fh, sizeof fh, 1, rec_file);
    if (last_substances.h.magic)
        rec_write(REC_PACKET, &last_substances, sizeof last_substances);
    if (last_truck.h.magic)
        rec_write(REC_PACKET, &last_truck, sizeof last_truck);
    log_line("recording to %s", path);
}

static void rec_stop(void) {
    if (rec_file)
        fclose(rec_file);
    rec_file = NULL;
}

// ---- live mode ----

static int open_socket(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RUMBLE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // A previous helper may still be shutting down; give it a moment.
    int err = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (bind(sock, (struct sockaddr *)&addr, sizeof addr) == 0)
            return sock;
        err = errno;
        if (err != EADDRINUSE)
            break;
        SDL_Delay(100);
    }
    close(sock);
    errno = err;
    return -1;
}

// Two short high-motor ticks confirm a marker without feeling like an effect.
static bool marker_tick(Uint64 since_ms, float *low, float *high, float high_min) {
    if (since_ms >= MARKER_TICK_MS)
        return false;
    bool on = since_ms < 40 || (since_ms >= 100 && since_ms < 140);
    *low = 0.0f;
    *high = on ? fx_map_motor(0.5f, high_min) : 0.0f;
    return true;
}

static int run_live(void) {
    int sock = open_socket();
    if (sock < 0) {
        log_line("could not bind UDP port %d (another helper running?): %s", RUMBLE_PORT, strerror(errno));
        return 0;
    }
    if (!init_sdl())
        return 1;

    char cfg_path[1100];
    snprintf(cfg_path, sizeof cfg_path, "%srumble.conf", base_dir);
    struct fx_config cfg;
    fx_config_defaults(&cfg);
    struct fx_state fx;
    fx_init(&fx);

    SDL_Gamepad *pad = NULL;
    SDL_JoystickID pad_id = 0;
    Uint64 start_ms = SDL_GetTicks(), last_packet_ms = start_ms, last_frame_ms = 0;
    Uint64 last_send_ms = 0, last_cfg_ms = 0, last_flush_ms = 0, marker_ms = 0;
    float sent_low = 0.0f, sent_high = 0.0f;
    bool running = true, paused = false, reported_no_pad = false, have_marker = false;
    uint32_t markers[3] = {0};

    while (running) {
        Uint64 now = SDL_GetTicks();
        if (now - last_cfg_ms > 1000) {
            if (fx_config_load(&cfg, cfg_path))
                log_line("config: master=%.2f game=%.2f bumps=%.2f surface=%.2f collisions=%.2f engine=%.2f "
                         "low_min=%.2f high_min=%.2f record=%d",
                         cfg.master, cfg.game, cfg.bumps, cfg.surface, cfg.collisions, cfg.engine, cfg.low_min,
                         cfg.high_min, cfg.record);
            last_cfg_ms = now;
        }

        struct pollfd pfd = {sock, POLLIN, 0};
        poll(&pfd, 1, 5);

        union {
            struct rumble_pkt_header h;
            struct rumble_pkt_frame frame;
            struct rumble_pkt_substances substances;
            struct rumble_pkt_truck truck;
            struct rumble_pkt_event event;
            struct rumble_pkt_game_vibration vib;
            struct rumble_pkt_hook_stats stats;
        } buf;
        ssize_t n;
        while ((n = recv(sock, &buf, sizeof buf, MSG_DONTWAIT)) >= (ssize_t)sizeof buf.h) {
            if (buf.h.magic != RUMBLE_MAGIC)
                continue;
            last_packet_ms = SDL_GetTicks();
            if (buf.h.type == RUMBLE_PKT_FRAME && n == sizeof buf.frame) {
                if (!rec_file && cfg.record)
                    rec_start();
                fx_frame(&fx, &buf.frame);
                last_frame_ms = last_packet_ms;
                paused = false;
                if (fx.dbg.hit)
                    log_line("collision (crash wear +%.4f, jolt %.1f m/s^2)", fx.dbg.crash_wear, fx.dbg.jolt);
                if (fx.dbg.reset && fx.resets > 1)
                    log_line("state reset (load/teleport/timer restart)");
            } else if (buf.h.type == RUMBLE_PKT_SUBSTANCES && n == sizeof buf.substances) {
                static uint32_t logged;
                last_substances = buf.substances;
                fx_set_substances(&fx, &buf.substances);
                if (buf.substances.count != logged) {
                    logged = buf.substances.count;
                    log_line("substances (%u):", logged);
                    for (uint32_t i = 0; i < logged && i < RUMBLE_MAX_SUBSTANCES; i++)
                        log_line("  [%u] %.31s -> roughness %.2f", i, buf.substances.names[i],
                                 fx.roughness[i]);
                }
            } else if (buf.h.type == RUMBLE_PKT_TRUCK && n == sizeof buf.truck) {
                static char logged[80];
                char truck[80];
                last_truck = buf.truck;
                snprintf(truck, sizeof truck, "%.31s %.31s/%u", buf.truck.brand, buf.truck.name, buf.truck.wheel_count);
                if (strcmp(truck, logged)) {
                    snprintf(logged, sizeof logged, "%s", truck);
                    log_line("truck: %s wheels", truck);
                }
            } else if (buf.h.type == RUMBLE_PKT_EVENT && n == sizeof buf.event) {
                log_line("event: %.47s %.63s %lld", buf.event.id, buf.event.detail, (long long)buf.event.amount);
            } else if (buf.h.type == RUMBLE_PKT_GAME_VIBRATION && n == sizeof buf.vib) {
                static uint32_t count;
                if (count++ == 0)
                    log_line("first vibration command from the game: left=%u right=%u", buf.vib.left, buf.vib.right);
                fx_game_vibration(&fx, &buf.vib);
            } else if (buf.h.type == RUMBLE_PKT_HOOK_STATS && n == sizeof buf.stats) {
                static struct rumble_pkt_hook_stats last;
                static Uint64 last_log_ms;
                bool started = buf.stats.vibration_calls > 0 && last.vibration_calls == 0;
                if (buf.stats.installed != last.installed || started || SDL_GetTicks() - last_log_ms > 60000) {
                    log_line("hook: installed=%u type queries=%u (overridden %u), vibration calls=%u, hidden polls=%u",
                             buf.stats.installed, buf.stats.type_queries, buf.stats.type_overrides,
                             buf.stats.vibration_calls, buf.stats.hidden_polls);
                    last_log_ms = SDL_GetTicks();
                }
                last = buf.stats;
            } else if (buf.h.type == RUMBLE_PKT_PAUSED) {
                paused = true;
            } else if (buf.h.type == RUMBLE_PKT_QUIT) {
                log_line("game shut down telemetry, exiting");
                running = false;
            } else {
                continue;
            }
            rec_write(REC_PACKET, &buf, (uint32_t)n);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { // SDL turns SIGINT/SIGTERM into this
                log_line("asked to quit");
                running = false;
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
                pad = open_pad(e.gdevice.which);
                if (pad) {
                    pad_id = e.gdevice.which;
                    sent_low = sent_high = 0.0f;
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == pad_id) {
                log_line("gamepad disconnected");
                SDL_CloseGamepad(pad);
                pad = NULL;
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && pad && rec_file && e.gbutton.which == pad_id &&
                       (e.gbutton.button == MARKER_MISSED_BUTTON || e.gbutton.button == MARKER_FALSE_BUTTON)) {
                struct rec_marker m = {e.gbutton.button == MARKER_MISSED_BUTTON ? REC_MARKER_MISSED : REC_MARKER_FALSE};
                rec_write(REC_MARKER, &m, sizeof m);
                markers[m.kind]++;
                marker_ms = SDL_GetTicks();
                have_marker = true;
                log_line("marker: %s", m.kind == REC_MARKER_MISSED ? "missed (PL)" : "false alarm (PR)");
            }
        }

        now = SDL_GetTicks();
        if (!pad && !reported_no_pad && now - start_ms > 3000) {
            log_joysticks();
            reported_no_pad = true;
        }
        if (now - last_packet_ms > EXIT_TIMEOUT_MS) {
            log_line("no telemetry for %d s, exiting", EXIT_TIMEOUT_MS / 1000);
            running = false;
        }
        if (rec_file && now - last_flush_ms > 1000) {
            fflush(rec_file);
            last_flush_ms = now;
        }

        if (pad) {
            float low = 0.0f, high = 0.0f;
            if (have_marker && marker_tick(now - marker_ms, &low, &high, cfg.high_min)) {
                // confirmation tick overrides effects
            } else if (!paused && now - last_frame_ms <= NO_DATA_TIMEOUT_MS) {
                fx_output(&fx, &cfg, &low, &high);
            }
            bool active = low > 0.0f || high > 0.0f;
            bool was_active = sent_low > 0.0f || sent_high > 0.0f;
            bool rising = low - sent_low > 0.15f || high - sent_high > 0.15f;
            bool changed = fabsf(low - sent_low) > 0.01f || fabsf(high - sent_high) > 0.01f;
            Uint64 since = now - last_send_ms;
            if ((rising) || (changed && since >= SEND_MIN_MS) || (!active && was_active) ||
                (active && since >= SEND_KEEPALIVE_MS)) {
                SDL_RumbleGamepad(pad, (Uint16)(low * 65535.0f), (Uint16)(high * 65535.0f),
                                  active ? RUMBLE_DURATION_MS : 0);
                struct rec_rumble r = {low, high};
                rec_write(REC_RUMBLE, &r, sizeof r);
                sent_low = low;
                sent_high = high;
                last_send_ms = now;
            }
        }
    }

    if (pad) {
        SDL_RumbleGamepad(pad, 0, 0, 0);
        SDL_CloseGamepad(pad);
    }
    rec_stop();
    log_line("session: %u collisions, %u resets, markers: %u missed, %u false alarm", fx.hits, fx.resets,
             markers[REC_MARKER_MISSED], markers[REC_MARKER_FALSE]);
    SDL_Quit();
    close(sock);
    return 0;
}

// ---- calibration ----

// Rewrite `key = value` in rumble.conf, appending it if missing.
static void conf_set(const char *path, const char *key, float value) {
    char lines[200][256];
    int n = 0;
    bool found = false;
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < 200 && fgets(lines[n], sizeof lines[n], f)) {
            char k[64];
            if (sscanf(lines[n], " %63[a-z_] =", k) == 1 && !strcmp(k, key)) {
                snprintf(lines[n], sizeof lines[n], "%s = %.2f\n", key, value);
                found = true;
            }
            n++;
        }
        fclose(f);
    }
    if (!found && n < 200)
        snprintf(lines[n++], sizeof lines[0], "%s = %.2f\n", key, value);
    f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < n; i++)
        fputs(lines[i], f);
    fclose(f);
}

static bool wait_ms(Uint64 ms, SDL_JoystickID pad_id, Uint64 *pressed_at) {
    Uint64 end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                exit(0);
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.which == pad_id &&
                (e.gbutton.button == MARKER_MISSED_BUTTON || e.gbutton.button == MARKER_FALSE_BUTTON)) {
                *pressed_at = SDL_GetTicks();
                return true;
            }
        }
        SDL_Delay(2);
    }
    return false;
}

#define CAL_STEP 0.02f
#define CAL_MAX 0.70f
#define CAL_ON_MS 700
#define CAL_OFF_MS 400
#define CAL_REACTION_MS 350

// Ramp one motor up until the driver presses a paddle; returns the level that
// was playing a reaction time before the press, or -1 if never pressed.
static float calibrate_pass(SDL_Gamepad *pad, SDL_JoystickID id, bool low_motor) {
    float levels[64];
    Uint64 starts[64];
    int count = 0;
    Uint64 pressed_at = 0;
    for (float level = CAL_STEP; level <= CAL_MAX + 1e-4f && count < 64; level += CAL_STEP) {
        Uint16 v = (Uint16)(level * 65535.0f);
        printf("\r  level %2.0f%% ", level * 100.0f);
        fflush(stdout);
        levels[count] = level;
        starts[count] = SDL_GetTicks();
        count++;
        SDL_RumbleGamepad(pad, low_motor ? v : 0, low_motor ? 0 : v, CAL_ON_MS);
        bool hit = wait_ms(CAL_ON_MS, id, &pressed_at);
        SDL_RumbleGamepad(pad, 0, 0, 0);
        if (!hit)
            hit = wait_ms(CAL_OFF_MS, id, &pressed_at);
        if (hit) {
            Uint64 felt = pressed_at - CAL_REACTION_MS;
            int i = count - 1;
            while (i > 0 && starts[i] > felt)
                i--;
            printf("\n");
            wait_ms(800, id, &pressed_at); // ignore bounce / double presses
            return levels[i];
        }
    }
    printf("\n");
    return -1.0f;
}

static int run_calibrate(void) {
    if (!init_sdl()) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Gamepad *pad = NULL;
    SDL_JoystickID id = 0;
    Uint64 end = SDL_GetTicks() + 3000;
    while (!pad && SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
                id = e.gdevice.which;
                pad = SDL_OpenGamepad(id);
            }
        SDL_Delay(10);
    }
    if (!pad) {
        fprintf(stderr, "No gamepad found.\n");
        return 1;
    }

    printf("Calibrating %s.\n", SDL_GetGamepadName(pad));
    printf("Each motor ramps up from nothing in small steps, twice.\n");
    printf("Hold the controller normally and press PL or PR the moment you feel anything.\n\n");
    SDL_Delay(2500);

    char cfg_path[1100];
    snprintf(cfg_path, sizeof cfg_path, "%srumble.conf", base_dir);
    const char *names[2] = {"low (heavy) motor", "high (buzzy) motor"};
    const char *keys[2] = {"low_min", "high_min"};
    for (int m = 0; m < 2; m++) {
        float sum = 0.0f;
        int got = 0;
        for (int pass = 1; pass <= 2; pass++) {
            printf("%s, pass %d:\n", names[m], pass);
            float v = calibrate_pass(pad, id, m == 0);
            if (v < 0.0f) {
                printf("  no press up to %.0f%%\n", CAL_MAX * 100.0f);
            } else {
                printf("  felt at %.0f%%\n", v * 100.0f);
                sum += v;
                got++;
            }
            SDL_Delay(1200);
        }
        if (got) {
            // The motor is felt at the threshold; start mapped output just below it.
            float min = fmaxf(0.0f, sum / got - CAL_STEP);
            conf_set(cfg_path, keys[m], min);
            printf("=> %s = %.2f\n\n", keys[m], min);
        } else {
            printf("=> %s unchanged\n\n", keys[m]);
        }
    }
    SDL_CloseGamepad(pad);
    SDL_Quit();
    printf("Saved to %s\n", cfg_path);
    return 0;
}

int main(int argc, char **argv) {
    const char *base = SDL_GetBasePath();
    snprintf(base_dir, sizeof base_dir, "%s", base ? base : "./");

    if (argc > 1 && !strcmp(argv[1], "--calibrate"))
        return run_calibrate();

    char log_path[1100];
    snprintf(log_path, sizeof log_path, "%srumble_helper.log", base_dir);
    log_file = fopen(log_path, "w");
    if (log_file)
        setvbuf(log_file, NULL, _IOLBF, 0);
    log_line("rumble_helper starting (pid %d)", (int)getpid());
    return run_live();
}
