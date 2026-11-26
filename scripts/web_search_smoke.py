#!/usr/bin/env python3
"""Живая разведка веб-поиска RouterAI: то, что нельзя узнать из документации.

От ответов этого скрипта зависит вся архитектура v0.4.0, поэтому спрашиваем
именно то, что нужно прошивке:

  * принимает ли API поля `plugins` вообще — и не отвечает ли 400 на наш вид
    (`[{"id": "web"}]`);
  * **работает ли плагин вместе с `tools`** — это главный риск плана
    («плагин web не работает вместе с tool calling», вероятность средняя).
    От ответа зависит, один запрос делает прошивка или два раздельных;
  * в каком виде приходят источники: `annotations` в сообщении, отдельное поле
    `citations`, что-то ещё — от этого зависит парсинг и показ ссылок в чате;
  * приходит ли `usage` при поиске (влияет на счётчик токенов);
  * есть ли у плагина параметры (число результатов, домены) или он беcпараметровый;
  * как это выглядит в СТРИМИНГЕ — прошивка стримит, нестриминговый ответ нам
    не подходит;
  * что приходит, когда поиск ничего не нашёл, и что — на неизвестный id плагина
    (текст ошибки часто перечисляет допустимые значения — это дешевле доков).

Ключ берётся из key.txt в корне репозитория или из переменной окружения
ROUTERAI_API_KEY. Файл в .gitignore, в лог ключ не печатается.

Стоимость: несколько коротких запросов, копейки.

Запуск:
    ./scripts/web_search_smoke.py           # все проверки
    ./scripts/web_search_smoke.py --raw     # плюс сырые чанки SSE
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

# Вопрос, на который модель без интернета ответить не может: ответ меняется
# от недели к неделе, а «знания» модели кончаются раньше.
SEARCH_PROMPT = (
    "Какая сейчас погода в Москве? Ответь одной строкой и укажи источник."
)

# Второй вопрос — прикладной, ровно из нашего сценария.
APPLIED_PROMPT = (
    "Очиститель воздуха просит заменить фильтр, я забыл, как сбросить ошибку "
    "на Xiaomi Mi Air Purifier 3. Найди в интернете и ответь по шагам."
)

WEB_PLUGIN = [{"id": "web"}]

# Инструмент ровно в том виде, в каком его получит модель в прошивке.
TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "search_docs",
            "description": "Найти документы в личном архиве по ключевым словам.",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "Что искать"},
                    "limit": {
                        "type": "integer",
                        "description": "Сколько результатов вернуть",
                        "default": 5,
                    },
                },
                "required": ["query"],
            },
        },
    },
]


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
        return urllib.request.urlopen(req, timeout=180)
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


def status_of(resp) -> int:
    return getattr(resp, "status", None) or resp.getcode()


def error_body(resp) -> str:
    try:
        return resp.read().decode("utf-8", "replace")
    except Exception:
        return ""


# --- Разбор источников ------------------------------------------------------
#
# Мы не знаем заранее, как называются поля, поэтому ищем их рекурсивно по
# всему ответу. Задача — не «правильно распарсить», а узнать имена полей.


def find_keys(node, wanted: set[str], path: str = "") -> list[tuple[str, object]]:
    found: list[tuple[str, object]] = []
    if isinstance(node, dict):
        for k, v in node.items():
            here = f"{path}.{k}" if path else k
            if k in wanted:
                found.append((here, v))
            found.extend(find_keys(v, wanted, here))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            found.extend(find_keys(v, wanted, f"{path}[{i}]"))
    return found


def describe_unknown_source_fields(data: dict) -> None:
    """Перечислить подозрительные на «источники» поля ответа."""
    wanted = {
        "annotations",
        "citations",
        "sources",
        "references",
        "links",
        "url",
        "url_citation",
        "search_results",
        "plugin",
        "plugins",
    }
    hits = find_keys(data, wanted)
    if not hits:
        warn("полей со ссылками в ответе нет — источники приходят иначе или не приходят")
        return
    for path, value in hits:
        preview = json.dumps(value, ensure_ascii=False) if not isinstance(value, str) else value
        ok(f"поле {path}: {str(preview)[:300]}")


def show_message_shape(data: dict) -> None:
    try:
        message = data["choices"][0]["message"]
    except (KeyError, IndexError):
        warn("неожиданная структура ответа")
        print(json.dumps(data, ensure_ascii=False)[:600])
        return
    print(f"  ключи ответа: {sorted(data)}")
    print(f"  ключи message: {sorted(message)}")
    content = message.get("content") or ""
    print(f"  текст: {content[:300]!r}")


def read_sse(resp, raw: bool) -> dict:
    """Прочитать поток SSE: текст, tool_calls, usage и все неожиданные поля."""
    out = {
        "content": "",
        "reasoning": "",
        "tool_calls": {},
        "finish_reason": None,
        "usage": None,
        "chunks": 0,
        "chunk_keys": set(),
        "delta_keys": set(),
        "choice_keys": set(),
        "extra": [],  # (path, value) всего, что похоже на источники
        "annotation_chunks": [],  # [(номер чанка, [(url, есть_индексы)]), ...]
        "urls": [],  # уникальные URL в порядке появления
    }
    wanted = {
        "annotations",
        "citations",
        "sources",
        "references",
        "links",
        "url",
        "url_citation",
        "search_results",
    }

    while True:
        line = resp.readline()
        if not line:
            break
        text = line.decode("utf-8", "replace").rstrip("\n")
        if raw:
            print(f"  {text}")
        stripped = text.strip()
        if not stripped.startswith("data:"):
            continue
        payload = stripped[5:].strip()
        if payload == "[DONE]":
            break
        try:
            chunk = json.loads(payload)
        except json.JSONDecodeError:
            warn(f"чанк не разобрался как JSON: {payload[:120]}")
            continue

        out["chunks"] += 1
        out["chunk_keys"].update(chunk)
        if chunk.get("usage"):
            out["usage"] = chunk["usage"]
        for path, value in find_keys(chunk, wanted):
            out["extra"].append((path, value))

        # Источники: важно, приходят ли они целиком одним чанком или растут.
        for choice in chunk.get("choices") or []:
            ann = (choice.get("delta") or {}).get("annotations")
            if ann:
                batch = []
                for a in ann:
                    cite = (a or {}).get("url_citation") or {}
                    url = cite.get("url")
                    batch.append((url, "start_index" in cite))
                    if url and url not in out["urls"]:
                        out["urls"].append(url)
                out["annotation_chunks"].append((out["chunks"], batch))

        for choice in chunk.get("choices") or []:
            out["choice_keys"].update(choice)
            if choice.get("finish_reason"):
                out["finish_reason"] = choice["finish_reason"]
            delta = choice.get("delta") or {}
            out["delta_keys"].update(delta)
            if delta.get("content"):
                out["content"] += delta["content"]
            if delta.get("reasoning"):
                out["reasoning"] += delta["reasoning"]
            for call in delta.get("tool_calls") or []:
                idx = call.get("index", 0)
                slot = out["tool_calls"].setdefault(
                    idx, {"id": None, "name": "", "arguments": ""}
                )
                if call.get("id"):
                    slot["id"] = call["id"]
                fn = call.get("function") or {}
                if fn.get("name"):
                    slot["name"] += fn["name"]
                if fn.get("arguments"):
                    slot["arguments"] += fn["arguments"]
    return out


# --- Проверки ---------------------------------------------------------------


def check_unknown_plugin(key: str) -> None:
    section("0. Неизвестный id плагина — какие имена допустимы")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "1"}],
            "plugins": [{"id": "__no_such_plugin__"}],
        },
        key,
        stream=False,
    )
    status = status_of(resp)
    body = error_body(resp)
    print(f"  HTTP {status}")
    print(f"  тело: {body[:500]}")
    if status == 200:
        warn("неизвестный плагин молча проигнорирован — проверять имена не на чем")
    else:
        ok("сервер знает про `plugins` и отвергает неизвестный id")


def check_plugin_plain(key: str) -> bool:
    section("1. Принимает ли API плагин web (без стриминга)")
    t0 = time.time()
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": SEARCH_PROMPT}],
            "plugins": WEB_PLUGIN,
            "temperature": 0,
        },
        key,
        stream=False,
    )
    status = status_of(resp)
    body = error_body(resp)
    dt = time.time() - t0
    print(f"  HTTP {status} за {dt:.2f} с")
    if status != 200:
        fail(f"ответ: {body[:500]}")
        return False

    ok("плагин web принят")
    data = json.loads(body)
    show_message_shape(data)
    print(f"  finish_reason: {data['choices'][0].get('finish_reason')}")
    usage = data.get("usage") or {}
    print(f"  usage: {json.dumps(usage, ensure_ascii=False)}")
    describe_unknown_source_fields(data)
    return True


def check_plugin_stream(key: str, raw: bool) -> dict | None:
    section("2. Плагин web в СТРИМИНГЕ — как приходят источники")
    t0 = time.time()
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": APPLIED_PROMPT}],
            "plugins": WEB_PLUGIN,
            "temperature": 0,
            "stream": True,
            "stream_options": {"include_usage": True},
        },
        key,
        stream=True,
    )
    status = status_of(resp)
    if status != 200:
        fail(f"HTTP {status}: {error_body(resp)[:400]}")
        return None

    result = read_sse(resp, raw)
    dt = time.time() - t0
    print(f"  HTTP 200 за {dt:.2f} с, чанков: {result['chunks']}")
    print(f"  ключи чанка: {sorted(result['chunk_keys'])}")
    print(f"  ключи choice: {sorted(result['choice_keys'])}")
    print(f"  ключи delta: {sorted(result['delta_keys'])}")
    print(f"  finish_reason: {result['finish_reason']!r}")
    print(f"  текст ({len(result['content'])} символов): {result['content'][:300]!r}")

    if result["annotation_chunks"]:
        batches = result["annotation_chunks"]
        ok(f"чанков с annotations: {len(batches)}")
        print(f"  порции по {[len(b) for _, b in batches]} источников, чанки {[n for n, _ in batches]}")
        per_batch = [tuple(u for u, _ in b) for _, b in batches]
        if len(batches) == 1:
            ok("все источники пришли ОДНИМ чанком — собирать просто")
        elif all(p == per_batch[0] for p in per_batch):
            ok("один и тот же список повторяется в каждом чанке — брать последний, дедупликация не нужна")
        elif len({u for b in per_batch for u in b}) > len(per_batch[0]):
            warn("список растёт от чанка к чанку — накапливать и дедуплицировать")
        has_index = any(idx for _, b in batches for _, idx in b)
        if has_index:
            print("  есть start_index/end_index — можно показать источник у места в тексте")
        else:
            print("  start_index/end_index нет — привязка к фрагменту текста недоступна")
        print(f"  уникальных URL: {len(result['urls'])}")


        ok(f"полей-источников в стриминге: {len(result['extra'])}")
        shown: set[str] = set()
        for path, value in result["extra"]:
            if path in shown:
                continue
            shown.add(path)
            preview = json.dumps(value, ensure_ascii=False) if not isinstance(value, str) else value
            ok(f"  {path}: {str(preview)[:300]}")
    else:
        warn("в стриминге полей со ссылками не найдено")

    if result["usage"]:
        ok(f"usage при поиске приходит: {json.dumps(result['usage'], ensure_ascii=False)}")
    else:
        warn("usage при поиске НЕ пришёл")
    return result


def check_plugin_with_tools(key: str) -> None:
    """Главный вопрос: дружат ли плагин web и tool calling."""
    section("3. Плагин web ВМЕСТЕ с tools — принимает ли сервер")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {
                    "role": "user",
                    "content": (
                        "Найди в интернете, как сбросить ошибку на Xiaomi Mi Air "
                        "Purifier 3, и проверь, нет ли такой заметки в моём архиве"
                    ),
                }
            ],
            "plugins": WEB_PLUGIN,
            "tools": TOOLS,
            "tool_choice": "auto",
            "temperature": 0,
            "stream": False,
        },
        key,
        stream=False,
    )
    status = status_of(resp)
    body = error_body(resp)
    print(f"  HTTP {status}")
    if status != 200:
        fail(f"ответ: {body[:600]}")
        print("    → плагин и tools несовместимы в одном запросе: нужны два раздельных")
        return

    data = json.loads(body)
    message = data["choices"][0]["message"]
    finish = data["choices"][0].get("finish_reason")
    print(f"  finish_reason: {finish}")
    calls = message.get("tool_calls") or []
    content = message.get("content") or ""
    print(f"  tool_calls: {len(calls)}, текста: {len(content)} символов")
    if calls:
        for c in calls:
            print(f"    вызов {c['function']['name']!r}: {c['function']['arguments']!r}")
    if content:
        print(f"  текст: {content[:200]!r}")
    describe_unknown_source_fields(data)

    if calls and content:
        ok("сервер принял plugins+tools, модель и искала, и звала инструмент")
    elif calls:
        ok("plugins+tools приняты, модель выбрала инструмент")
    elif content:
        warn("plugins+tools приняты, но модель не стала звать инструмент")
        print("    → проверить, работает ли поиск в этом же ответе (см. поля выше)")


def check_plugin_options(key: str) -> None:
    """Проверка 4: влияет ли что-нибудь на число источников.

    Проверка 0 показала: сервер молча глотает неизвестные поля (неизвестный id
    плагина дал 200). Значит «HTTP 200» ничего не доказывает — считать надо
    фактическое число ссылок в ответе.
    """
    section("4. Параметры плагина: влияет ли что-то на число источников")
    variants = [
        ("без параметров", WEB_PLUGIN),
        ("max_results=2", [{"id": "web", "max_results": 2}]),
        ("options.max_results=2", [{"id": "web", "options": {"max_results": 2}}]),
        ("options.max_results=8", [{"id": "web", "options": {"max_results": 8}}]),
        ("домены: options.allowed_domains", [{"id": "web", "options": {"allowed_domains": ["habr.com"]}}]),
    ]
    counts: dict[str, int] = {}
    for label, plugins in variants:
        resp = post(
            {
                "model": MODEL,
                "messages": [{"role": "user", "content": SEARCH_PROMPT}],
                "plugins": plugins,
                "temperature": 0,
                "stream": True,
            },
            key,
            stream=True,
        )
        status = status_of(resp)
        if status != 200:
            warn(f"{label}: HTTP {status} — {error_body(resp)[:200]}")
            continue
        result = read_sse(resp, False)
        counts[label] = len(result["urls"])
        print(f"  {label}: источников {len(result['urls'])}, ответ {len(result['content'])} символов")
        if label.startswith("домены") and result["urls"]:
            outside = [u for u in result["urls"] if "habr.com" not in u]
            if outside:
                warn(f"    домен не соблюдён, вне habr.com: {len(outside)} из {len(result['urls'])}")

    base = counts.get("без параметров")
    if base is None:
        return
    same = [label for label, n in counts.items() if label != "без параметров" and n == base]
    diff = [label for label, n in counts.items() if label != "без параметров" and n != base]
    if diff:
        ok(f"на число источников влияет: {diff}")
    if same:
        warn(f"без эффекта (как без параметров): {same}")
        print("    → количество результатов плагином не настраивается;")
        print("      ограничивать показ придётся на своей стороне")


def check_no_result(key: str) -> None:
    section("5. Поиск, который ничего не найдёт")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {
                    "role": "user",
                    "content": (
                        "Найди в интернете расписание автобуса 42-бис в посёлке "
                        "Нижние Кочки на 30 февраля. Если не найдёшь, так и скажи."
                    ),
                }
            ],
            "plugins": WEB_PLUGIN,
            "temperature": 0,
        },
        key,
        stream=False,
    )
    status = status_of(resp)
    body = error_body(resp)
    print(f"  HTTP {status}")
    if status != 200:
        fail(f"ответ: {body[:400]}")
        return
    data = json.loads(body)
    message = data["choices"][0]["message"]
    print(f"  finish_reason: {data['choices'][0].get('finish_reason')}")
    print(f"  текст: {(message.get('content') or '')[:300]!r}")
    describe_unknown_source_fields(data)


def check_bad_plugin_shape(key: str) -> None:
    section("6. Кривая форма plugins — как ругается сервер")
    for label, plugins in (
        ("строка вместо объекта", ["web"]),
        ("plugins как объект", {"id": "web"}),
        ("пустой массив", []),
    ):
        resp = post(
            {
                "model": MODEL,
                "messages": [{"role": "user", "content": "1"}],
                "plugins": plugins,
            },
            key,
            stream=False,
        )
        status = status_of(resp)
        body = error_body(resp)
        print(f"  {label}: HTTP {status} — {body[:200]}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", action="store_true", help="дамп сырых чанков SSE")
    args = parser.parse_args()

    key = load_key()
    print(f"Модель: {MODEL}")
    print(f"Ключ: {key[:6]}…{key[-4:]} ({len(key)} символов)")  # не целиком
    t0 = time.time()

    check_unknown_plugin(key)
    if not check_plugin_plain(key):
        print("\n\033[31mДальше проверять нечего: плагин web не принимается.\033[0m")
        return

    check_plugin_stream(key, args.raw)
    check_plugin_with_tools(key)
    check_plugin_options(key)
    check_no_result(key)
    check_bad_plugin_shape(key)

    print(f"\n\033[32mГотово за {time.time() - t0:.1f} с.\033[0m")


if __name__ == "__main__":
    main()
