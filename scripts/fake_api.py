#!/usr/bin/env python3
"""Фальшивый OpenAI-совместимый сервер: проверка путей, которые не вызвать вопросом.

Зачем: два критерия приёмки v0.4.0 нельзя воспроизвести живым вопросом.
Сервер на бессмысленный запрос отдаёт смежные страницы (Р-036 п. 7), поэтому
«поиск ничего не нашёл» вопросом не получить; отказ поискового сервера тоже не
устроишь по заказу. Здесь оба случая выдаются по команде и **бесплатно**: плата
ходит на ноутбук, а не в интернет.

Сервер изображает полный агентский цикл, чтобы проверить именно прошивку:

1. Основной запрос → стриминг вызова `web_search` (`finish_reason: tool_calls`).
2. Поисковый подзапрос (в теле есть `plugins`) → то, что задано режимом:
   `empty` — ни текста, ни `annotations`; `error` — HTTP 500; `ok` — текст
   и источники.
3. Второй основной запрос (последнее сообщение в истории — `role=tool`) →
   обычный текстовый ответ.

    ./scripts/fake_api.py --mode empty          # «поиск без результата»
    ./scripts/fake_api.py --mode error          # «поиск не удался»
    ./scripts/fake_api.py --mode recover        # 500 на первой попытке, успех на повторе
    ./scripts/fake_api.py --mode ok             # контроль: поиск что-то нашёл
    ./scripts/fake_api.py --mode empty --port 8765

Адрес на плате: `http://<ip ноутбука>:8765/api/v1`. Ставится командой
`api <адрес>` по Serial (или в `Ctrl+A` → «Адрес API»), возвращается так же:
`api https://routerai.ru/api/v1`. Ключ API подойдёт любой: сервер его не
проверяет.

Скрипт ничего не знает о ключах и не ходит в интернет.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Модель и id вызова — любые: прошивка их только читает.
MODEL = "fake-model"
CALL_ID = "call_fake_1"


def sse(event: dict) -> bytes:
    """Одно событие SSE. Ровно так, как их присылает RouterAI."""
    return b"data: " + json.dumps(event, ensure_ascii=False).encode("utf-8") + b"\n\n"


def chunk(delta: dict, finish: str | None = None) -> dict:
    return {
        "id": "chatcmpl-fake",
        "object": "chat.completion.chunk",
        "model": MODEL,
        "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
    }


def tool_call_delta() -> dict:
    """Вызов инструмента web_search одним куском аргументов."""
    args = json.dumps({"query": "сброс фильтра тест", "max_results": 3}, ensure_ascii=False)
    return {
        "tool_calls": [
            {"index": 0, "id": CALL_ID, "type": "function", "function": {"name": "web_search", "arguments": args}}
        ]
    }


def main_request_stream() -> list[bytes]:
    """Ответ на основной запрос: попросить поиск и закончить ход."""
    return [
        sse(chunk({"role": "assistant", "content": None})),
        sse(chunk(tool_call_delta())),
        sse(chunk({}, finish="tool_calls")),
        b"data: [DONE]\n\n",
    ]


def final_request_stream() -> list[bytes]:
    """Ответ, когда в истории уже есть результат инструмента: обычный текст."""
    text = ("Нажимать и держать кнопку сброса не нужно: на этом сервере данные "
            "не проверены, ответ учебный.")
    events = [sse(chunk({"role": "assistant", "content": ""}))]
    for i in range(0, len(text), 20):
        events.append(sse(chunk({"content": text[i : i + 20]})))
    events.append(sse(chunk({}, finish="stop")))
    events.append(
        sse(
            {
                "id": "chatcmpl-fake",
                "object": "chat.completion.chunk",
                "model": MODEL,
                "choices": [],
                "usage": {"prompt_tokens": 10, "completion_tokens": 20, "total_tokens": 30},
            }
        )
    )
    events.append(b"data: [DONE]\n\n")
    return events


def search_stream(mode: str) -> list[bytes]:
    """Ответ на поисковый подзапрос. Здесь и живут оба проверяемых случая."""
    if mode == "empty":
        # Ни текста, ни источников: ровно то, что бывает при пустой выдаче.
        # `annotations` нет вовсе — так же, как когда сервер ничего не нашёл.
        return [
            sse(chunk({"role": "assistant", "content": ""})),
            sse(chunk({}, finish="stop")),
            b"data: [DONE]\n\n",
        ]

    text = "Сброс счётчика фильтра: удерживайте кнопку три секунды."
    citations = [
        {
            "type": "url_citation",
            "url": "https://example.invalid/manual",
            "title": "Инструкция (фальшивая)",
            "content": "Удерживайте кнопку три секунды, затем отпустите.",
            "start_index": 0,
            "end_index": 10,
        }
    ]
    return [
        sse(chunk({"role": "assistant", "content": ""})),
        sse(chunk({"content": text})),
        sse(chunk({"annotations": citations})),
        sse(chunk({}, finish="stop")),
        b"data: [DONE]\n\n",
    ]


class Handler(BaseHTTPRequestHandler):
    mode = "empty"
    searches = 0
    mains = 0
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("[fake] " + fmt % args + "\n")

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw)
        except json.JSONDecodeError:
            body = {}

        has_plugins = "plugins" in body
        has_tools = bool(body.get("tools"))
        # Видно, что именно прислала плата: без этого «пришёл финальный ответ
        # вместо вызова» не отличить от ошибки в сервере.
        sys.stderr.write(
            "[fake] ключи: %s, tools=%s, plugins=%s\n"
            % (",".join(sorted(body.keys())), has_tools, has_plugins)
        )

        if has_plugins:
            Handler.searches += 1
            # `error` — сбой всегда; `recover` — сбой только на первой попытке.
            # Второй режим важен отдельно: успешный повтор обязан разобрать
            # ответ, а не только дойти до сервера.
            fail_now = Handler.mode == "error" or (Handler.mode == "recover" and Handler.searches == 1)
            if fail_now:
                # 500 — временная ошибка, прошивка повторит её дважды. Это тоже
                # часть проверки: важно, что повторы не вешают плату и что они
                # отправляют ТО ЖЕ тело (иначе сервер увидит пустой запрос).
                payload = json.dumps({"error": {"message": "фальшивый сбой сервера"}}, ensure_ascii=False).encode("utf-8")
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            self.stream(search_stream("ok" if Handler.mode == "recover" else Handler.mode))
            return

        # Основной запрос. Отличаем первый шаг от второго по ПОСЛЕДНЕМУ
        # сообщению: после вызова инструмента оно с ролью tool. Проверять «есть
        # ли tool где-то в истории» нельзя — в треде остаются результаты
        # прошлых поисков, и второй ход подряд выглядел бы как продолжение.
        messages = body.get("messages") or []
        saw_tool_result = bool(messages) and messages[-1].get("role") == "tool"
        Handler.mains += 1
        if has_tools and not saw_tool_result:
            self.stream(main_request_stream())
        else:
            self.stream(final_request_stream())

    def stream(self, events: list[bytes]) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        for ev in events:
            self.wfile.write(ev)
            self.wfile.flush()
            # Небольшая пауза: клиент читает неблокирующе, и мгновенная выдача
            # выглядела бы как один кусок. Так честнее к живому серверу.
            time.sleep(0.01)

    def do_GET(self) -> None:
        # Удобно проверять браузером, что сервер поднялся.
        info = {"mode": Handler.mode, "mains": Handler.mains, "searches": Handler.searches}
        payload = json.dumps(info, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", default="empty", choices=["empty", "error", "recover", "ok"])
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()

    Handler.mode = args.mode
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Фальшивый API: режим {args.mode}, http://{args.host}:{args.port}/api/v1")
    print(f"На плате: Serial \"api http://<ip>:{args.port}/api/v1\" или Ctrl+A → «Адрес API»")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nОстановлен.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
