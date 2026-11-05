#!/bin/sh
# Runs a command in the build image. Only this directory, the QNX SDP (read-only) and the QNX licence are mounted.
# Usage: ./dev.sh <command>
set -e
cd "$(dirname "$0")"
QNX_ROOT="${QNX_ROOT:-$HOME/qnx800}"
mkdir -p .home
docker image inspect sendgraph-dev >/dev/null 2>&1 || docker build -q -t sendgraph-dev -f Dockerfile.dev . >/dev/null
docker network inspect sendgraph-net >/dev/null 2>&1 || docker network create --opt com.docker.network.driver.mtu=1280 sendgraph-net >/dev/null
KVM=""; [ -e /dev/kvm ] && KVM="--device /dev/kvm --group-add $(stat -c %g /dev/kvm)"
TTY=""; [ -t 0 ] && TTY="-t"
exec docker run --rm -i $TTY --network sendgraph-net $KVM --user "$(id -u):$(id -g)" \
  -e HOME=/app/.home -e QNX_HOST=/qnx/host/linux/x86_64 -e QNX_TARGET=/qnx/target/qnx \
  -e MAKEFLAGS=-Rr \
  -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro -v "$PWD":/app -v "$QNX_ROOT":/qnx:ro -v "$HOME/.qnx":/app/.home/.qnx "${SG_IMAGE:-sendgraph-dev}" \
  sh -c 'PATH=/qnx/host/linux/x86_64/usr/bin:/qnx/host/common/bin:$PATH; export PATH; exec "$@"' sh "$@"
