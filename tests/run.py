#!/usr/bin/env python3
"""Runs the analyzer on small traces written in traceprinter's format. Each test states one rule.
usage: tests/run.py [path to sendgraph]"""
import json, os, shutil, subprocess, sys, tempfile

TOOL = sys.argv[1] if len(sys.argv) > 1 else "build/host/sendgraph"


class Trace:
    """Builds trace text event by event. The clock moves on by itself."""

    def __init__(self):
        self.t = 0x1000
        self.lines = ["TRACEPRINTER version 1.02", "  TRACE_CYCLES_PER_SEC:: 1000000", " -- KERNEL EVENTS -- "]
        self.rcvid = 0

    def ev(self, cpu, cls, name, rest="", extra=()):
        self.t += 10
        self.lines.append(f"t:{self.t:#x} CPU:{cpu:02d} {cls:<16}:{name:<19}{rest}")
        self.lines += [f"                      {e}" for e in extra]

    def name(self, pid, name):
        self.ev(0, "PROCESS", "PROCCREATE_NAME", "", [f"ppid:1", f"pid:{pid}", f"name:./{name}"])

    def run(self, pid, tid, cpu=0):
        self.ev(cpu, "THREAD", "THRUNNING", f"pid:{pid} tid:{tid} priority:10 policy:2")

    def receive(self, pid, tid, chid, cpu=0):
        self.run(pid, tid, cpu)
        self.ev(cpu, "KER_CALL", "MSG_RECEIVEV/14", "", [f"chid:{chid:#010x}", "rparts:1"])

    def send(self, pid, tid, to_pid, coid=5, mtype=0x200, cpu=0):
        """pid/tid calls MsgSend. Returns the rcvid the receiver will see."""
        self.run(pid, tid, cpu)
        self.ev(cpu, "KER_CALL", "MSG_SENDV/11", "", [f"coid:{coid:#010x}", "sparts:1", "rparts:0", f'msg[0]:"" ({mtype:#010x} 0x00000000 0x00000000)'])
        self.rcvid += 1
        self.ev(cpu, "COMM", "SND_MESSAGE", f"rcvid:{self.rcvid:#010x} pid:{to_pid}")
        return self.rcvid

    def got(self, pid, tid, rcvid, cpu=0):
        self.run(pid, tid, cpu)
        self.ev(cpu, "COMM", "REC_MESSAGE", f"rcvid:{rcvid:#010x} pid:{pid}")

    def reply(self, pid, tid, rcvid, cpu=0):
        self.run(pid, tid, cpu)
        self.ev(cpu, "KER_CALL", "MSG_REPLYV/15", "", [f"rcvid:{rcvid:#010x}", "sparts:0", "status:0x0"])

    def pulse(self, pid, tid, to_pid, cpu=0):
        self.run(pid, tid, cpu)
        self.ev(cpu, "COMM", "SND_PULSE", f"scoid:0x00010001 pid:{to_pid}")

    def call(self, client, server, chid, coid=5, mtype=0x200):
        """A whole exchange: client sends, server takes it from chid and answers."""
        r = self.send(*client, server[0], coid=coid, mtype=mtype)
        self.receive(*server, chid)
        self.got(*server, r)
        self.reply(*server, r)

    def text(self):
        return "\n".join(self.lines) + "\n"


def analyze(trace, *flags):
    with tempfile.TemporaryDirectory() as d:
        path, out = os.path.join(d, "t.txt"), os.path.join(d, "r.json")
        with open(path, "w") as f:
            f.write(trace if isinstance(trace, str) else trace.text())
        p = subprocess.run([TOOL, path, "--json", out, "--html", os.path.join(d, "r.html"), "--dot", os.path.join(d, "r.dot"), *flags], capture_output=True, text=True)
        result = json.load(open(out)) if os.path.exists(out) else None
        return p.returncode, p.stdout + p.stderr, result


def serves(t, pid, tid, chid):
    """The thread proves it serves chid: a harmless message from a third party."""
    t.call((9000 + pid, 1), (pid, tid), chid)


def test_a_loop_of_two_single_threaded_services_is_a_cycle():
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    serves(t, 100, 1, 1); serves(t, 200, 1, 1)
    # Neither exchange overlaps the other, so this trace did not hang. It is still a deadlock in waiting.
    t.call((100, 1), (200, 1), 1)
    t.call((200, 1), (100, 1), 1)
    code, out, r = analyze(t)
    assert code == 1, out
    assert len(r["cycles"]) == 1 and r["cycles"][0]["threads_needed"] == 1, r
    assert "a -> b -> a" in out or "b -> a -> b" in out, out


