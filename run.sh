#!/bin/bash
set -euo pipefail
MODE="${1:-workqueue}"
INTERVAL="${2:-10}"
case "$MODE" in tasklet|workqueue|kthread) ;; *) echo "Usage: $0 {tasklet|workqueue|kthread} [interval_ms]"; exit 1;; esac
sudo insmod ./irqdriver.ko mode="$MODE" interval_ms="$INTERVAL"
echo "Loaded mode=$MODE interval=${INTERVAL}ms. Ctrl-C to stop monitor."
trap 'sudo rmmod irqdriver 2>/dev/null || true' EXIT
watch -n 1 cat /proc/irqdriver
