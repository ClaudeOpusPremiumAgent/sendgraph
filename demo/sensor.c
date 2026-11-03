/* usage: sensor [-p period_us] [-s] [-t seconds]
 *   -s  serve status queries on a separate channel and thread
 *   -t  stop after this many seconds if nothing has hung */
#include <pthread.h>
#include "plant.h"

static volatile long readings;
static long limit_s;

static void *watchdog(void *arg)
{
    (void)arg;
    long last = -1;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        sleep(2);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (readings == last) {
            printf("HANG after %ld s and %ld readings\n", (long)(now.tv_sec - start.tv_sec) - 2, last);
            fflush(stdout);
            _exit(3);
        }
        last = readings;
        if (limit_s && now.tv_sec - start.tv_sec >= limit_s) {
            printf("OK for %ld s and %ld readings\n", (long)(now.tv_sec - start.tv_sec), last);
            fflush(stdout);
            _exit(0);
        }
    }
    return NULL;
}

static void serve_status(int rcvid)
{
    int32_t calibration = 42;
    MsgReply(rcvid, EOK, &calibration, sizeof calibration);
}

static void *status_thread(void *arg)
{
    name_attach_t *status = arg;
    plant_msg_t msg;
    pthread_setname_np(0, "status");
    for (;;) {
        int rcvid = MsgReceive(status->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1 || handle_system(rcvid, &msg, status->chid)) continue;
        if (rcvid > 0 && msg.type == MSG_STATUS) serve_status(rcvid);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    long period_us = 2000;
    int separate = 0, opt;
    while ((opt = getopt(argc, argv, "p:st:")) != -1) {
        if (opt == 'p') period_us = atol(optarg);
        if (opt == 's') separate = 1;
        if (opt == 't') limit_s = atol(optarg);
    }
    name_attach_t *main_ch = name_attach(NULL, "sg_sensor", 0);
    if (!main_ch) { perror("name_attach"); return 1; }
    if (separate) {
        name_attach_t *status = name_attach(NULL, "sg_sensor_status", 0);
        if (!status) { perror("name_attach"); return 1; }
        pthread_create(NULL, NULL, status_thread, status);
    }
    pthread_setname_np(0, "main");
    int control = connect_to("sg_control");
    pulse_every(main_ch->chid, PULSE_TICK, period_us);
    pthread_create(NULL, NULL, watchdog, NULL);
    for (;;) {
        plant_msg_t msg;
        int rcvid = MsgReceive(main_ch->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1 || (rcvid > 0 && handle_system(rcvid, &msg, main_ch->chid))) continue;
        if (rcvid == 0) {
            if (msg.pulse.code == PULSE_TICK) {
                plant_msg_t out = { .m = { MSG_READING, (int32_t)readings } };
                if (MsgSend(control, &out, sizeof out.m, NULL, 0) == -1) { perror("send"); return 1; }
                readings++;
            } else {
                handle_system(0, &msg, main_ch->chid);
            }
        } else if (msg.type == MSG_STATUS) {
            serve_status(rcvid);
        }
    }
}
