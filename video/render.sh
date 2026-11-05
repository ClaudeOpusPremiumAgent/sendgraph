#!/bin/bash
# Builds the demo film from a real recording (see session.sh), the narration and film.html.
#   video/render.sh voice              speak narration.json, once
#   video/render.sh preview 3 22 41    stills at those seconds
#   video/render.sh film               the whole film: video/out/sendgraph-demo.mp4
set -e
cd "$(dirname "$0")/.."
OUT=video/out
if [ "$1" = voice ]; then
  docker run --rm --network sendgraph-net --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD/video":/video python:3.12-slim sh -c \
    'pip install --quiet --no-cache-dir --target /tmp/py kokoro-onnx soundfile 2>&1 | grep -v -E "WARNING|notice" ; PYTHONPATH=/tmp/py python /video/tts.py'
  exit
fi
mkdir -p $OUT/session $OUT/frames
[ -f $OUT/session/00001.jpg ] || ffmpeg -y -loglevel error -i $OUT/session.mp4 -qscale:v 2 $OUT/session/%05d.jpg
docker run --rm --network sendgraph-net --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/site:ro -v "$PWD/$OUT":/out \
  -v "$PWD/video/render.mjs":/mnt/render.mjs:ro mcr.microsoft.com/playwright:v1.56.1-noble sh -c '
    mkdir -p /tmp/app && cd /tmp/app && cp /mnt/render.mjs . && npm init -y >/dev/null \
    && npm i --silent --no-audit --no-fund --ignore-scripts playwright@1.56.1 >/dev/null 2>&1
    (cd /site && python3 -m http.server 8000 >/dev/null 2>&1 &)
    sleep 1
    node render.mjs "$@"' sh "$@"
if [ "$1" = film ]; then
  # Places each spoken line at the moment film.html reserved for it.
  args=(); mix=""; i=0
  for line in $(python3 -c "import json;[print(f'{k}={s}') for k,s in json.load(open('$OUT/timeline.json'))]"); do
    args+=(-i "$OUT/voice/${line%%=*}.wav")
    mix+="[$i:a]adelay=$(python3 -c "print(int(${line##*=}*1000))"):all=1[a$i];"; i=$((i+1))
  done
  for ((j = 0; j < i; j++)); do mix+="[a$j]"; done
  ffmpeg -y -loglevel error "${args[@]}" -filter_complex "${mix}amix=inputs=$i:normalize=0[out]" -map "[out]" -ar 48000 $OUT/voice.wav
  ffmpeg -y -loglevel error -framerate 30 -i $OUT/frames/%05d.jpg -i $OUT/voice.wav -vf scale=out_range=tv,format=yuv420p \
    -c:v libx264 -preset slow -crf 18 -c:a aac -b:a 160k -shortest -movflags +faststart $OUT/sendgraph-demo.mp4
  rm -rf $OUT/frames
  ls -la $OUT/sendgraph-demo.mp4
  ffprobe -v error -show_entries format=duration -of csv=p=0 $OUT/sendgraph-demo.mp4
fi
