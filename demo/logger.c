/* usage: logger [-r rotate_us] [-a] [-n name]
 *   -r  how often the log rotates
 *   -a  ask with a pulse and take the answer later, so rotation never blocks
 *   -n  the name to ask: sg_sensor (default) or sg_sensor_status */
#include <pthread.h>
#include "plant.h"

static volatile int sensor = -1;
static const char *ask = "sg_sensor";
static long rotate_us = 50000;
static int own_chid;

/* name_open itself is a message to sensor, so it runs here, away from the thread control is waiting on. */
static void *connector(void *arg)
{
    (void)arg;
    sensor = connect_to(ask);
    pulse_every(own_chid, PULSE_ROTATE, rotate_us);
    return NULL;
}

int main(int argc, char **argv)
{
    int async = 0, opt;
    while ((opt = getopt(argc, argv, "r:an:")) != -1) {
        if (opt == 'r') rotate_us = atol(optarg);
        if (opt == 'a') async = 1;
        if (opt == 'n') ask = optarg;
    }
    name_attach_t *ch = name_attach(NULL, "sg_logger", 0);
    if (!ch) { perror("name_attach"); return 1; }
    own_chid = ch->chid;
    pthread_create(NULL, NULL, connector, NULL);
    long records = 0;
    int32_t calibration = 0;
    for (;;) {
        plant_msg_t msg;
        int rcvid = MsgReceive(ch->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1) continue;
        if (rcvid == 0 && msg.pulse.code == PULSE_ROTATE) {
            if (async) {
                MsgSendPulse(sensor, -1, PULSE_STATUS, 0);
            } else {
                plant_msg_t out = { .m = { MSG_STATUS, 0 } };
                MsgSend(sensor, &out, sizeof out.m, &calibration, sizeof calibration);
            }
            continue;
        }
        if (handle_system(rcvid, &msg, ch->chid)) continue;
        if (rcvid > 0 && msg.type == MSG_RECORD) {
            records++;
            MsgReply(rcvid, EOK, NULL, 0);
        }
    }
}
