#!/usr/bin/env python3
"""Разговор с платой по Serial: отправка команды и чтение ответа.

Зачем: приёмочный прогон чата требует вопроса на устройстве. Ввод с
клавиатуры A164 человека не отменить, но команда `ask` по Serial идёт тем же
путём (см. `util/serial_cmd`, `ChatScreen::ask`), поэтому прогон можно
повторить скриптом.

    ./scripts/tab5_serial.py --reset --wait 20          # только загрузка
    ./scripts/tab5_serial.py help                        # подсказка, без денег
    ./scripts/tab5_serial.py --ask "2 + 2?" --wait 60    # платный запрос

Скрипт НЕ содержит ключей и не ходит в API: он только говорит с платой.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text", nargs="?", help="команда: help, ask <вопрос>, > <вопрос>")
    parser.add_argument("--ask", help="вопрос в чат (то же, что 'ask ...')")
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--reset", action="store_true", help="перезагрузить плату")
    parser.add_argument("--wait", type=float, default=25.0, help="сколько секунд читать вывод")
    parser.add_argument("--grep", help="показывать только строки, содержащие подстроку")
    parser.add_argument("--then", help="вторая команда, отправить её позже (например 'wifi off')")
    parser.add_argument(
        "--then-after",
        type=float,
        default=3.0,
        help="через сколько секунд после первой команды отправить --then",
    )
    args = parser.parse_args()

    s = serial.Serial(args.port, 115200, timeout=1)
    if args.reset:
        # Сброс через DTR/RTS: так же делают pio и esptool после прошивки.
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.2)
        s.setRTS(False)
        time.sleep(0.3)

    command = None
    if args.ask:
        command = f"ask {args.ask}"
    elif args.text:
        command = args.text

    # Команда без вопроса (например `wifi off`) тоже ждёт готовности сети, но
    # только если мы её не выключаем: после `wifi off` строка подключения уже
    # не появится, и ожидание съело бы минуту впустую.
    wait_for_wifi = command is not None and not command.strip().lower().startswith("wifi off")

    if command:
        # Ждём готовности, а не фиксированную паузу: сразу после сброса Wi-Fi
        # ещё подключается, и вопрос получил бы «Нет Wi-Fi» вместо ответа.
        #
        # Признак — строка лога о подключении («[wifi] подключено к …»).
        # Строка «Статус:» для этого не годится дважды: она печатается в
        # setup() ДО подключения (сеть поднимается в фоне, ~9 с) и всего один
        # раз, так что «Wi-Fi OK» в ней обычно означает «нет».
        if wait_for_wifi:
            deadline = time.time() + 60.0
            ready = b""
            while time.time() < deadline:
                chunk = s.read(4096)
                if chunk:
                    ready += chunk
                    if "[wifi] подключено".encode("utf-8") in ready:
                        break
            # Небольшая пауза после подключения: адрес и NTP приходят следом, а
            # вопросу нужен рабочий сокет, а не только факт соединения.
            time.sleep(1.0)
        s.write((command + "\n").encode("utf-8"))
        s.flush()

    if args.then:
        time.sleep(args.then_after)
        s.write((args.then + "\n").encode("utf-8"))
        s.flush()

    buf = b""
    t0 = time.time()
    while time.time() - t0 < args.wait:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
    s.close()

    text = buf.decode("utf-8", "replace")
    for line in text.splitlines():
        if args.grep and args.grep not in line:
            continue
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
