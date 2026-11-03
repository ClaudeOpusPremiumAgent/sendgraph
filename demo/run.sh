#!/bin/sh
# Runs on the target. Starts one build of the plant, traces it, stops it.
# usage: run.sh <plain|thread|pulse> [sendgraph options]
variant=$1; [ $# -gt 0 ] && shift
cd "$(dirname "$0")"
case "$variant" in
  plain)  sensor_args="";   logger_args="" ;;
  thread) sensor_args="-s"; logger_args="-n sg_sensor_status" ;;
  pulse)  sensor_args="";   logger_args="-a" ;;
  *) echo "usage: run.sh <plain|thread|pulse>" >&2; exit 2 ;;
esac
./logger $logger_args >/dev/null 2>&1 & l=$!
./control >/dev/null 2>&1 & c=$!
./sensor $sensor_args >/dev/shmem/sensor.out 2>&1 & s=$!
sleep 1
SG_KEV=/dev/shmem/$variant.kev ./sgcapture 2 "$@"
rc=$?
kill -9 $l $c $s 2>/dev/null
exit $rc
