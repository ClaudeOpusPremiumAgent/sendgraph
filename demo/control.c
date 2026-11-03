/* usage: control    Records each reading through logger before answering sensor. */
#include "plant.h"

int main(void)
{
    name_attach_t *ch = name_attach(NULL, "sg_control", 0);
    if (!ch) { perror("name_attach"); return 1; }
    int logger = connect_to("sg_logger");
    for (;;) {
        plant_msg_t msg;
        int rcvid = MsgReceive(ch->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1 || handle_system(rcvid, &msg, ch->chid)) continue;
        if (rcvid > 0 && msg.type == MSG_READING) {
            plant_msg_t out = { .m = { MSG_RECORD, msg.m.value } };
            MsgSend(logger, &out, sizeof out.m, NULL, 0);
            MsgReply(rcvid, EOK, NULL, 0);
        }
    }
}
