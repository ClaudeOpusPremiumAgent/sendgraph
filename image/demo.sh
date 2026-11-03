#!/bin/bash
# Boots QNX in QEMU and runs the whole story. Writes reports and raw traces to image/out/.
# usage: image/vm.sh image/demo.sh
set -e
cd "$(dirname "$0")/.."
for v in plain thread pulse; do
  echo "=== $v build of the plant, traced for 3 healthy seconds"
  vm_ssh "/data/sendgraph/run.sh $v" || echo "(exit code $?)"
  # A plant that has already hung leaves no trace to copy; sgcapture has reported it by snapshot.
  if vm_get "/dev/shmem/$v.kev" "image/out/$v.kev" 2>/dev/null; then
    traceprinter -f "image/out/$v.kev" | build/host/sendgraph --quiet --html "image/out/report-$v.html" || true
  fi
done
echo "=== the plain build at stress rates, after it has hung"
vm_ssh 'cd /data/sendgraph; ./logger -r 2000 >/dev/null 2>&1 & ./control >/dev/null 2>&1 & ./sensor -p 1000 >/dev/null 2>&1 & sleep 5; ./sgcapture 1; echo "(exit code $?)"; slay -f logger control sensor; true'
echo "Reports: image/out/report-plain.html, report-thread.html, report-pulse.html"
