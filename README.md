# M5Stack TAB5 AI Assistant

Прошивка для **M5Stack TAB5** — карманный ИИ-помощник для бытовых задач.
Пишешь вопрос на клавиатуре — он ищет в интернете, отвечает и **сохраняет
найденное в собственный архив markdown-файлов на SD-карте**, чтобы второй раз
не гуглить.

> Очиститель воздуха просит заменить фильтр, а как сбросить ошибку — забыл.
> Ассистент найдёт инструкцию, покажет по шагам и сохранит
> `notes/home/air-purifier-filter-reset.md` со ссылкой на источник.
> Через месяц: «как сбросить ошибку очистителя» → ответ из собственного архива,
> интернет не нужен.

---

## Железо

| | |
|---|---|
| Плата | M5Stack TAB5 Kit (**K145**): ESP32-P4 rev v1.3 + ESP32-C6 (Wi-Fi 6) |
| Экран | 5″ IPS 1280×720, MIPI-DSI, **ST7121**, ёмкостный тач |
| Память | 16 МБ flash, 32 МБ PSRAM (200 МГц), microSD |
| Питание | NP-F550 2S 2000 мА·ч, INA226 (напряжение/ток), IP2326 (заряд) |
| Ввод | Клавиатура **A164** (матрица 5×14, I2C 0x6D на G0/G1) |
| Прочее | ES8388 + ES7210 (аудио/AEC), SC2356 (камера), BMI270 (IMU), RX8130 (RTC) |

Подробная распиновка и грабли — в [`docs/BOARD_AI_RESEARCH.md`](docs/BOARD_AI_RESEARCH.md),
факты с живой платы — в [`docs/HARDWARE_VERIFICATION.md`](docs/HARDWARE_VERIFICATION.md).

---

## Что уже работает

Текущая версия — **v0.1.0 «Bring-up»**. Скелет проекта собран и проверен на
живой плате: дисплей 1280×720 с кириллицей, клавиатура A164 с раскладкой
латиница/ЙЦУКЕН, SD-карта в 4-bit с записью, батарея через INA226, Wi-Fi через
ESP32-C6.

Диалога с моделью пока нет — это следующий этап. Полный план и статус — в
[`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md), измеренные значения — в
[`docs/HARDWARE_VERIFICATION.md`](docs/HARDWARE_VERIFICATION.md).

---

## Стек и зависимости

| Компонент | Версия | Зачем |
|---|---|---|
| [PlatformIO Core](https://platformio.org/) | 6.2.0 | Сборка и прошивка |
| [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) | **55.03.313** | Платформа с поддержкой ESP32-P4 |
| Arduino ESP32 core | 3.3.13 | Внутри платформы |
| ESP-IDF | 5.5.6 | Внутри платформы |
| [M5Unified](https://github.com/m5stack/M5Unified) | **0.3.0** | Дисплей, тач, батарея, RTC, IO-экспандеры |
| [M5GFX](https://github.com/m5stack/M5GFX) | **0.3.0** | Графика и шрифты |
| [M5UnitUnified](https://github.com/m5stack/M5UnitUnified) | 0.6.5 | Фреймворк юнитов |
| [M5Unit-KEYBOARD](https://github.com/m5stack/M5Unit-KEYBOARD) | 0.1.0 | Клавиатура A164 |
| M5Utility / M5HAL | 0.3.1 / 0.1.4 | Зависимости M5UnitUnified |

Почему именно эти версии, что отклонили и какие ограничения это накладывает —
в [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md).

### Почему не ESP-IDF и не LVGL

- **ESP-IDF** дал бы больше контроля над памятью, но батарею, RTC и питание
  пришлось бы писать самим — M5Unified отдаёт это бесплатно.
- **LVGL 9.2 не содержит ни одного готового шрифта с кириллицей** (проверено
  разбором cmap: Montserrat, DejaVu, SimSun — только ASCII и CJK). В M5GFX
  кириллица включается одной строкой.

Подробнее — [`docs/DECISIONS.md`](docs/DECISIONS.md), решения Р-001…Р-003.

---

## Быстрый старт

```bash
# 1. Собрать
pio run -e tab5

# 2. Прошить (USB-C)
pio run -e tab5 -t upload

# 3. Смотреть лог
pio device monitor -b 115200

# 4. Прогнать тесты логики (без железа)
pio test -e native

# 5. Проверить всё, что проверяет CI (формат, cppcheck, тесты, сборка)
./scripts/check.sh

# 6. То же в контейнере — максимально близко к CI
./scripts/check.sh docker
```

Первая сборка долгая: скачивается Arduino core и ESP-IDF (несколько ГБ).

**Перед коммитом** достаточно `./scripts/check.sh`: скрипт ставит
зафиксированную версию clang-format через pip, поэтому результат совпадает с CI
на любой ОС. Голый системный `clang-format` не используйте — в Ubuntu 24.04 это
18.1.3, а на macOS 21.0.0, и они расходятся в форматировании.

**Перед первой прошивкой** снимите дамп заводской прошивки — обратно её взять
негде:

```bash
~/.platformio/packages/tool-esptool/esptool --chip esp32p4 -p /dev/cu.usbmodem* \
    -b 921600 read_flash 0 0x1000000 backup/tab5-factory-16mb.bin
```

---

## Структура

```
.
├── AGENTS.md              — сводка для агентов и людей
├── CHANGELOG.md           — история версий
├── platformio.ini         — env:tab5 (плата) и env:native (тесты)
├── include/               — конфигурация и версия
├── src/                   — прошивка
├── test/                  — юнит-тесты
├── scripts/               — локальные проверки (scripts/check.sh)
├── docs/                  — вся документация
└── .github/workflows/     — CI
```

---

## Документация

| Документ | О чём |
|---|---|
| [`AGENTS.md`](AGENTS.md) | Краткая сводка, статус, команды, правила работы |
| [`docs/DEVELOPMENT_PLAN.md`](docs/DEVELOPMENT_PLAN.md) | Этапы до v1.0.0, критерии готовности, сценарии приёмки, backlog к 2.0.0 |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Слои, модули, задачи FreeRTOS, форматы файлов, инструменты агента |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | Журнал решений и отклонённые альтернативы |
| [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) | Версии всего и обоснование выбора |
| [`docs/HARDWARE_VERIFICATION.md`](docs/HARDWARE_VERIFICATION.md) | Что проверено на живой плате |
| [`docs/BOARD_AI_RESEARCH.md`](docs/BOARD_AI_RESEARCH.md) | Распиновка, дисплей, питание, грабли |
| [`docs/TAB5_BOARD_FACTORY_MANUAL.md`](docs/TAB5_BOARD_FACTORY_MANUAL.md) | Официальная документация M5Stack |
| [`TECHNICAL_TASK.md`](TECHNICAL_TASK.md) | Исходное ТЗ |

---

## Ограничения MVP

Не входит в MVP, запланировано на 2.0.0: голос (STT/TTS), камера,
OTA-обновления, рендер markdown на экране, векторный поиск, умный дом.

Без интернета устройство остаётся работоспособным: документы и история доступны,
поиск и диалог сообщают об отсутствии сети.

Ввод в MVP — только с клавиатуры A164. Голосового ввода нет.
