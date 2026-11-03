#!/bin/bash
# Builds a QEMU x86_64 image of QNX 8.0 with mkqnximage and adds the demo programs to its data partition.
# Run it through ./dev.sh, or on a host where qnxsdp-env.sh has been sourced.
set -e
cd "$(dirname "$0")"
mkdir -p out && cd out
[ -f ~/.ssh/sendgraph ] || { mkdir -p ~/.ssh; ssh-keygen -q -t ed25519 -N '' -f ~/.ssh/sendgraph; }
mkdir -p local/snippets
# OpenSSH in SDP 8.0 refuses to start without sshd-auth, and mkqnximage from the same SDP leaves it out.
echo '[type=link] /usr/libexec/sshd-auth=/system/bin/sshd-auth' > local/snippets/ifs_files.custom
# The stock image has tracelogger but not traceprinter, and sendgraph can run on the target too.
cat > local/snippets/system_files.custom <<'EOT'
bin/sshd-auth=usr/libexec/sshd-auth
bin/traceprinter=usr/bin/traceprinter
lib/libtraceparser.so.1=usr/lib/libtraceparser.so.1
EOT
if [ -d ../../build/qnx ]; then
  # host-path=target-path lines for mkifs; the data partition appears as /data on the target.
  : > local/snippets/data_files.custom
  for f in ../../build/qnx/*; do echo "[perms=0755] sendgraph/$(basename "$f")=$PWD/$f" >> local/snippets/data_files.custom; done
fi
mkqnximage --type=qemu --arch=x86_64 --cpu=2 --ram=1G --data-size=256 --hostname=sendgraph \
  --ip=dhcp --ssh-ident="$HOME/.ssh/sendgraph.pub" --noprompt --force --build
ls -la output/ifs.bin output/disk-qemu.vmdk
