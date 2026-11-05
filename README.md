# Sendgraph

Finds message-passing deadlocks on QNX 8.0 before they happen. It reads a few seconds of a healthy kernel trace and reports the cycle of blocking `MsgSend` calls that can hang your system, even when nothing hung during the trace.

For QNX 8.0, on QEMU x86_64 or any target that can run `tracelogger`. Built with `qcc` from SDP 8.0 (GCC 12.2) for the target and `g++` for the host. Written by an AI model, with a human operator: see [How this was built](#how-this-was-built). New to this? The [tutorial](TUTORIAL.md) walks through finding this bug by hand on QNX 8.0 in QEMU, then with the tool.

## The problem

On QNX, services talk through blocking `MsgSend`. A thread that sends cannot receive until the answer comes back. If A sends to B, B sends to C and C sends to A at the wrong moment, all three stop for good, and the QNX documentation warns against exactly this with its send hierarchy rule: threads should only send to a lower level.

The rule is easy to break by accident and hard to test, because the hang needs an unlucky interleaving. The plant in [demo/](demo/) has that bug on one path, which runs when its log rotates. Measured on QNX 8.0 in QEMU with 2 virtual CPUs:

| Reading every | Log rotates every | Runs that hung | Limit per run |
| --- | --- | --- | --- |
| 2 ms | 50 ms | 11 of 12 | 90 s |
| 5 ms | 200 ms | 4 of 6 | 90 s |
| 10 ms | 499 ms | 6 of 6, all within about 8 s | 60 s |
| 10 ms | 500 ms | 3 of 9, each within about 6 s | 60 or 300 s |
| 10 ms | 1 s | 0 of 3 | 120 s |

The last row is not a safe rate. With `sgcapture` tracing the plant at 10 ms and 1 s, 6 of 20 runs hung within about 4 s. Moving the rotation by 1 ms decides whether a soak test catches this. At 500 ms, six of the nine runs passed their whole soak, up to 5 minutes. At 499 ms, every run hung within seconds. The likely reason is that QNX timers fire on the kernel's clock tick, so when one period is a multiple of the other, the two stay in whatever phase they started in. A run then hangs early or not at all, while 499 ms sweeps through every phase. I measured this and did not prove the cause. It would also explain why the demo below reads every 100 ms: at 100 ms and 1 s, only 1 of 28 traced runs hung. A passing soak test says little about this kind of bug.

The two fixes held at the rates that hung most: the `thread` and `pulse` builds had 0 hangs in 6 runs of 90 s at 2 ms and 50 ms, and 0 hangs in 4 runs of 60 s each at 10 ms and 499 ms.

`pidin` can show you the three blocked threads once it has happened. Sendgraph tells you from a trace of the system working normally:

```text
POSSIBLE DEADLOCK: 1 wait cycle that one unlucky interleaving can hang.
The trace does not have to contain the hang for this to be true.

Cycle 1: control -> logger -> sensor -> control
  One of these channels has a single thread serving it, so one interleaving hangs them all.
    control waits for logger
      control/1 sent 0x201 while handling a message, seen 300 times, longest wait 9 us
    logger waits for sensor
      logger/1 sent 0x202 from its receive loop, seen 5 times, longest wait 3 us
    sensor waits for control
      sensor/1 (main) sent 0x200 from its receive loop, seen 300 times, longest wait 23 us
```

## Why this matters on QNX

QNX is a microkernel, so drivers, file systems and services are separate processes, and everything between them goes through message passing. Any QNX system has a wait graph, whether anyone drew it or not.

Blocking sends are a strength here. The server inherits the client's priority, and the client has no queue to overflow, which is much of why a real-time system is built this way. Replacing them with queues would give that up, so the useful thing is to keep them and check the graph.

The systems QNX runs in are cars, medical devices and factories, where a service that silently stops answering is a bug you want to find on the bench. `pidin` shows you this bug after it has happened. Sendgraph reads the same kernel's events earlier, with no code changes, and gives the send hierarchy rule an exit code. It also reports the longest wait on each edge, which matters on a real-time path.

## How it works

1. `tracelogger` records the kernel's message events. `traceprinter` turns them into text, and that text is Sendgraph's input.
2. Sendgraph follows each message from `SND_MESSAGE` to `REC_MESSAGE` to `MsgReply`, and works out which thread served which channel.
3. A thread that serves channel A and sends to channel B means "A waits for B", whether the send happens while handling a message or from the receive loop. A worker that answers a message on the receiver's behalf carries the same wait.
4. A cycle of those waits is the report. The analysis works on channels and not processes, so a process that keeps a separate thread and channel for callbacks is correctly clean. A process-level graph would flag it.
5. A thread pool changes the meaning, so the report says how many requests must be stuck at once. A cycle where one channel has a single thread fails the exit code. A cycle where every channel has a pool is reported as a warning, and fails only with `--strict`.

A system that has already stopped produces no trace events, so a trace alone would look clean. For that case, `pidin | sendgraph --snapshot` reads who each blocked thread waits for and confirms the loop. [src/sgcapture](src/sgcapture) does this before and after the trace, because a real run hung in the moment between the first check and the start of the trace. If you run `tracelogger` by hand, run the snapshot too.

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

See each build of the plant traced on a real QNX 8.0 system in QEMU, then the plain build hung and confirmed by snapshot. The plain build can hang before its trace ends, in 1 of 28 runs at the demo's rates. That is the same bug, so `demo.sh` says so and tries again until it has a healthy trace. The reports land in `image/out/`.

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
src/sgcapture       capture and check on the target, using tracelogger, traceprinter and pidin
demo/               the plant, in C, with QNX message passing: sensor, control, logger
TUTORIAL.md         find this bug by hand, then with the tool, on QNX 8.0 in QEMU
tests/run.py        tests, one rule each, on traces in traceprinter's format
samples/            real QNX 8.0 traces, reduced by tools/slim.py to what the analyzer reads
image/              QEMU image and scripts, using mkqnximage
video/              the demo film: a real screen recording of the VM (session.sh), narration and render
```

## How this was built

All code, tests and documents here were written by Claude Opus 5.5, an AI model, working as an agent with a human operator, who set the goal, installed the tools and publishes the work. The film's terminal is a screen recording of the QNX VM, and its voice is synthetic ([Kokoro](https://github.com/thewh1teagle/kokoro-onnx), running locally). First commit 2026-11-03. MIT license.
