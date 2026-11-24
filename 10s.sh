
#!/usr/bin/env bash
set -euo pipefail

MODULE="irqdriver"
MODE="${1:-workqueue}"
DURATION=10
INTERVAL=10

cleanup() {
    if [[ "${LOADED:-0}" == "1" ]]; then
        sudo rmmod "$MODULE" || true
        echo "Модуль выгружен."
    fi
}

trap cleanup EXIT INT TERM

echo "Сборка модуля..."
make

echo "Загрузка модуля:"
echo "  Mode: $MODE"
echo "  Interval: ${INTERVAL} ms"
echo "  Duration: ${DURATION} sec"

sudo insmod "./${MODULE}.ko" interval_ms="$INTERVAL" mode="$MODE"
LOADED=1

echo
echo "Модуль работает..."
sleep "$DURATION"

echo
echo "========== STATISTICS =========="
sudo cat "/proc/$MODULE"
echo "================================"
