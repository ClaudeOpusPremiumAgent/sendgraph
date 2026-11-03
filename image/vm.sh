#!/bin/bash
# Boots the QNX image in QEMU with user-mode networking (no bridge, no root), runs a command, powers off.
#   image/vm.sh <command...>     the command runs on the host side; use vm_ssh and vm_get inside it
# Exposes: SSH to the target on localhost:${SSH_PORT:-2222}.
set -e
cd "$(dirname "$0")/out"
SSH_PORT="${SSH_PORT:-2222}"
KVM=""; [ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"
qemu-system-x86_64 $KVM -smp 2 -m 1G $(cat output/build/qemu_options) \
  -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 -snapshot \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 -device virtio-net-pci,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -display none -serial file:console.log -monitor none &
QEMU=$!
trap 'kill $QEMU 2>/dev/null; wait $QEMU 2>/dev/null || true' EXIT
SSH="ssh -q -i $HOME/.ssh/sendgraph -p $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5"
vm_ssh() { $SSH root@127.0.0.1 "$@"; }
vm_get() { scp -q -O -i "$HOME/.ssh/sendgraph" -P "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "root@127.0.0.1:$1" "$2"; }
vm_put() { scp -q -O -i "$HOME/.ssh/sendgraph" -P "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$1" "root@127.0.0.1:$2"; }
export -f vm_ssh vm_get vm_put; export SSH SSH_PORT
for i in $(seq 1 90); do vm_ssh true 2>/dev/null && break; sleep 1; [ "$i" = 90 ] && { echo "the target did not answer on ssh"; tail -20 console.log; exit 1; }; done
cd ../..
"$@"
