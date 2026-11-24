#!/usr/bin/env python3
"""Живая проверка RouterAI: то, чего нет в документации.

Скрипт отвечает на вопросы, от которых зависит код прошивки, но которые нельзя
выяснить по докам:

  * приходит ли `usage` в СТРИМИНГЕ и какими именами полей (`input_tokens` или
    `prompt_tokens`) — от этого зависит счётчик токенов в статус-баре;
  * как выглядит `reasoning_effort` у DeepSeek: работает ли и не ломает ли
    стриминг;
  * что реально приходит в теле ошибки 401/404 — от этого зависит текст,
    который увидит пользователь;
  * какими кусками идёт поток (размер дельт) — влияет на выбор буферов.

Ключ берётся из key.txt в корне репозитория или из переменной окружения
ROUTERAI_API_KEY. Файл в .gitignore, в лог ключ не печатается.

Стоимость: несколько коротких запросов к дешёвой модели, копейки.

Запуск:
    ./scripts/api_smoke.py            # все проверки
    ./scripts/api_smoke.py --raw      # плюс дамп сырого потока SSE
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import time

BASE_URL = "https://routerai.ru/api/v1"
MODEL = "~deepseek/deepseek-v4-flash-latest"

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
KEY_FILE = REPO_ROOT / "key.txt"


def load_key() -> str:
    env = os.environ.get("ROUTERAI_API_KEY", "").strip()
    if env:
        return env
    if KEY_FILE.exists():
        key = KEY_FILE.read_text(encoding="utf-8").strip()
        if key:
            return key
    sys.exit(f"Нет ключа: положите его в {KEY_FILE} или в ROUTERAI_API_KEY")


def post(payload: dict, key: str, stream: bool):
    import urllib.error
    import urllib.request

    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        f"{BASE_URL}/chat/completions",
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {key}",
            "Accept": "text/event-stream" if stream else "application/json",
        },
    )
    try:
        return urllib.request.urlopen(req, timeout=120)
    except urllib.error.HTTPError as e:
        return e


def section(title: str) -> None:
    print(f"\n\033[1m== {title} ==\033[0m")


def ok(msg: str) -> None:
    print(f"\033[32m✓\033[0m {msg}")


def warn(msg: str) -> None:
    print(f"\033[33m!\033[0m {msg}")


def fail(msg: str) -> None:
    print(f"\033[31m✗\033[0m {msg}")


def check_auth(key: str) -> None:
    section("Авторизация и неверный ключ")
    resp = post(
        {"model": MODEL, "messages": [{"role": "user", "content": "1"}]},
        "sk-this-key-is-definitely-wrong",
        stream=False,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    body = resp.read().decode("utf-8", "replace")
    print(f"  HTTP {status}")
    print(f"  тело: {body[:400]}")
    if status == 401:
        ok("неверный ключ даёт 401")
    else:
        warn(f"ожидали 401, получили {status}")
    try:
        parsed = json.loads(body)
        print(f"  JSON-поля: {list(parsed)}")
    except json.JSONDecodeError:
        warn("тело ошибки — не JSON")


def check_plain(key: str) -> None:
    section("Обычный запрос (без стриминга)")
    t0 = time.time()
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "Скажи одно слово: привет"}],
            "temperature": 0,
        },
        key,
        stream=False,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    body = resp.read().decode("utf-8", "replace")
    dt = time.time() - t0

    if status != 200:
        fail(f"HTTP {status}: {body[:300]}")
        return

    data = json.loads(body)
    ok(f"HTTP 200 за {dt:.2f} с")

    print(f"  id: {data.get('id')}")
    print(f"  model вернулся: {data.get('model')}")
    print(f"  finish_reason: {data['choices'][0].get('finish_reason')}")
    print(f"  текст: {data['choices'][0]['message'].get('content')!r}")
    reasoning = data["choices"][0]["message"].get("reasoning")
    if reasoning:
        print(f"  reasoning: {str(reasoning)[:120]!r}")

    usage = data.get("usage") or {}
    print(f"  usage: {json.dumps(usage, ensure_ascii=False)}")
    names = set(usage)
    if {"input_tokens", "output_tokens"} & names:
        ok("usage использует input_tokens/output_tokens (наш случай)")
    if {"prompt_tokens", "completion_tokens"} & names:
        ok("usage использует prompt_tokens/completion_tokens (OpenAI-стиль)")


def check_stream(key: str, raw: bool) -> None:
    section("Стриминг: формат чанков и usage")
    t0 = time.time()
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "Перечисли три цвета через запятую"}],
            "stream": True,
            "temperature": 0,
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        fail(f"HTTP {status}: {resp.read().decode('utf-8', 'replace')[:300]}")
        return

    ctype = resp.headers.get("Content-Type", "")
    print(f"  Content-Type: {ctype}")
    if "event-stream" in ctype:
        ok("сервер отдаёт text/event-stream")
    else:
        warn("Content-Type не event-stream — проверьте разбор")

    raw_lines: list[str] = []
    events = 0
    deltas = 0
    text = ""
    usage_seen = None
    finish = None
    first_byte_at = None

    while True:
        line = resp.readline()
        if not line:
            break
        if first_byte_at is None:
            first_byte_at = time.time() - t0
        raw_lines.append(line.decode("utf-8", "replace").rstrip("\n"))

        line_s = line.decode("utf-8", "replace").strip()
        if not line_s.startswith("data:"):
            continue
        payload = line_s[5:].strip()
        if payload == "[DONE]":
            events += 1
            break
        events += 1
        try:
            chunk = json.loads(payload)
        except json.JSONDecodeError:
            warn(f"чанк не разобрался как JSON: {payload[:120]}")
            continue

        choices = chunk.get("choices") or []
        if choices:
            delta = choices[0].get("delta") or {}
            piece = delta.get("content")
            if piece:
                deltas += 1
                text += piece
            if choices[0].get("finish_reason"):
                finish = choices[0]["finish_reason"]
        if chunk.get("usage"):
            usage_seen = chunk["usage"]

    dt = time.time() - t0
    print(f"  событий: {events} (из них с текстом: {deltas})")
    print(f"  первый чанк через {first_byte_at:.2f} с, весь ответ {dt:.2f} с")
    print(f"  finish_reason: {finish}")
    print(f"  текст: {text!r}")

    if finish:
        ok("finish_reason приходит в стриминге")
    else:
        fail("finish_reason не пришёл — обрыв ответа не будет виден")

    if usage_seen:
        print(f"  usage: {json.dumps(usage_seen, ensure_ascii=False)}")
        ok("usage приходит в стриминге — счётчик токенов работает без второго запроса")
    else:
        warn("usage в стриминге НЕТ: для токенов нужен include_usage или подсчёт по тексту")
        # Проверим, помогает ли stream_options.include_usage (стиль OpenAI).
        check_include_usage(key)

    if raw:
        section("Сырой поток (первые 40 строк)")
        for ln in raw_lines[:40]:
            print(f"  {ln}")


def check_include_usage(key: str) -> None:
    section("Проверка stream_options.include_usage")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "Скажи: ок"}],
            "stream": True,
            "stream_options": {"include_usage": True},
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        warn(f"HTTP {status}: {resp.read().decode('utf-8', 'replace')[:200]}")
        return

    usage = None
    while True:
        line = resp.readline()
        if not line:
            break
        s = line.decode("utf-8", "replace").strip()
        if not s.startswith("data:"):
            continue
        payload = s[5:].strip()
        if payload == "[DONE]":
            break
        try:
            chunk = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if chunk.get("usage"):
            usage = chunk["usage"]

    if usage:
        ok(f"include_usage работает: {json.dumps(usage, ensure_ascii=False)}")
    else:
        warn("include_usage не дал usage — считаем токены сами или вторым запросом")


def check_reasoning(key: str) -> None:
    section("reasoning_effort у DeepSeek")
    for effort in ("none", "low"):
        t0 = time.time()
        resp = post(
            {
                "model": MODEL,
                "messages": [{"role": "user", "content": "Сколько будет 2+2? Ответь цифрой"}],
                "stream": True,
                "temperature": 0,
                "reasoning_effort": effort,
            },
            key,
            stream=True,
        )
        status = getattr(resp, "status", None) or resp.getcode()
        if status != 200:
            fail(f"reasoning_effort={effort}: HTTP {status}: {resp.read().decode('utf-8', 'replace')[:200]}")
            continue

        text = ""
        reasoning = ""
        while True:
            line = resp.readline()
            if not line:
                break
            s = line.decode("utf-8", "replace").strip()
            if not s.startswith("data:"):
                continue
            payload = s[5:].strip()
            if payload == "[DONE]":
                break
            try:
                chunk = json.loads(payload)
            except json.JSONDecodeError:
                continue
            choices = chunk.get("choices") or []
            if not choices:
                continue
            delta = choices[0].get("delta") or {}
            if delta.get("content"):
                text += delta["content"]
            # Некоторые провайдеры кладут рассуждения в delta.reasoning
            if delta.get("reasoning"):
                reasoning += delta["reasoning"]

        dt = time.time() - t0
        ok(f"reasoning_effort={effort}: HTTP 200 за {dt:.2f} с, ответ {text.strip()!r}")
        if reasoning:
            print(f"  рассуждения приходят в delta.reasoning: {reasoning[:80]!r}")
        else:
            print("  поля reasoning в дельте нет (рассуждения не отдаются или выключены)")


def check_bad_model(key: str) -> None:
    section("Несуществующая модель")
    resp = post(
        {"model": "no-such/model-xyz", "messages": [{"role": "user", "content": "1"}]},
        key,
        stream=False,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    body = resp.read().decode("utf-8", "replace")
    print(f"  HTTP {status}")
    print(f"  тело: {body[:300]}")
    if status in (400, 404):
        ok(f"неизвестная модель даёт {status} — покажем понятный текст")
    else:
        warn(f"неожиданный код {status}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", action="store_true", help="дамп сырого потока SSE")
    args = parser.parse_args()

    key = load_key()
    print(f"Модель: {MODEL}")
    print(f"Ключ: {key[:6]}…{key[-4:]} ({len(key)} символов)")  # не целиком

    check_auth(key)
    check_plain(key)
    check_stream(key, args.raw)
    check_reasoning(key)
    check_bad_model(key)

    print("\n\033[32mГотово.\033[0m")


if __name__ == "__main__":
    main()
