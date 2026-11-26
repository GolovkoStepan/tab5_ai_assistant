#!/usr/bin/env python3
"""Вторая разведка веб-поиска: то, чего не было в ``web_search_smoke.py``.

Первый скрипт отвечал на вопросы архитектуры: принимается ли плагин, работает
ли он с ``tools``, в каком виде приходят источники. Этот — отвечает на вопросы
**прошивки**, которая отличается от него в трёх важных местах:

  * она **стримит** (``stream: true``), а проверка 3 в первом скрипте шла без
    стриминга — то есть не в том режиме, в котором работает устройство;
  * она ведёт **агентский цикл**: после вызова инструмента отправляет второй
    запрос с историей (``role: tool``) — а в нём источники могут не приходить;
  * она держит событие SSE в буфере 8 КБ (``sse::kMaxEventBytes``): событие с
    ``annotations`` может не влезть, и тогда источники потеряются молча.

Что проверяем:

  A. ``plugins`` + ``tools`` + **стриминг**: приходят ли ``annotations`` и
     ``tool_calls`` в одном ответе и не мешают ли друг другу.
  B. **Второй шаг агентского цикла**: запрос с историей (вызов + результат
     ``role: tool``) и стримингом — приходят ли источники на этом шаге.
  C. **Худший случай размера события** с ``annotations``: ``max_results=8`` и
     длинный ``content``. От этого зависит, хватит ли ``kMaxEventBytes``.
  D. **Стоимость**: приходит ли ``cost`` в ``usage`` при стриминге и сколько
     стоит один вопрос с поиском. По этому владелец решает, не дорог ли этап.
  E. Приходит ли ``annotations``, когда модель **не** искала (вопрос из
     головы) — чтобы прошивка не показывала «поиск не дал результатов» там,
     где поиска не было вовсе.

Ключ берётся из key.txt в корне репозитория или из ROUTERAI_API_KEY.
В лог ключ не печатается.

Запуск:
    ./scripts/web_search_probe.py
    ./scripts/web_search_probe.py --raw    # плюс сырые чанки SSE
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

WEB_PLUGIN = [{"id": "web"}]

SEARCH_TOOL = {
    "type": "function",
    "function": {
        "name": "search_docs",
        "description": "Найти документы в личном архиве по ключевым словам.",
        "parameters": {
            "type": "object",
            "properties": {"query": {"type": "string", "description": "Что искать"}},
            "required": ["query"],
        },
    },
}

APPLIED_PROMPT = (
    "Очиститель воздуха просит заменить фильтр, я забыл, как сбросить ошибку "
    "на Xiaomi Mi Air Purifier 3. Сначала поищи в моём архиве документов "
    "инструментом search_docs, потом ответь по шагам."
)

NO_SEARCH_PROMPT = "Сколько будет 17 умножить на 23? Ответь одним числом."


def load_key() -> str:
    env = os.environ.get("ROUTERAI_API_KEY", "").strip()
    if env:
        return env
    if KEY_FILE.exists():
        return KEY_FILE.read_text(encoding="utf-8").strip()
    sys.exit("Нет ключа: положи его в key.txt или задай ROUTERAI_API_KEY")


def post(payload: dict, key: str):
    import urllib.error
    import urllib.request

    req = urllib.request.Request(
        f"{BASE_URL}/chat/completions",
        data=json.dumps(payload).encode(),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {key}",
            "Accept": "text/event-stream",
            "Accept-Encoding": "identity",
        },
    )
    try:
        return urllib.request.urlopen(req, timeout=180)
    except urllib.error.HTTPError as e:
        print(f"  HTTP {e.code}: {e.read().decode('utf-8', 'replace')[:300]}")
        return None
    except Exception as e:  # noqa: BLE001
        print(f"  сеть: {e}")
        return None


def read_sse(resp, raw: bool) -> dict:
    """Читает поток и собирает то, что нужно прошивке."""
    result = {
        "events": 0,
        "max_event": 0,
        "annotation_events": [],
        "urls": [],
        "tool_calls": [],
        "text": "",
        "usage": None,
        "finish_reason": None,
        "event_sizes": [],
    }
    raw_buf = b""
    tool_acc: dict[int, dict] = {}

    for line in resp:
        raw_buf += line
        while b"\n\n" in raw_buf:
            event, raw_buf = raw_buf.split(b"\n\n", 1)
            result["events"] += 1
            size = len(event)
            result["max_event"] = max(result["max_event"], size)
            result["event_sizes"].append(size)

            if raw:
                print(f"  [{size:5d}] {event[:300].decode('utf-8', 'replace')}")

            data = None
            for part in event.split(b"\n"):
                if part.startswith(b"data: "):
                    data = part[6:]
            if data is None or data.strip() == b"[DONE]":
                continue
            try:
                chunk = json.loads(data)
            except json.JSONDecodeError:
                continue

            choices = chunk.get("choices") or []
            if choices:
                choice = choices[0]
                delta = choice.get("delta") or {}
                if delta.get("content"):
                    result["text"] += delta["content"]
                if choice.get("finish_reason"):
                    result["finish_reason"] = choice["finish_reason"]

                ann = delta.get("annotations")
                if ann:
                    result["annotation_events"].append(size)
                    for item in ann:
                        url = (item.get("url_citation") or {}).get("url")
                        if url and url not in result["urls"]:
                            result["urls"].append(url)

                for tc in delta.get("tool_calls") or []:
                    idx = tc.get("index", 0)
                    slot = tool_acc.setdefault(idx, {"name": "", "args": ""})
                    fn = tc.get("function") or {}
                    if fn.get("name"):
                        slot["name"] += fn["name"]
                    if fn.get("arguments"):
                        slot["args"] += fn["arguments"]

            if chunk.get("usage"):
                result["usage"] = chunk["usage"]

    result["tool_calls"] = [v for _, v in sorted(tool_acc.items())]
    return result


def section(title: str) -> None:
    print(f"\n\033[1m== {title} ==\033[0m")


def ok(msg: str) -> None:
    print(f"\033[32m✓\033[0m {msg}")


def warn(msg: str) -> None:
    print(f"\033[33m!\033[0m {msg}")


def check_stream_with_tools(key: str, raw: bool) -> dict | None:
    section("A. plugins + tools + СТРИМИНГ — тот режим, в котором работает прошивка")
    if raw:
        print("  (сырые чанки ниже)")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": APPLIED_PROMPT}],
            "plugins": WEB_PLUGIN,
            "tools": [SEARCH_TOOL],
            "stream": True,
        },
        key,
    )
    if resp is None:
        return None

    t0 = time.time()
    r = read_sse(resp, False)
    dt = time.time() - t0

    print(f"  событий: {r['events']}, максимальное: {r['max_event']} байт, {dt:.1f} с")
    print(f"  finish_reason: {r['finish_reason']}")
    print(f"  вызовов инструментов: {len(r['tool_calls'])}")
    for tc in r["tool_calls"]:
        print(f"    {tc['name']}: {tc['args'][:120]}")
    print(f"  текста: {len(r['text'])} символов")
    print(f"  событий с annotations: {len(r['annotation_events'])}, размеры: {r['annotation_events']}")
    print(f"  источников: {len(r['urls'])}")
    for u in r["urls"][:3]:
        print(f"    {u}")
    if r["usage"]:
        print(f"  usage: {json.dumps(r['usage'], ensure_ascii=False)}")
    if raw:
        pass

    if r["annotation_events"] and r["tool_calls"]:
        ok("в стриминге приходят И вызовы, И источники — прошивке хватит одного запроса")
    elif r["tool_calls"]:
        warn("вызовы есть, источников нет — на шаге с инструментом поиск не сработал")
    elif r["annotation_events"]:
        warn("источники есть, вызовов нет — модель не стала звать инструмент")
    else:
        warn("ни того, ни другого")
    return r


def check_second_step(key: str, first: dict | None, raw: bool) -> None:
    section("B. Второй шаг агентского цикла — приходят ли источники с историей")
    if not first or not first["tool_calls"]:
        warn("первый шаг не дал вызовов — второй шаг проверять не на чем")
        return

    tc = first["tool_calls"][0]
    messages = [
        {"role": "user", "content": APPLIED_PROMPT},
        {
            "role": "assistant",
            "content": first["text"] or None,
            "tool_calls": [
                {
                    "id": "call_1",
                    "type": "function",
                    "function": {"name": tc["name"], "arguments": tc["args"] or "{}"},
                }
            ],
        },
        {
            "role": "tool",
            "tool_call_id": "call_1",
            "content": '{"found": 0, "hint": "В архиве ничего нет, ответь из интернета"}',
        },
    ]

    resp = post(
        {"model": MODEL, "messages": messages, "plugins": WEB_PLUGIN, "tools": [SEARCH_TOOL], "stream": True},
        key,
    )
    if resp is None:
        return

    t0 = time.time()
    r = read_sse(resp, False)
    dt = time.time() - t0

    print(f"  событий: {r['events']}, максимальное: {r['max_event']} байт, {dt:.1f} с")
    print(f"  finish_reason: {r['finish_reason']}, вызовов: {len(r['tool_calls'])}")
    print(f"  текста: {len(r['text'])} символов")
    print(f"  источников: {len(r['urls'])}, события: {r['annotation_events']}")
    for u in r["urls"][:3]:
        print(f"    {u}")
    if r["usage"]:
        print(f"  usage: {json.dumps(r['usage'], ensure_ascii=False)}")

    if r["urls"]:
        ok("на втором шаге источники приходят — один накопитель на весь ход подходит")
    else:
        warn("на втором шаге источников нет — тогда их надо копить с первого шага")


def check_worst_case_size(key: str) -> None:
    section("C. Худший случай размера события с annotations (max_results=8)")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {
                    "role": "user",
                    "content": (
                        "Найди и подробно перескажи инструкцию по сбросу счётчика "
                        "фильтра Xiaomi Mi Air Purifier 3, приведи полные цитаты источников."
                    ),
                }
            ],
            "plugins": [{"id": "web", "max_results": 8}],
            "stream": True,
        },
        key,
    )
    if resp is None:
        return

    r = read_sse(resp, False)
    print(f"  событий: {r['events']}, максимальное: {r['max_event']} байт")
    print(f"  событий с annotations: {len(r['annotation_events'])}, размеры: {r['annotation_events']}")
    print(f"  источников: {len(r['urls'])}")

    limit = 8192
    biggest = max(r["annotation_events"]) if r["annotation_events"] else 0
    if biggest == 0:
        warn("аннотаций не было — размер не измерить")
    elif biggest > limit:
        warn(f"НЕ ВЛЕЗАЕТ: событие {biggest} байт при лимите {limit} — источник потеряется молча")
    elif biggest > limit * 3 // 4:
        warn(f"впритык: {biggest} из {limit} байт — запас меньше 25 %")
    else:
        ok(f"влезает с запасом: {biggest} из {limit} байт")


def check_cost_matters(key: str) -> None:
    section("D. Стоимость: приходит ли cost и сколько стоит вопрос с поиском")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "Кратко: что такое фильтр HEPA?"}],
            "plugins": WEB_PLUGIN,
            "stream": True,
        },
        key,
    )
    if resp is None:
        return

    r = read_sse(resp, False)
    usage = r["usage"] or {}
    print(f"  usage: {json.dumps(usage, ensure_ascii=False)}")
    if "cost" in usage:
        ok(f"cost приходит в usage: {usage['cost']:.4f} — можно показывать траты на устройстве")
    else:
        warn("cost нет — стоимость на устройстве не посчитать")


def check_no_search_no_annotations(key: str) -> None:
    section("E. Вопрос без поиска — приходят ли annotations вообще")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": NO_SEARCH_PROMPT}],
            "plugins": WEB_PLUGIN,
            "stream": True,
        },
        key,
    )
    if resp is None:
        return

    r = read_sse(resp, False)
    print(f"  finish_reason: {r['finish_reason']}, текста: {len(r['text'])}")
    print(f"  событий с annotations: {len(r['annotation_events'])}, источников: {len(r['urls'])}")
    print(f"  ответ: {r['text'][:80]!r}")

    if not r["annotation_events"]:
        ok("поля annotations нет вовсе — прошивка верно отличит «поиска не было»")
    else:
        warn("annotations есть даже без поиска — «ничего не нашлось» не отличить от «не искали»")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", action="store_true", help="дамп сырых чанков SSE")
    args = parser.parse_args()

    key = load_key()
    print(f"Модель: {MODEL}")
    print(f"Ключ: {key[:6]}…{key[-4:]} ({len(key)} символов)")
    t0 = time.time()

    first = check_stream_with_tools(key, args.raw)
    check_second_step(key, first, args.raw)
    check_worst_case_size(key)
    check_cost_matters(key)
    check_no_search_no_annotations(key)

    print(f"\n\033[32mГотово за {time.time() - t0:.1f} с.\033[0m")


if __name__ == "__main__":
    main()
