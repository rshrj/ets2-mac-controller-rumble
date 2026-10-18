// Sends a scripted drive to rumble_helper so effects can be felt without the
// game: smooth road, braking, a pothole, gravel, a rumble strip, then a crash.
// Build: cc fake_telemetry.c -o fake_telemetry
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

#define WHEELS 6
#define HZ 60

static int sock;
static struct sockaddr_in addr;

static void send_pkt(const void *data, size_t size) {
    sendto(sock, data, size, 0, (struct sockaddr *)&addr, sizeof addr);
}

int main(void) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RUMBLE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct rumble_pkt_substances subs;
    memset(&subs, 0, sizeof subs);
    subs.h.magic = RUMBLE_MAGIC;
    subs.h.type = RUMBLE_PKT_SUBSTANCES;
    subs.count = 4;
    strcpy(subs.names[0], "road");
    strcpy(subs.names[1], "gravel");
    strcpy(subs.names[2], "rumble_stripe");
    strcpy(subs.names[3], "grass");
    send_pkt(&subs, sizeof subs);

    struct rumble_pkt_truck truck;
    memset(&truck, 0, sizeof truck);
    truck.h.magic = RUMBLE_MAGIC;
    truck.h.type = RUMBLE_PKT_TRUCK;
    truck.wheel_count = WHEELS;
    strcpy(truck.brand, "Fake");
    strcpy(truck.name, "Truck");
    for (int w = 0; w < WHEELS; w++)
        truck.wheel_radius[w] = 0.5f;
    send_pkt(&truck, sizeof truck);

    struct rumble_pkt_frame f;
    memset(&f, 0, sizeof f);
    f.h.magic = RUMBLE_MAGIC;
    f.h.type = RUMBLE_PKT_FRAME;
    f.wheel_count = WHEELS;
    f.engine_enabled = 1;
    f.rpm = 1300.0f;
    f.speed = 50.0f / 3.6f;
    for (int w = 0; w < WHEELS; w++)
        f.on_ground[w] = 1;

    const char *phase = "";
    for (int i = 0; i < 10 * HZ; i++) {
        float t = (float)i / HZ;
        const char *now;
        uint32_t left = 0, right = 0;
        float bump = 0.0f, dive = 0.0f;
        f.lin_acc[0] = f.lin_acc[2] = 0.0f;

        if (t < 1.5f) {
            now = "smooth road, 50 km/h (should be still)";
        } else if (t < 2.5f) {
            now = "hard braking: slow suspension dive (should be still)";
            dive = 0.04f * (t - 1.5f);
            f.lin_acc[2] = 5.0f;
        } else if (t < 3.5f) {
            now = "pothole at 2.6s";
            if (t >= 2.6f && t < 2.65f) bump = 0.03f;
        } else if (t < 5.0f) {
            now = "gravel";
            left = right = 1;
        } else if (t < 6.0f) {
            now = "road";
        } else if (t < 7.5f) {
            now = "rumble strip under the left wheels";
            left = 2;
        } else if (t < 8.5f) {
            now = "road";
        } else {
            now = "crash at 8.6s";
            if (i == (int)(8.6f * HZ)) {
                f.wear_chassis += 0.01f;
                f.lin_acc[2] = -30.0f;
            }
            if (t > 8.6f) f.speed = 0.0f;
        }
        if (strcmp(now, phase) != 0) {
            printf("%4.1fs  %s\n", t, now);
            fflush(stdout);
            phase = now;
        }

        f.sim_time_us = (uint64_t)i * 1000000 / HZ;
        f.frame_index = (uint32_t)i;
        f.position[2] -= f.speed / HZ;
        for (int w = 0; w < WHEELS; w++) {
            f.substance[w] = (w % 2 == 0) ? left : right;
            f.deflection[w] = bump + (w < 2 ? dive : -dive * 0.5f);
        }
        send_pkt(&f, sizeof f);
        usleep(1000000 / HZ);
    }

    struct rumble_pkt_header quit = {RUMBLE_MAGIC, RUMBLE_PKT_QUIT};
    send_pkt(&quit, sizeof quit);
    printf("done\n");
    close(sock);
    return 0;
}
