#!/usr/bin/env python3
"""Долгая сессия: 20 ходов подряд, контроль PSRAM и heap от хода к ходу.

Зачем: сценарий приёмки С6 — «20 сообщений подряд с несколькими вызовами
инструментов: нет утечек PSRAM, интерфейс отзывчив». Живыми вопросами это
стоит 20-40 ₽ и занимает полчаса, а утечку ловит не смысл ответа, а объём
работы: столько же запросов, столько же разборов SSE.

Поэтому по умолчанию сессия идёт на фальшивый API (`scripts/fake_api.py`):
бесплатно, повторяемо, и каждый ход действительно проходит полный агентский
цикл (вызов `web_search`, поисковый подзапрос, второй основной запрос).
Строку «Ответ: … PSRAM N КБ, heap N КБ» печатает прошивка в конце каждого
хода — по ней и видно, убывает ли память.

    ./scripts/long_session.py                    # 20 ходов на фальшивом API
    ./scripts/long_session.py --turns 5          # короткий прогон
    ./scripts/long_session.py --live             # на настоящем API (платно!)

Перед прогоном скрипт поднимает фальшивый сервер и переключает плату на него
командой `api http://<ip ноутбука>:8765/api/v1`, а после — возвращает
настоящий адрес. Если прогон прервать, адрес останется подменённым: вернуть
его можно командой `api https://routerai.ru/api/v1`.
"""

from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time

import serial

# Строка, которую печатает ChatScreen::log_turn_result() в конце хода.
TURN_RE = re.compile(
    r"Ответ: (.+?), источников (\d+), поиск (.+?), шагов агента (\d+), PSRAM (\d+) КБ, heap (\d+) КБ"
)

# «Чат: в запрос ушло N сообщений, отброшено M, бюджет K КБ» — обрезка истории
# по бюджету. Печатается только когда что-то отброшено.
TRIM_RE = re.compile(r"отброшено (\d+), бюджет")


