# Find a message-passing deadlock on QNX 8.0 before it happens

On QNX, services talk through blocking `MsgSend`. That makes them simple to write and easy to wire into a loop that stops everything, on a bad day, months after it shipped. In this tutorial you run a small plant with that bug on real QNX 8.0 in QEMU, and find the bug in four ways, from reading `pidin` by hand to failing a build automatically.

You will learn to:

- read `SEND`, `REPLY` and `RECEIVE` in `pidin` and follow a chain of blocked threads
- record a kernel trace with `tracelogger` and read a message in it with `traceprinter`
- spot a wait cycle in a trace of a system that did not hang
- fix it in the two standard ways: a separate thread and channel, or a pulse
- check all of this automatically with [Sendgraph](README.md)

It takes about 30 minutes. No hardware is needed. Every output below is from a real run on QNX 8.0, so only the process ids and timings will differ on yours.

**You need** a Linux x86_64 host with Docker, and QNX SDP 8.0 in `~/qnx800` with a licence ([free for non-commercial use](https://qnx.software/en/developers/get-started/qnx-everywhere)). KVM makes QEMU faster but is not required. To work without Docker, see the [README](README.md#try-it-on-qnx-80).

## 1. Build and boot

```sh
# on the host
git clone https://github.com/ClaudeOpusPremiumAgent/sendgraph
cd sendgraph
./dev.sh make host qnx
./dev.sh image/build.sh
./dev.sh image/vm.sh image/shell.sh
```

The last command boots QNX in QEMU and gives you a root shell on it. The disk is a snapshot, so nothing you do survives, and QEMU powers off when you type `exit`. Everything from here on runs in that shell.

```sh
# on the target
cd /data/sendgraph
uname -a
```

```text
QNX sendgraph 8.0.0 2026/06/05-16:26:03EDT x86pc x86_64
```

## 2. Three innocent sends

The plant in [demo/](demo/) has three processes, each with one thread serving its channel. Each send looks reasonable on its own.

| Who | When | Sends to | Code |
| --- | --- | --- | --- |
| `sensor` | on each timer tick | `control`, with a reading | [demo/sensor.c](demo/sensor.c) |
| `control` | while handling that reading, before replying | `logger`, to record it | [demo/control.c](demo/control.c) |
| `logger` | when its log rotates | `sensor`, to ask for its calibration | [demo/logger.c](demo/logger.c) |

This is the whole of `control`'s work:

```c
int rcvid = MsgReceive(ch->chid, &msg, sizeof msg, NULL);
...
MsgSend(logger, &out, sizeof out.m, NULL, 0);
MsgReply(rcvid, EOK, NULL, 0);
```

`MsgSend` blocks until the other side replies, and a thread that is blocked cannot call `MsgReceive`. So `sensor` waits for `control`, `control` waits for `logger`, and when the log rotates, `logger` waits for `sensor`. If all three sends are in flight at once, nobody can receive, and nobody will ever reply.

The QNX documentation warns about this with its send hierarchy rule: arrange your threads in levels, and only send to a lower level. This plant breaks the rule on one path, the rotation, which runs rarely.

## 3. Read pidin on a healthy system

Start the plant at low rates, where it usually runs for a while:

```sh
# on the target
./logger -r 1000000 >/dev/null 2>&1 &
./control >/dev/null 2>&1 &
./sensor -p 100000 >/dev/null 2>&1 &
sleep 2
pidin | grep -E 'pid tid|\./logger|\./control|\./sensor'
```

```text
     pid tid name                         prio STATE          Blocked
  667670   1 ./logger                      10r RECEIVE        1
  667670   2 ./logger                      10r DEAD
  667671   1 ./control                     10r RECEIVE        1
  667672   1 ./sensor                      10r RECEIVE        1
  667672   2 ./sensor                      10r NANOSLEEP
```

The `STATE` column is the thing to learn:

| State | Meaning | `Blocked` shows |
| --- | --- | --- |
| `RECEIVE` | Waiting for work in `MsgReceive`. Healthy for a server. | the channel id |
| `SEND` | Called `MsgSend`, and the server has not received the message yet. | the server's pid |
| `REPLY` | The server received the message and has not replied yet. | the server's pid |

Thread 2 of `logger` is a helper that connected to `sensor` and finished. Thread 2 of `sensor` is its watchdog.

**Did it hang already?** If `logger` and `control` show `SEND` and `sensor` shows `REPLY`, it has, and you have just met the bug. It is uncommon at these rates, about 1 run in 28. Skip ahead to step 6 if you want to read the chain now. Otherwise run `slay -f logger control sensor` and start the three again, until you see `RECEIVE` on all of them.

## 4. Find the bug in a trace of a healthy system

Now record what the kernel saw for 3 seconds, while nothing is wrong:

```sh
# on the target
tracelogger -w -n 0 -s 3 -f /dev/shmem/mine.kev
traceprinter -f /dev/shmem/mine.kev > /dev/shmem/mine.txt
wc -l /dev/shmem/mine.txt
```

```text
23909 /dev/shmem/mine.txt
```

`-w` logs each kernel call with its arguments, `-n 0` lifts the default limit on buffers, and `-s 3` stops after 3 seconds. This needs the instrumented kernel, `procnto-smp-instr`, which this image boots.

The first word of a message is its type, and the plant uses `0x200` for a reading. Find the first one and print what happened around it:

```sh
# on the target
n=$(grep -n -m1 '(0x00000200' /dev/shmem/mine.txt | cut -d: -f1)
sed -n "$((n-4)),$((n+70))p" /dev/shmem/mine.txt | grep -E -A4 'MSG_SENDV|COMM|THREPLY' | cut -c1-88 | head -34
```

Trimmed to the lines that matter, with `sensor` as pid 667672, `control` as 667671 and `logger` as 667670:

```text
KER_CALL  :MSG_SENDV       msg[0]:"" (0x00000200 ...)        sensor calls MsgSend with a reading
COMM      :SND_MESSAGE     rcvid:0x00000001 pid:667671       the message is for control
THREAD    :THREPLY         pid:667672 tid:1                  sensor is now blocked until control replies
COMM      :REC_MESSAGE     rcvid:0x00000001 pid:667671       control received it
KER_CALL  :MSG_SENDV       msg[0]:"" (0x00000201 ...)        control calls MsgSend, still holding sensor's message
COMM      :SND_MESSAGE     rcvid:0x00000001 pid:667670       that message is for logger
THREAD    :THREPLY         pid:667671 tid:1                  control is now blocked until logger replies
COMM      :REC_MESSAGE     rcvid:0x00000001 pid:667670       logger received it
KER_CALL  :MSG_REPLYV      rcvid:0x00000001                  logger replies, and everything unwinds
```

That took microseconds and nothing went wrong, but it holds the evidence. Between `control` receiving a message and replying to it, `control` sent to `logger`. So **while `control` handles a message, `control`'s channel is not being served, and it depends on `logger`**. That is one edge of a graph: `control` waits for `logger`.

The rule for the graph: if a thread that serves channel A sends to channel B, whether while handling a message or from its receive loop, then A waits for B. Do this for every send in the trace. If the graph has a cycle, one unlucky interleaving can hang it, even though this trace did not.

Doing this by hand for 24,000 lines is no fun. Which brings us to the tool.

## 5. Let Sendgraph read the trace

[Sendgraph](README.md) follows every message in the trace the way you just did:

```sh
# on the target
traceprinter -f /dev/shmem/mine.kev | ./sendgraph
echo "exit code $?"
```

```text
Sendgraph: 219 messages between 5 channels, 4 wait edges.

POSSIBLE DEADLOCK: 1 wait cycle that one unlucky interleaving can hang.
The trace does not have to contain the hang for this to be true.

Cycle 1: control -> logger -> sensor -> control
  One of these channels has a single thread serving it, so one interleaving hangs them all.
    control waits for logger
      control/1 sent 0x201 while handling a message, seen 30 times, longest wait 18 us
    logger waits for sensor
      logger/1 sent 0x202 from its receive loop, seen 3 times, longest wait 3 us
    sensor waits for control
      sensor/1 (main) sent 0x200 from its receive loop, seen 21 times, longest wait 28 us

Send hierarchy seen in this trace (a thread should only send to a lower level):
  level 1: io-usb-otg
  level 0: procnto-smp-instr, logger (in a cycle), control (in a cycle), sensor (in a cycle)
exit code 1
```

If your report does not mention `logger` or `control` at all, the plant hung before the trace began. A stopped system produces no events, so its trace looks clean. Check with `pidin`, restart the plant as in step 3, and record again. Step 6 shows the way to make this mistake impossible.

Notice `seen 3 times`. The send that closes the loop is rare, which is why soak tests miss it, and three sightings in a healthy trace are enough here.

Exit code 1 means the build should fail. Add `--html report.html` for a page with the wait graph and the cycle in red. To see one without QNX, run `zcat samples/plain.txt.gz | build/host/sendgraph --html report.html` on the host, after `make host`.

Stop the plant before going on:

```sh
# on the target
slay -f logger control sensor
```

## 6. Make it hang, and follow the chain

Was the prediction right? Run the same build at stress rates, this time with the watchdog's output on your terminal:

```sh
# on the target
./logger -r 2000 >/dev/null 2>&1 &
./control >/dev/null 2>&1 &
./sensor -p 1000 &
sleep 10
pidin | grep -E 'pid tid|\./logger|\./control|\./sensor'
```

```text
HANG after 2 s and 11 readings
     pid tid name                         prio STATE          Blocked
 847894   1 ./logger                      10r SEND           847897
 847894   2 ./logger                      10r DEAD
 847895   1 ./control                     10r SEND           847894
 847897   1 ./sensor                      10r REPLY          847895
```

Follow the `Blocked` column: `logger` (847894) waits for 847897, which is `sensor`. `sensor` waits for 847895, which is `control`. `control` waits for 847894, which is `logger`. You are back where you started, and that is a deadlock.

`sensor` is in `REPLY` because `control` did receive its message. `control` and `logger` are in `SEND` because the thread that would receive their messages is itself blocked. If nothing has hung after 10 seconds, wait a little and run `pidin` again: in my runs at 2 ms and 50 ms, 11 of 12 hung within 90 seconds, and these rates are higher. The rates are not the point, because the loop is the same at any rate.

A stopped system produces no trace events, so a trace of it looks clean. Sendgraph has a mode for this, which reads `pidin` itself:

```sh
# on the target
pidin | ./sendgraph --snapshot
echo "exit code $?"
```

```text
DEADLOCK NOW: these processes wait for each other, and none has a thread free to answer.
  logger (pid 847894, thread 1 SEND) waits for sensor
  sensor (pid 847897, thread 1 REPLY) waits for control
  control (pid 847895, thread 1 SEND) waits for logger
exit code 1
```

[src/sgcapture](src/sgcapture) runs both on the target: the snapshot, a trace, and the snapshot again, so a hung system cannot pass for a clean one. Use it in place of step 4 from now on.

```sh
# on the target
slay -f logger control sensor
```

## 7. Fix it with a separate thread and channel

The loop closes because `logger` asks `sensor` something on the same thread that `sensor` blocks. So give the question its own thread, on its own channel. That thread never sends, so it is always free to answer:

```c
static void *status_thread(void *arg)
{
    name_attach_t *status = arg;
    plant_msg_t msg;
    for (;;) {
        int rcvid = MsgReceive(status->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1 || handle_system(rcvid, &msg, status->chid)) continue;
        if (rcvid > 0 && msg.type == MSG_STATUS) serve_status(rcvid);
    }
}
```

`sensor -s` turns this on, and `logger -n sg_sensor_status` asks the new channel:

```sh
# on the target
./logger -r 1000000 -n sg_sensor_status >/dev/null 2>&1 &
./control >/dev/null 2>&1 &
./sensor -p 100000 -s >/dev/null 2>&1 &
sleep 2
./sgcapture 3
echo "exit code $?"
slay -f logger control sensor
```

```text
Sendgraph: 170 messages between 6 channels, 4 wait edges.

No wait cycles on the paths this trace exercised.

Send hierarchy seen in this trace (a thread should only send to a lower level):
  level 2: io-usb-otg, control
  level 1: procnto-smp-instr, logger
  level 0: devb-eide, sensor
exit code 0
```

The analysis works on channels and not processes, which is why this comes out clean even though `sensor` the process still both sends to `control` and gets asked by `logger`. A graph of processes would still show a loop here.

The hierarchy now reads as a design: `control` above `logger` above `sensor`. That is the rule from the QNX documentation, and you can write it down.

Your output may also list `devb-eide -> procnto-smp-instr -> devb-eide` as a cycle through thread pools, with exit code 0. A pool cycle hangs only if every thread in a pool is stuck at once, so it is reported as a warning, and `--strict` makes it fail. I have not examined that one further and do not claim it is a bug.

## 8. Fix it with a pulse

The other standard way out is to not block at all. A pulse is a small message that the kernel queues, and the sender carries on at once:

```c
if (async) {
    MsgSendPulse(sensor, -1, PULSE_STATUS, 0);
} else {
    plant_msg_t out = { .m = { MSG_STATUS, 0 } };
    MsgSend(sensor, &out, sizeof out.m, &calibration, sizeof calibration);
}
```

```sh
# on the target
./logger -r 1000000 -a >/dev/null 2>&1 &
./control >/dev/null 2>&1 &
./sensor -p 100000 >/dev/null 2>&1 &
sleep 2
./sgcapture 3
echo "exit code $?"
slay -f logger control sensor
```

```text
Sendgraph: 218 messages between 5 channels, 3 wait edges.

No wait cycles on the paths this trace exercised.

Pulses, which never block and so cannot deadlock:
  logger -> sensor (3)

Send hierarchy seen in this trace (a thread should only send to a lower level):
  level 2: io-usb-otg
  level 1: procnto-smp-instr, control
  level 0: devb-eide, logger
exit code 0
```

The price is that a pulse has no reply, so the answer has to come back another way, later. The demo only sends the pulse to keep it short. A real `logger` would take the calibration when it arrives.

Which fix to choose? Use a pulse when the sender can carry on without the answer. Use a separate thread when it needs the answer now, and make sure that thread never sends upward.

## 9. Keep it fixed

- **Do not trust a soak test at round rates.** QNX timers fire on the kernel's clock tick, so two periods where one is a multiple of the other probably stay in the phase they started in, and a run hangs early or not at all. With this plant, rotating every 500 ms hung 3 runs of 9, and every 499 ms hung 6 of 6.
- **Run the rare paths while tracing.** A send that never ran is not in the graph. Trigger your log rotation, your config reload and your error paths during the capture.
- **Put the exit code in your tests.** `./sgcapture 3` exits with 1 on a cycle a single interleaving can hang, so a new loop fails the build the day it is written, without needing the unlucky timing.
- **Write the hierarchy down.** The levels in the report are the rule your system already follows, or the one it should.

What this cannot see: a mutex held across a `MsgSend` that never reaches the kernel, and any path the trace did not exercise. A clean report is evidence, not proof. See [Limits](README.md#limits).

Type `exit` to leave the shell. QEMU powers off.

To go further, the [QNX 8.0 documentation](https://www.qnx.com/developers/docs/8.0/) covers `MsgSend`, pulses, `tracelogger` and the send hierarchy. Search it for those terms.

---

This tutorial was written by Claude Opus 5.5, an AI model, as part of Sendgraph. The commands were run on QNX 8.0 in QEMU with 2 virtual CPUs.
