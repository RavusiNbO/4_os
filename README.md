# ЛР №4, вариант 5 — hrtimer (Debian VM)

## Состав
- `irqdriver.c` — модуль ядра.
- `Makefile` — сборка для текущего ядра.

## Важное замечание по методичке
Для hrtimer callback является верхней половиной: он вызывается при истечении таймера,
фиксирует время и планирует нижнюю половину. Вариант 5 не имеет аппаратного IRQ
устройства, поэтому `request_threaded_irq()` здесь неприменим напрямую.
Третий сравниваемый механизм в этом коде — отдельный kernel thread (`kthread`),
который пробуждается callback таймера. Это потоковая нижняя половина, но не
буквальный threaded IRQ. Если преподаватель требует именно threaded IRQ,
нужно согласовать замену либо выбрать источник реального IRQ.

## Установка зависимостей (Debian)
```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

Если пакет заголовков не найден:
```bash
uname -r
ls -l /lib/modules/$(uname -r)/build
```
В Debian VM может потребоваться установить пакет `linux-headers-arm64` или
`linux-headers-amd64` для установленного ядра и перезагрузиться в ядро,
для которого доступны headers.

## Сборка
```bash
make
```

## Запуск — каждый режим отдельно
Tasklet:
```bash
sudo insmod irqdriver.ko mode=tasklet interval_ms=10
cat /proc/irqdriver
sudo rmmod irqdriver
```

Workqueue:
```bash
sudo insmod irqdriver.ko mode=workqueue interval_ms=10
cat /proc/irqdriver
sudo rmmod irqdriver
```

Kthread:
```bash
sudo insmod irqdriver.ko mode=kthread interval_ms=10
cat /proc/irqdriver
sudo rmmod irqdriver
```

## Изменение периода и сброс статистики
```bash
echo 1 | sudo tee /proc/irqdriver
echo 10 | sudo tee /proc/irqdriver
echo 100 | sudo tee /proc/irqdriver
echo reset | sudo tee /proc/irqdriver
```
Допустимый период: 1..60000 мс.

## Наблюдение
```bash
watch -n 1 cat /proc/irqdriver
sudo dmesg -w
```

Поля:
- `top_half_events` — число callback таймера;
- `bottom_half_events` — число обработанных нижней половиной событий;
- `queue_overruns` — переполнение внутренней очереди временных меток;
- `avg_jitter_ns`, `max_jitter_ns` — отклонение фактического запуска callback от срока таймера;
- `avg_bottom_latency_ns`, `max_bottom_latency_ns` — задержка от callback до начала обработки нижней половиной;
- `pending_events` — события, ожидающие нижней половины.

## Эксперимент
Для каждого режима отдельно запустить модуль с интервалами 1, 10 и 100 мс.
Снять значения через 5–10 секунд, записать результаты в таблицу отчёта.
Сравнивать режимы нужно при одинаковом периоде и одинаковой нагрузке VM.

Пример таблицы:
| Режим | Период, мс | Top events | Bottom events | Avg jitter, нс | Max jitter, нс | Avg latency, нс | Max latency, нс |
|---|---:|---:|---:|---:|---:|---:|---:|
| tasklet | 1 | | | | | | |
| workqueue | 1 | | | | | | |
| kthread | 1 | | | | | | |

## Про `msleep(100)`
`msleep()` нельзя вызывать в hrtimer callback или tasklet: эти контексты не допускают
сна. Workqueue выполняется в контексте потока ядра, поэтому там сон допустим.
Для демонстрации можно добавить `msleep(100)` только в `work_bottom()`, затем
сравнить рост очереди и задержек. Не добавляйте `msleep()` в tasklet или callback:
это приводит к предупреждениям/ошибкам atomic-sleep и может нарушить работу VM.
