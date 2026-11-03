#!/bin/sh
# Runs the plant until it hangs or the limit passes, several times, and prints one line per run.
# usage: soak.sh <runs> <limit_s> <sensor args> -- <logger args>
# A hung sensor cannot finish exiting while control holds its message, so this waits for its report instead.
runs=$1; limit=$2; shift 2
sensor_args=""; while [ $# -gt 0 ] && [ "$1" != "--" ]; do sensor_args="$sensor_args $1"; shift; done; shift
cd "$(dirname "$0")"
out=/dev/shmem/soak.out
i=0
while [ $i -lt "$runs" ]; do
  i=$((i+1))
  : > $out
  ./logger "$@" >/dev/null 2>&1 & l=$!
  ./control >/dev/null 2>&1 & c=$!
  ./sensor -t "$limit" $sensor_args > $out 2>&1 & s=$!
  while [ ! -s $out ]; do sleep 1; done
  cat $out
  kill -9 $l $c $s 2>/dev/null
  wait $l $c $s 2>/dev/null
done
