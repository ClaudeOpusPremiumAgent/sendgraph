#!/bin/bash
# Boots QNX in QEMU and runs the whole story. Writes reports and raw traces to image/out/.
# usage: image/vm.sh image/demo.sh
set -e
cd "$(dirname "$0")/.."
for v in plain thread pulse; do
  echo "=== $v build of the plant, traced for 3 healthy seconds"
  # The plain build can hang before its trace ends. That is the bug, but the point here is the healthy trace.
  for attempt in 1 2 3 4 5 6; do
    rc=0; vm_ssh "/data/sendgraph/run.sh $v" > "image/out/$v.out" || rc=$?
    grep -q 'DEADLOCK NOW' "image/out/$v.out" || break
    echo "(attempt $attempt hung before its trace ended, which is this same bug: trying again)"
  done
  cat "image/out/$v.out"; [ "$rc" = 0 ] || echo "(exit code $rc)"
  # A plant that hung leaves no useful trace; sgcapture has reported it by snapshot.
  if vm_get "/dev/shmem/$v.kev" "image/out/$v.kev" 2>/dev/null; then
    traceprinter -f "image/out/$v.kev" | build/host/sendgraph --quiet --html "image/out/report-$v.html" || true
  fi
done
echo "=== the plain build at stress rates, after it has hung"
vm_ssh 'cd /data/sendgraph; ./logger -r 2000 >/dev/null 2>&1 & ./control >/dev/null 2>&1 & ./sensor -p 1000 >/dev/null 2>&1 & sleep 5; ./sgcapture 1; echo "(exit code $?)"; slay -f logger control sensor; true'
echo "Reports, for each build that reached a trace: image/out/report-*.html"