def laptop_ip() -> str:
    """Адрес ноутбука в локальной сети: по нему плата достучится до сервера."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Адрес не отправляется — connect только выбирает маршрут наружу.
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    finally:
        sock.close()


def run(args: argparse.Namespace) -> int:
    port = args.port
    turns = args.turns

    try:
        link = serial.Serial(port, 115200, timeout=0.2)
    except serial.SerialException as exc:
        print(f"Порт {port} недоступен: {exc}", file=sys.stderr)
        return 1

    server: subprocess.Popen[bytes] | None = None
    old_api = "https://routerai.ru/api/v1"

    def send(text: str) -> None:
        link.write((text + "\n").encode("utf-8"))
        link.flush()

    def wait_for(pattern: re.Pattern[str], timeout_s: float) -> re.Match[str] | None:
        """Читать вывод, пока не найдётся строка. Возвращает совпадение."""
        buf = ""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            chunk = link.read(4096)
            if chunk:
                buf += chunk.decode("utf-8", "replace")
                m = pattern.search(buf)
                if m:
                    return m
        return None

    try:
        if not args.live:
            server = subprocess.Popen(
                [sys.executable, "-u", "scripts/fake_api.py", "--mode", args.fake_mode, "--port", str(args.fake_port)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            # Сервер поднимается мгновенно, но дать ему кадр-другой дешевле,
            # чем ловить «connection refused» на первом ходе.
            time.sleep(1.0)
            fake_api = f"http://{laptop_ip()}:{args.fake_port}/api/v1"
            print(f"Фальшивый API: {fake_api}")
        else:
            fake_api = None
            print("Живой API: каждый ход платный (~2 ₽ с поиском)")

        if fake_api is not None:
            send(f"api {fake_api}")
            if wait_for(re.compile(r"адрес API изменён"), 5.0) is None:
                print("Плата не подтвердила смену адреса — продолжаю, но адрес мог не примениться")
            # Переключение адреса не перезагружает плату, но дать кадр полезно.
            time.sleep(1.0)

        send("screen chat")

        history: list[tuple[int, int, int]] = []
        failures = 0
        trims = 0

        for i in range(1, turns + 1):
            print(f"— ход {i}/{turns}", flush=True)
            send(f"ask ход {i}: сброс фильтра")

            # Ход с вызовом инструмента — это три запроса подряд и паузы по
            # 10 мс на событие; сорока секунд хватает с запасом.
            deadline = time.time() + args.turn_timeout
            psram = heap = None
            ok = None
            trimmed = False
            buf = ""
            while time.time() < deadline:
                chunk = link.read(4096)
                if not chunk:
                    continue
                buf += chunk.decode("utf-8", "replace")
                m = TURN_RE.search(buf)
                if m:
                    # Строка «отброшено N» печатается перед «Ответ:», поэтому
                    # к моменту совпадения она уже в буфере.
                    trimmed = TRIM_RE.search(buf) is not None
                    ok = m.group(1).strip()
                    psram = int(m.group(5))
                    heap = int(m.group(6))
                    break

            if psram is None:
                print(f"  нет строки «Ответ:» за {args.turn_timeout} с — ход не отработал")
                failures += 1
                continue

            if trimmed:
                trims += 1

            history.append((i, psram, heap))
            delta = ""
            if len(history) > 1:
                d_psram = psram - history[-2][1]
                d_heap = heap - history[-2][2]
                delta = f", ΔPSRAM {d_psram:+d} КБ, Δheap {d_heap:+d} КБ"
            print(f"  ответ {ok}, PSRAM {psram} КБ, heap {heap} КБ{delta}", flush=True)
            if trimmed:
                print("  история обрезана по бюджету — проверяется Р-047", flush=True)

            if ok != "ок":
                failures += 1

        print()
        if not history:
            print("Ни один ход не отработал — прогон не состоялся")
            return 1

        first = history[0]
        last = history[-1]
        print(f"Итог: ходов {len(history)}/{turns}, сбоев {failures}, с обрезкой истории {trims}")
        print(f"PSRAM: {first[1]} → {last[1]} КБ (Δ{last[1] - first[1]:+d})")
        print(f"heap:  {first[2]} → {last[2]} КБ (Δ{last[2] - first[2]:+d})")

        # Для режима `fat` обрезка — цель прогона, а не случайность: без неё
        # исправление Р-047 на плате не проверено ни разу.
        if args.fake_mode == "fat" and trims == 0:
            print("ВНИМАНИЕ: обрезка истории не срабатывала — бюджет не достигнут.")
            print("Проверка бюджета не состоялась; увеличьте --turns.")
            return 2

        # Порог произвольный, но осмысленный: за ход допускается небольшое
        # колебание, а утечка растёт монотонно. Пять килобайт на двадцать
        # ходов — четверть килобайта на ход; больше уже пахнет утечкой.
        if first[1] - last[1] > 5:
            print("ВНИМАНИЕ: PSRAM убывает — похоже на утечку. Смотрите, после какого хода.")
            return 2
        if first[2] - last[2] > 5:
            print("ВНИМАНИЕ: heap убывает — похоже на утечку.")
            return 2

        print("Память стабильна: утечки не видно")
        return 0 if failures == 0 else 2

    finally:
        if server is not None:
            server.terminate()
            server.wait(timeout=5)
        if not args.live and not args.keep_api:
            # Возвращаем настоящий адрес: иначе после прогона плата ходила бы
            # на выключенный ноутбук и отвечала «сеть недоступна».
            try:
                link.write((f"api {old_api}\n").encode("utf-8"))
                link.flush()
                time.sleep(0.5)
            except Exception:  # noqa: BLE001 — порт мог закрыться
                pass
        link.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--turns", type=int, default=20, help="сколько ходов сделать")
    parser.add_argument("--turn-timeout", type=float, default=40.0, help="сколько ждать ответа на ход")
    parser.add_argument("--fake-port", type=int, default=8765)
    parser.add_argument(
        "--fake-mode",
        default="ok",
        choices=["ok", "empty", "error", "recover", "fat"],
        help="режим фальшивого API; fat — вызовы по 3.5 КБ для проверки бюджета истории",
    )
    parser.add_argument("--live", action="store_true", help="на настоящем API (платно)")
    parser.add_argument("--keep-api", action="store_true", help="не возвращать настоящий адрес API")
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
