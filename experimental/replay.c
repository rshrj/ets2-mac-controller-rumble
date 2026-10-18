// Runs a drive recording through the current rumble model and writes CSVs:
//   <recording>.frames.csv  one row per physics frame: telemetry, model internals,
//                           model output, what was actually sent live, markers
//   <recording>.events.csv  markers, gameplay events, truck/substance tables, pauses
//
// Usage: replay <drive.rec> [rumble.conf]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects.h"
#include "protocol.h"
#include "recording.h"

#define CSV_WHEELS 12 // per-wheel columns; ETS2 trucks have at most 10 wheels

static char substance_names[RUMBLE_MAX_SUBSTANCES][RUMBLE_SUBSTANCE_NAME_LEN];

static const char *substance_name(uint32_t i) {
    return i < RUMBLE_MAX_SUBSTANCES && substance_names[i][0] ? substance_names[i] : "-";
}

static void write_frame_header(FILE *f) {
    fputs("host_s,sim_s,frame,speed_kmh,rpm,gear,throttle,brake,clutch,steering,cruise_kmh,retarder,"
          "parking_brake,motor_brake,engine_on,trailer,"
          "acc_x,acc_y,acc_z,angvel_x,angvel_y,angvel_z,angacc_x,angacc_y,angacc_z,"
          "cab_off_x,cab_off_y,cab_off_z,cab_pitch,cab_roll,cab_angacc_x,cab_angacc_y,cab_angacc_z,"
          "pos_x,pos_y,pos_z,wear_cabin,wear_chassis,trailer_wear,cargo_damage,"
          "wheels,grounded,surfaces",
          f);
    for (int i = 0; i < CSV_WHEELS; i++)
        fprintf(f, ",defl%d", i);
    for (int i = 0; i < CSV_WHEELS; i++)
        fprintf(f, ",wvel%d", i);
    for (int i = 0; i < CSV_WHEELS; i++)
        fprintf(f, ",ground%d", i);
    for (int i = 0; i < CSV_WHEELS; i++)
        fprintf(f, ",subst%d", i);
    fputs(",bump_metric,jolt,crash_wear_delta,reset,hit,env_bump,env_collision,env_surface,"
          "game_low,game_high,model_low,model_high,sent_low,sent_high,marker\n",
          f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <drive.rec> [rumble.conf]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    struct rec_file_header fh;
    if (fread(&fh, sizeof fh, 1, in) != 1 || memcmp(fh.magic, REC_FILE_MAGIC, sizeof fh.magic) != 0) {
        fprintf(stderr, "%s: not a recording\n", argv[1]);
        return 1;
    }
    if (fh.protocol_magic != RUMBLE_MAGIC || fh.frame_size != sizeof(struct rumble_pkt_frame)) {
        fprintf(stderr, "%s: recorded with a different protocol version\n", argv[1]);
        return 1;
    }

    struct fx_config cfg;
    fx_config_defaults(&cfg);
    if (argc > 2 && !fx_config_load(&cfg, argv[2]))
        fprintf(stderr, "warning: could not read %s, using defaults\n", argv[2]);

    char path[4096];
    snprintf(path, sizeof path, "%s.frames.csv", argv[1]);
    FILE *frames = fopen(path, "w");
    snprintf(path, sizeof path, "%s.events.csv", argv[1]);
    FILE *events = fopen(path, "w");
    if (!frames || !events) {
        perror("output");
        return 1;
    }
    write_frame_header(frames);
    fputs("host_s,sim_s,type,detail\n", events);

    struct fx_state fx;
    fx_init(&fx);
    float sent_low = 0.0f, sent_high = 0.0f;
    int pending_marker = 0;
    double sim_s = 0.0;
    uint64_t host0 = 0;
    bool have_host0 = false;
    unsigned long frame_rows = 0, markers = 0, game_vibrations = 0;

    struct rec_header rh;
    static unsigned char payload[1 << 16];
    while (fread(&rh, sizeof rh, 1, in) == 1) {
        if (rh.size > sizeof payload || fread(payload, rh.size, 1, in) != 1)
            break;
        if (!have_host0) {
            host0 = rh.host_us;
            have_host0 = true;
        }
        double host_s = (double)(rh.host_us - host0) / 1e6;

        if (rh.kind == REC_RUMBLE && rh.size == sizeof(struct rec_rumble)) {
            const struct rec_rumble *r = (const void *)payload;
            sent_low = r->low;
            sent_high = r->high;
            continue;
        }
        if (rh.kind == REC_MARKER && rh.size == sizeof(struct rec_marker)) {
            const struct rec_marker *m = (const void *)payload;
            pending_marker = (int)m->kind;
            markers++;
            fprintf(events, "%.3f,%.3f,marker,%s\n", host_s, sim_s,
                    m->kind == REC_MARKER_MISSED ? "missed" : "false_alarm");
            continue;
        }
        if (rh.kind != REC_PACKET || rh.size < sizeof(struct rumble_pkt_header))
            continue;

        const struct rumble_pkt_header *h = (const void *)payload;
        if (h->type == RUMBLE_PKT_FRAME && rh.size == sizeof(struct rumble_pkt_frame)) {
            const struct rumble_pkt_frame *f = (const void *)payload;
            fx_frame(&fx, f);
            float low, high;
            fx_output(&fx, &cfg, &low, &high);
            sim_s = (double)f->sim_time_us / 1e6;

            unsigned wheels = f->wheel_count < RUMBLE_MAX_WHEELS ? f->wheel_count : RUMBLE_MAX_WHEELS;
            unsigned grounded = 0;
            char surfaces[256] = "";
            for (unsigned i = 0; i < wheels; i++) {
                if (!f->on_ground[i])
                    continue;
                grounded++;
                const char *name = substance_name(f->substance[i]);
                if (!strstr(surfaces, name)) {
                    if (surfaces[0])
                        strncat(surfaces, "|", sizeof surfaces - strlen(surfaces) - 1);
                    strncat(surfaces, name, sizeof surfaces - strlen(surfaces) - 1);
                }
            }

            fprintf(frames,
                    "%.4f,%.4f,%u,%.2f,%.0f,%d,%.3f,%.3f,%.3f,%.3f,%.1f,%u,%u,%u,%u,%u,"
                    "%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%.4f,%.4f,%.4f,%.5f,%.5f,%.4f,%.4f,%.4f,"
                    "%.2f,%.2f,%.2f,%.5f,%.5f,%.5f,%.5f,"
                    "%u,%u,%s",
                    host_s, sim_s, f->frame_index, f->speed * 3.6f, f->rpm, f->gear, f->throttle, f->brake, f->clutch,
                    f->steering, f->cruise_control * 3.6f, f->retarder, f->parking_brake, f->motor_brake,
                    f->engine_enabled, f->trailer_connected, f->lin_acc[0], f->lin_acc[1], f->lin_acc[2],
                    f->ang_vel[0], f->ang_vel[1], f->ang_vel[2], f->ang_acc[0], f->ang_acc[1], f->ang_acc[2],
                    f->cabin_offset[0], f->cabin_offset[1], f->cabin_offset[2], f->cabin_rot[1], f->cabin_rot[2],
                    f->cabin_ang_acc[0], f->cabin_ang_acc[1], f->cabin_ang_acc[2], f->position[0], f->position[1],
                    f->position[2], f->wear_cabin, f->wear_chassis, f->trailer_wear_chassis + f->trailer_wear_body,
                    f->trailer_cargo_damage, wheels, grounded, surfaces[0] ? surfaces : "-");
            for (int i = 0; i < CSV_WHEELS; i++)
                fprintf(frames, i < (int)wheels ? ",%.5f" : ",", f->deflection[i]);
            for (int i = 0; i < CSV_WHEELS; i++)
                fprintf(frames, i < (int)wheels ? ",%.3f" : ",", f->wheel_ang_vel[i]);
            for (int i = 0; i < CSV_WHEELS; i++)
                fprintf(frames, i < (int)wheels ? ",%u" : ",", f->on_ground[i]);
            for (int i = 0; i < CSV_WHEELS; i++)
                fprintf(frames, i < (int)wheels ? ",%s" : ",", substance_name(f->substance[i]));
            fprintf(frames, ",%.5f,%.2f,%.5f,%u,%u,%.3f,%.3f,%.3f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%d\n",
                    fx.dbg.bump_metric, fx.dbg.jolt, fx.dbg.crash_wear, fx.dbg.reset, fx.dbg.hit, fx.bump,
                    fx.collision, fx.surface, fx.game_low, fx.game_high, low, high, sent_low, sent_high,
                    pending_marker);
            pending_marker = 0;
            frame_rows++;
        } else if (h->type == RUMBLE_PKT_SUBSTANCES && rh.size == sizeof(struct rumble_pkt_substances)) {
            const struct rumble_pkt_substances *s = (const void *)payload;
            fx_set_substances(&fx, s);
            bool changed = false;
            for (uint32_t i = 0; i < s->count && i < RUMBLE_MAX_SUBSTANCES; i++) {
                if (strncmp(substance_names[i], s->names[i], RUMBLE_SUBSTANCE_NAME_LEN) != 0)
                    changed = true;
                memcpy(substance_names[i], s->names[i], RUMBLE_SUBSTANCE_NAME_LEN);
                substance_names[i][RUMBLE_SUBSTANCE_NAME_LEN - 1] = '\0';
            }
            if (changed)
                fprintf(events, "%.3f,%.3f,substances,%u\n", host_s, sim_s, s->count);
        } else if (h->type == RUMBLE_PKT_TRUCK && rh.size == sizeof(struct rumble_pkt_truck)) {
            const struct rumble_pkt_truck *t = (const void *)payload;
            static char last[96];
            char desc[96];
            snprintf(desc, sizeof desc, "%.31s %.31s (%u wheels)", t->brand, t->name, t->wheel_count);
            if (strcmp(desc, last)) {
                snprintf(last, sizeof last, "%s", desc);
                fprintf(events, "%.3f,%.3f,truck,%s", host_s, sim_s, desc);
                for (uint32_t i = 0; i < t->wheel_count && i < RUMBLE_MAX_WHEELS; i++)
                    fprintf(events, " | w%u r=%.3f pos=(%.2f %.2f %.2f)%s%s%s", i, t->wheel_radius[i],
                            t->wheel_position[i][0], t->wheel_position[i][1], t->wheel_position[i][2],
                            t->wheel_powered[i] ? " powered" : "", t->wheel_steerable[i] ? " steer" : "",
                            t->wheel_liftable[i] ? " lift" : "");
                fputc('\n', events);
            }
        } else if (h->type == RUMBLE_PKT_EVENT && rh.size == sizeof(struct rumble_pkt_event)) {
            const struct rumble_pkt_event *e = (const void *)payload;
            fprintf(events, "%.3f,%.3f,event,%.47s %.63s %lld\n", host_s, sim_s, e->id, e->detail,
                    (long long)e->amount);
        } else if (h->type == RUMBLE_PKT_GAME_VIBRATION && rh.size == sizeof(struct rumble_pkt_game_vibration)) {
            fx_game_vibration(&fx, (const void *)payload);
            game_vibrations++;
        } else if (h->type == RUMBLE_PKT_HOOK_STATS && rh.size == sizeof(struct rumble_pkt_hook_stats)) {
            const struct rumble_pkt_hook_stats *st = (const void *)payload;
            static uint32_t last_installed = 99, last_calls_nonzero;
            if (st->installed != last_installed || (st->vibration_calls > 0) != last_calls_nonzero) {
                fprintf(events, "%.3f,%.3f,hook,installed=%u type_queries=%u overridden=%u vibration_calls=%u hidden_polls=%u\n",
                        host_s, sim_s, st->installed, st->type_queries, st->type_overrides, st->vibration_calls,
                        st->hidden_polls);
                last_installed = st->installed;
                last_calls_nonzero = st->vibration_calls > 0;
            }
        } else if (h->type == RUMBLE_PKT_PAUSED) {
            fprintf(events, "%.3f,%.3f,paused,\n", host_s, sim_s);
        } else if (h->type == RUMBLE_PKT_QUIT) {
            fprintf(events, "%.3f,%.3f,quit,\n", host_s, sim_s);
        }
    }

    fclose(frames);
    fclose(events);
    fclose(in);
    printf("%lu frames, %lu game vibration commands, %u collisions, %u resets, %lu markers\n", frame_rows,
           game_vibrations, fx.hits, fx.resets, markers);
    return 0;
}