def test_a_chain_with_no_loop_is_clean_and_gives_a_hierarchy():
    t = Trace()
    t.name(100, "app"); t.name(200, "mid"); t.name(300, "drv")
    serves(t, 100, 1, 1)
    r1 = t.send(100, 1, 200); t.receive(200, 1, 1); t.got(200, 1, r1)
    t.call((200, 1), (300, 1), 1)
    t.reply(200, 1, r1)
    code, out, r = analyze(t)
    assert code == 0, out
    assert r["cycles"] == [], r
    assert out.index("level 2: app") < out.index("level 1: mid") < out.index("level 0: drv"), out


def test_a_separate_thread_on_a_separate_channel_breaks_the_cycle():
    # The processes form a loop, but the thread that is blocked is not the one that has to answer.
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    serves(t, 100, 1, 1); serves(t, 200, 1, 1)
    t.call((100, 1), (200, 1), 1)       # a's main thread (serves a#1) waits for b
    t.call((200, 1), (100, 2), 2)       # b waits only for a's status thread on a#2
    code, out, r = analyze(t)
    assert code == 0, out


def test_a_status_thread_that_sends_back_closes_the_cycle_again():
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    serves(t, 100, 1, 1); serves(t, 100, 2, 2); serves(t, 200, 1, 1)
    t.call((100, 1), (200, 1), 1)
    t.call((200, 1), (100, 2), 2)
    t.call((100, 2), (200, 1), 1)       # now a#2 waits for b as well
    code, out, r = analyze(t)
    assert code == 1, out
    assert r["cycles"][0]["threads_needed"] == 1


def test_thread_pools_raise_the_number_of_requests_needed():
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    for tid in (1, 2, 3):
        serves(t, 100, tid, 1)
    for tid in (1, 2):
        serves(t, 200, tid, 1)
    t.call((100, 1), (200, 1), 1)
    t.call((200, 2), (100, 3), 1)
    code, out, r = analyze(t)
    assert code == 0, out
    assert r["cycles"][0]["threads_needed"] == 2, r
    assert "2 requests" in out and "only under load" in out, out
    code, out, r = analyze(t, "--strict")
    assert code == 1, out


def test_a_thread_sending_to_its_own_channel_is_a_cycle():
    t = Trace()
    t.name(100, "a")
    serves(t, 100, 1, 1)
    t.send(100, 1, 100)
    code, out, r = analyze(t)
    assert code == 1, out


def test_a_worker_that_answers_for_the_receiver_carries_the_wait():
    # The receiver hands the message to a worker, stays free, and only the client is stuck.
    t = Trace()
    t.name(100, "front"); t.name(200, "back")
    serves(t, 100, 1, 1); serves(t, 200, 1, 1)
    r1 = t.send(200, 1, 100, coid=7)     # back asks front
    t.receive(100, 1, 1); t.got(100, 1, r1)
    t.call((100, 2), (200, 1), 1, coid=8)  # front's worker asks back while the first request is open
    t.reply(100, 2, r1)                  # the worker answers it
    code, out, r = analyze(t)
    assert code == 1, out


def test_pulses_never_make_a_wait():
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    serves(t, 100, 1, 1); serves(t, 200, 1, 1)
    t.call((100, 1), (200, 1), 1)
    t.pulse(200, 1, 100)
    code, out, r = analyze(t)
    assert code == 0, out
    assert "b -> a (1)" in out, out


def test_a_thread_blocked_in_receive_when_the_trace_began_is_still_placed():
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    r1 = t.send(100, 1, 200)
    t.got(200, 1, r1)                    # no MsgReceive seen yet
    t.reply(200, 1, r1)
    t.receive(200, 1, 3)                 # only now does it show chid 3
    t.call((200, 1), (100, 1), 1, coid=6)
    t.receive(100, 1, 1)
    serves(t, 100, 1, 1)
    code, out, r = analyze(t)
    assert code == 1, out


def test_a_send_that_was_never_received_uses_the_known_connection():
    # A real hang leaves a send queued forever. The connection tells which channel it was for.
    t = Trace()
    t.name(100, "a"); t.name(200, "b"); t.name(300, "c")
    serves(t, 200, 1, 4); serves(t, 100, 1, 1)
    t.call((300, 1), (200, 1), 4, coid=9)
    t.call((100, 1), (200, 1), 4, coid=5)   # a learns coid 5 means b#4
    t.send(100, 1, 200, coid=5)             # queued, never received
    t.call((200, 1), (100, 1), 1, coid=6)
    code, out, r = analyze(t)
    assert code == 1, out


def test_input_that_is_not_a_trace_is_refused():
    code, out, _ = analyze("hello\nworld\n")
    assert code == 2 and "no trace events" in out, out


