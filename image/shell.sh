#!/bin/bash
# A shell on the running target; QEMU powers off when you leave it.
# usage: ./dev.sh image/vm.sh image/shell.sh
exec $SSH -t root@127.0.0.1
