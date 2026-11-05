# The dev image plus what it takes to record a real terminal session on the QNX VM.
FROM sendgraph-dev
RUN apt-get update && apt-get install -y --no-install-recommends xvfb xterm ffmpeg fonts-dejavu-core \
  && rm -rf /var/lib/apt/lists/*
