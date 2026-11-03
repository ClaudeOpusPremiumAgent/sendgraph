# Sendgraph

Finds message-passing deadlocks on QNX 8.0 before they happen. It reads a few seconds of a healthy kernel trace and reports the cycle of blocking `MsgSend` calls that can hang your system, even when nothing hung during the trace.

For QNX 8.0, on QEMU x86_64 or any target that can run `tracelogger`. Built with `qcc` from SDP 8.0 (GCC 12.2) for the target and `g++` for the host. Written by an AI model, with a human operator: see [How this was built](#how-this-was-built).

## The problem

On QNX, services talk through blocking `MsgSend`. A thread that sends cannot receive until the answer comes back. If A sends to B, B sends to C and C sends to A at the wrong moment, all three stop for good, and the QNX documentation warns against exactly this with its send hierarchy rule: threads should only send to a lower level.

The rule is easy to break by accident and hard to test, because the hang needs an unlucky interleaving. The plant in [demo/](demo/) has that bug on one rare path, and this is what the bug does at two rates:

RESULTS_TABLE

`pidin` can show you the three blocked threads once it has happened. Sendgraph tells you from a trace of the system working normally:

```text
POSSIBLE DEADLOCK: 1 wait cycle that one unlucky interleaving can hang.

Cycle 1: control -> logger -> sensor -> control
  One of these channels has a single thread serving it, so one interleaving hangs them all.
    control waits for logger
      control/1 sent 0x201 while handling a message, seen 300 times, longest wait 9 us
    logger waits for sensor
      logger/1 sent 0x202 from its receive loop, seen 5 times, longest wait 3 us
    sensor waits for control
      sensor/1 (main) sent 0x200 from its receive loop, seen 300 times, longest wait 23 us
```

## How it works

1. `tracelogger` records the kernel's message events. `traceprinter` turns them into text, and that text is Sendgraph's input.
2. Sendgraph follows each message from `SND_MESSAGE` to `REC_MESSAGE` to `MsgReply`, and works out which thread served which channel.
3. A thread that serves channel A and sends to channel B means "A waits for B", whether the send happens while handling a message or from the receive loop. A worker that answers a message on the receiver's behalf carries the same wait.
4. A cycle of those waits is the report. The analysis works on channels and not processes, so a process that keeps a separate thread and channel for callbacks is correctly clean. A process-level graph would flag it.
5. A thread pool changes the meaning, so the report says how many requests must be stuck at once. A cycle where one channel has a single thread fails the exit code. A cycle where every channel has a pool is reported as a warning, and fails only with `--strict`.

A system that has already stopped produces no trace events, so a trace of it shows nothing. For that case, `pidin | sendgraph --snapshot` reads who each blocked thread waits for and confirms the loop.

## Try it without QNX

You need `g++`, `make` and `python3`. Real traces from QNX 8.0 are in [samples/](samples/).

```sh
make host test
zcat samples/plain.txt.gz | build/host/sendgraph --html report.html
```

See `17 passed`, then the cycle above with exit code 1. Then see the two ways to fix it come out clean:

```sh
zcat samples/thread.txt.gz | build/host/sendgraph
zcat samples/pulse.txt.gz | build/host/sendgraph
```

Open `report.html` to see the wait graph with the cycle in red.

## Try it on QNX 8.0

You need QNX SDP 8.0 in `~/qnx800` with a licence ([free for non-commercial use](https://qnx.software/en/developers/get-started/qnx-everywhere)) and Docker. Only this directory, the SDP (read-only) and `~/.qnx` are mounted.

```sh
./dev.sh make host qnx
./dev.sh image/build.sh
./dev.sh image/vm.sh image/demo.sh
```

See each build of the plant traced on a real QNX 8.0 system in QEMU, then the plain build hung and confirmed with `--snapshot`. The reports land in `image/out/`.

Without Docker, source `qnxsdp-env.sh`, install `qemu-system-x86_64`, and run the same commands without `./dev.sh`.

On your own system:

```sh
# on the target (needs procnto-smp-instr, which the stock image uses)
tracelogger -w -n 0 -s 3 -f /tmp/mine.kev
# on the host
traceprinter -f mine.kev | sendgraph --html report.html
```

Or run it all on the target with [src/sgcapture](src/sgcapture). Put the exit code in CI and a new wait cycle fails the build.

## What the report gives you

| Output | Use |
| --- | --- |
| Text and exit code | Exit 0 is clean, 1 is a cycle a single interleaving can hang, 2 is unusable input. |
| `--html FILE` | A self-contained page with the wait graph, cycles in red, and the evidence for each wait. |
| `--json FILE`, `--dot FILE` | For scripts and Graphviz. |
| Send hierarchy | The levels your system already follows, so you can write the rule down. |
| Longest wait per edge | The worst blocking seen on each edge, which matters on a real-time path. |

## Limits

- **It sees the paths the trace exercised**, like lockdep. A send that never ran during the capture is not in the graph, so run your rare paths while tracing.
- **Message passing only.** A mutex taken without contention never reaches the kernel, so a mutex held across `MsgSend` is not modelled.
- **It reports what can block, not what will.** A send guarded by `TimerTimeout` is still reported, though it would stall and not hang.
- **Pool sizes are what the trace saw.** A pool that grows on demand can be larger than reported.
- **It depends on `traceprinter`'s text format**, as printed by SDP 8.0 (traceprinter 1.02).
- **System services can appear.** On the stock image it reports `devb-eide -> procnto -> devb-eide` as a cycle through pools. I do not claim that is a bug: `procnto`'s pool grows on demand and I have not examined it further.
- **Tested on x86_64 in QEMU only**, with 2 virtual CPUs. The trace format should be the same on aarch64, which I have not run.

## A note on mkqnximage

The `mkqnximage` in SDP 8.0 leaves out `sshd-auth`, and OpenSSH in the same SDP refuses to start without it. [image/build.sh](image/build.sh) adds it through a snippet. This repository holds no QNX binaries: the image is built from your own SDP.

## Layout

```text
src/sendgraph.cpp   the analyzer: one C++17 file, no dependencies, builds with g++ and qcc
src/sgcapture       capture and check on the target
demo/               the plant, in C, with QNX message passing: sensor, control, logger
tests/run.py        tests, one rule each, on traces in traceprinter's format
samples/            real QNX 8.0 traces, reduced by tools/slim.py to what the analyzer reads
image/              QEMU image and scripts, using mkqnximage
```

## How this was built

All code, tests and documents here were written by Claude Opus 5.5, an AI model, working as an agent with a human operator, who set the goal, installed the tools and publishes the work. First commit 2026-11-03. MIT license.
