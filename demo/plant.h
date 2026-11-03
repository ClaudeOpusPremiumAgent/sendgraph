/* A small plant: sensor sends readings to control, control records them through logger,
 * and logger asks sensor for its calibration when it rotates its log. */
#ifndef PLANT_H
#define PLANT_H
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>

#define MSG_READING (_IO_MAX + 1)
#define MSG_RECORD  (_IO_MAX + 2)
#define MSG_STATUS  (_IO_MAX + 3)
#define PULSE_TICK  (_PULSE_CODE_MINAVAIL + 0)
#define PULSE_ROTATE (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_STATUS (_PULSE_CODE_MINAVAIL + 2)

typedef union {
    uint16_t type;
    struct _pulse pulse;
    struct { uint16_t type; int32_t value; } m;
} plant_msg_t;

static inline int connect_to(const char *name)
{
    for (int i = 0; i < 200; i++) {
        int coid = name_open(name, 0);
        if (coid != -1) return coid;
        usleep(10000);
    }
    fprintf(stderr, "cannot reach %s: %s\n", name, strerror(errno));
    exit(1);
}

/* Handles what name_attach channels receive besides our own messages. Returns 1 if it was handled. */
static inline int handle_system(int rcvid, plant_msg_t *msg, int chid)
{
    (void)chid;
    if (rcvid == 0) {
        if (msg->pulse.code == _PULSE_CODE_DISCONNECT) { ConnectDetach(msg->pulse.scoid); return 1; }
        return msg->pulse.code < 0;
    }
    if (msg->type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); return 1; }
    if (msg->type < _IO_MAX) { MsgError(rcvid, ENOSYS); return 1; }
    return 0;
}

/* Delivers a pulse to this channel every period_us. */
static inline void pulse_every(int chid, int code, long period_us)
{
    struct sigevent ev;
    struct itimerspec it = { 0 };
    timer_t timer;
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    SIGEV_PULSE_INIT(&ev, coid, SIGEV_PULSE_PRIO_INHERIT, code, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) { perror("timer_create"); exit(1); }
    it.it_value.tv_sec = period_us / 1000000;
    it.it_value.tv_nsec = (period_us % 1000000) * 1000;
    it.it_interval = it.it_value;
    if (timer_settime(timer, 0, &it, NULL) == -1) { perror("timer_settime"); exit(1); }
}
#endif