def test_a_trace_without_communication_events_is_refused():
    t = Trace()
    t.run(1, 1)
    code, out, _ = analyze(t)
    assert code == 2 and "tracelogger -w" in out, out


def test_known_message_types_are_named():
    t = Trace()
    t.name(100, "a"); t.name(200, "b")
    serves(t, 100, 1, 1)
    t.call((100, 1), (200, 1), 1, mtype=0x101)
    code, out, r = analyze(t)
    assert code == 0 and r["edges"][0]["message"] == "_IO_READ", r


HUNG = """     pid tid name               prio STATE       Blocked
  100001   1 ./sensor            10r REPLY       100002
  100001   2 ./sensor            10r NANOSLEEP
  100002   1 ./control           10r SEND        100003
  100003   1 ./logger            10r SEND        100001
       1   1 /proc/boot/procnto  0f  READY
"""


def snapshot(text):
    return subprocess.run([TOOL, "--snapshot"], input=text, capture_output=True, text=True)


def test_a_snapshot_shows_a_deadlock_that_has_already_happened():
    p = snapshot(HUNG)
    assert p.returncode == 1 and "DEADLOCK NOW" in p.stdout, p.stdout + p.stderr
    assert "sensor (pid 100001, thread 1 REPLY) waits for control" in p.stdout, p.stdout


def test_a_snapshot_with_a_free_thread_is_not_a_deadlock():
    p = snapshot(HUNG + "  100002   2 ./control           10r RECEIVE     1\n")
    assert p.returncode == 0, p.stdout


def test_a_process_waiting_on_a_loop_is_not_named_as_part_of_it():
    p = snapshot(HUNG + "  100009   1 ./client            10r REPLY       100001\n")
    assert p.returncode == 1 and "client" not in p.stdout, p.stdout


def test_a_snapshot_of_something_else_is_refused():
    p = snapshot("hello\n")
    assert p.returncode == 2, p.stdout


def capture(*pidin_outputs):
    """Runs sgcapture with stand-ins for the QNX tools. Each pidin call prints the next output, then the last again."""
    trace = Trace()
    trace.name(1, "a")
    trace.name(2, "b")
    trace.receive(2, 1, 1)
    trace.call((1, 1), (2, 1), 1)
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "sgcapture"), d)
        os.symlink(os.path.abspath(TOOL), f"{d}/sendgraph")
        for i, text in enumerate(pidin_outputs):
            open(f"{d}/pidin.{i}", "w").write(text)
        open(f"{d}/trace.txt", "w").write(trace.text())
        scripts = {
            "pidin": f'n=$(cat {d}/calls 2>/dev/null || echo 0); echo $((n+1)) > {d}/calls\n'
                     f'[ -f {d}/pidin.$n ] || n={len(pidin_outputs) - 1}; cat {d}/pidin.$n\n',
            "tracelogger": f"touch {d}/traced\n",
            "traceprinter": f"cat {d}/trace.txt\n",
        }
        for name, body in scripts.items():
            open(f"{d}/{name}", "w").write("#!/bin/sh\n" + body)
            os.chmod(f"{d}/{name}", 0o755)
        env = dict(os.environ, PATH=f"{d}:{os.environ['PATH']}", SG_KEV=f"{d}/x.kev")
        p = subprocess.run([f"{d}/sgcapture", "1"], capture_output=True, text=True, env=env)
        return p, os.path.exists(f"{d}/traced")


HEALTHY = HUNG + "  100002   2 ./control           10r RECEIVE     1\n"


def test_sgcapture_stops_on_a_system_already_hung_and_does_not_trace_it():
    p, traced = capture(HUNG)
    assert p.returncode == 1, p.stdout + p.stderr
    assert p.stdout.count("DEADLOCK NOW") == 1 and "No wait cycles" not in p.stdout, p.stdout
    assert not traced


def test_sgcapture_catches_a_system_that_hangs_after_its_first_check():
    p, traced = capture(HEALTHY, HUNG)
    assert p.returncode == 1 and traced, p.stdout + p.stderr
    assert p.stdout.count("DEADLOCK NOW") == 1 and "No wait cycles" not in p.stdout, p.stdout


def test_sgcapture_reports_the_trace_of_a_system_still_running():
    p, traced = capture(HEALTHY)
    assert p.returncode == 0 and traced and "No wait cycles" in p.stdout, p.stdout + p.stderr


if __name__ == "__main__":
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("ok  ", name)
        except AssertionError as e:
            failed += 1
            print("FAIL", name, "\n     ", str(e)[:600].replace("\n", "\n      "))
    print(f"{len(tests) - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
