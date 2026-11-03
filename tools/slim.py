#!/usr/bin/env python3
"""Keeps only what sendgraph reads from traceprinter output, so a sample trace stays small.
usage: tools/slim.py < full.txt > slim.txt"""
import re
import sys

KEEP = re.compile(r"(THREAD +:THRUNNING|PROCESS +:|COMM +:(SND_MESSAGE|REC_MESSAGE|SND_PULSE)\b|KER_CALL +:(MSG_RECEIVEV|MSG_SENDV|MSG_REPLYV|MSG_ERROR))")
keeping = True
for line in sys.stdin:
    if line.startswith("t:"):
        keeping = bool(KEEP.search(line))
    elif not line.startswith(" ") or "TRACE_CYCLES" in line:
        keeping = "TRACE_" in line
    if keeping:
        sys.stdout.write(line)
