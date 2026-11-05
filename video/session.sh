#!/bin/bash
# Records a real shell on the QNX VM: xterm under Xvfb, screen captured by ffmpeg, nothing replayed.
# usage: SG_IMAGE=sendgraph-film ./dev.sh image/vm.sh video/session.sh
set -e
OUT=$PWD/video/out; mkdir -p "$OUT"; cd "$OUT"
rm -f session.log session.times session.mp4
FIFO=$(mktemp -u); mkfifo "$FIFO"
Xvfb :9 -screen 0 1240x1000x24 -nolisten tcp >/dev/null 2>&1 & XV=$!
export DISPLAY=:9; sleep 1
xterm -geometry 94x38+0+0 -fa "DejaVu Sans Mono" -fs 16 -b 8 -bw 0 -bg '#101814' -fg '#e6efe9' -cr '#e6efe9' \
  -e bash -c "cat $FIFO | $SSH -tt root@127.0.0.1 | tee session.log" & XT=$!
exec 3>"$FIFO"
trap 'kill $FF $XT $XV 2>/dev/null; rm -f "$FIFO"' EXIT
prompts() { grep -a -c -E '(^|\[H)qnx# ' session.log || true; }
send() { printf '%s\n' "$1" >&3; }
# Types a command at human speed and waits for the shell to come back.
cmd() {
  local n; n=$(prompts); sleep 0.6
  for ((i = 0; i < ${#1}; i++)); do printf '%s' "${1:i:1}" >&3; sleep 0.035; done
  sleep 0.3; send ""
  until [ "$(prompts)" -gt "$n" ]; do sleep 0.2; done
  sleep "${2:-2}"
}
mark() { echo "$1 $(awk -v a="$START" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}')" >> session.times; }
send "cd /data/sendgraph; PS1='qnx# '; printf '\033[2J\033[H'"
until [ "$(prompts)" -gt 0 ]; do sleep 0.2; done; sleep 0.5
ffmpeg -loglevel error -f x11grab -draw_mouse 0 -framerate 15 -video_size 1240x1000 -i :9 -c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv444p session.mp4 & FF=$!
START=$(date +%s.%N); sleep 1

mark uname;  cmd 'uname -a' 3
mark plain;  cmd './run.sh plain; echo "exit code $?"' 6
mark fixes;  cmd './run.sh thread; echo "exit code $?"' 2
             cmd './run.sh pulse; echo "exit code $?"' 4
mark start;  cmd './logger -r 2000 >/dev/null 2>&1 & ./control >/dev/null 2>&1 & ./sensor -p 1000 &' 1
mark hang;   n=0; until grep -a -q 'HANG after' session.log || [ $n -gt 600 ]; do sleep 0.2; n=$((n+1)); done; sleep 2
send ""; sleep 0.5
mark pidin;  cmd "pidin | grep -E 'pid tid|logger|control|sensor'" 5
mark snap;   cmd 'pidin | ./sendgraph --snapshot; echo "exit code $?"' 6
mark end
send 'slay -f logger control sensor; exit'; sleep 1
kill -INT $FF; wait $FF 2>/dev/null || true
cat session.times
cd "$OUT/../.."
vm_get /dev/shmem/plain.kev video/out/plain.kev && traceprinter -f video/out/plain.kev | build/host/sendgraph --quiet --html video/out/report-plain.html || true
