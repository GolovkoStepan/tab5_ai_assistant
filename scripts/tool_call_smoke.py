#!/usr/bin/env python3
"""Живая разведка tool calling у RouterAI: то, что нельзя узнать из документации.

От ответов этого скрипта зависит код агентского цикла, поэтому спрашиваем
именно то, что нужно прошивке:

  * принимает ли модель `tools` вообще — и не отвечает ли 400 на нашу схему;
  * как выглядит `delta.tool_calls` в СТРИМИНГЕ: приходит ли `index`, делятся ли
    `arguments` на куски, повторяется ли `id` в каждом чанке;
  * каким становится `finish_reason`, когда модель просит инструмент;
  * в каком виде надо вернуть результат: эхо `tool_calls` в assistant-сообщении
    плюс `role: "tool"` — или достаточно `role: "tool"`;
  * приходит ли `usage` в стриминге при вызове инструмента;
  * вызывает ли модель несколько инструментов за один ход (это определяет,
    нужен ли нам цикл по `tool_calls[]` или достаточно одного);
  * что приходит, если аргументы битые или инструмент неизвестен.

Ключ берётся из key.txt в корне репозитория или из переменной окружения
ROUTERAI_API_KEY. Файл в .gitignore, в лог ключ не печатается.

Стоимость: несколько коротких запросов, копейки.

Запуск:
    ./scripts/tool_call_smoke.py           # все проверки
    ./scripts/tool_call_smoke.py --raw     # плюс сырые чанки SSE
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

# Два инструмента с разными типами аргументов: строка и число с умолчанием.
# Схема нарочно как в прошивке: описание по-русски (модель у нас русскоязычная),
# обязательные поля перечислены.
TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "read_doc",
            "description": "Прочитать сохранённый документ по пути относительно /notes.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Путь вида home/air-purifier-filter-reset.md",
                    }
                },
                "required": ["path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "search_docs",
            "description": "Найти документы в архиве по ключевым словам.",
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


def read_sse(resp, raw: bool, dump: list | None = None) -> dict:
    """Прочитать поток SSE и собрать из него то, что нужно для выводов.

    Возвращает: тексты дельт, собранные tool_calls по индексу, finish_reason,
    usage и число чанков, в которых повторялся `id`.
    """
    out = {
        "content": "",
        "reasoning": "",
        "tool_calls": {},  # index -> {"id", "name", "arguments"}
        "finish_reason": None,
        "usage": None,
        "chunks": 0,
        "arg_chunks": 0,  # сколько раз arguments приходил по частям
        "id_repeats": 0,  # сколько чанков повторяли id уже известного вызова
        "name_chunks": 0,
    }

    while True:
        line = resp.readline()
        if not line:
            break
        text = line.decode("utf-8", "replace").rstrip("\n")
        if raw:
            print(f"  {text}")
        if dump is not None:
            dump.append(text)

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
        if chunk.get("usage"):
            out["usage"] = chunk["usage"]

        for choice in chunk.get("choices") or []:
            if choice.get("finish_reason"):
                out["finish_reason"] = choice["finish_reason"]
            delta = choice.get("delta") or {}
            if delta.get("content"):
                out["content"] += delta["content"]
            if delta.get("reasoning"):
                out["reasoning"] += delta["reasoning"]

            # Главное ради чего всё затевалось: формат tool_calls в стриминге.
            for call in delta.get("tool_calls") or []:
                idx = call.get("index", 0)
                slot = out["tool_calls"].setdefault(idx, {"id": None, "name": "", "arguments": ""})
                if call.get("id"):
                    if slot["id"] is not None:
                        out["id_repeats"] += 1
                    slot["id"] = call["id"]
                fn = call.get("function") or {}
                if fn.get("name"):
                    slot["name"] += fn["name"]
                    out["name_chunks"] += 1
                if fn.get("arguments"):
                    slot["arguments"] += fn["arguments"]
                    out["arg_chunks"] += 1
    return out


def show_calls(result: dict) -> None:
    calls = result["tool_calls"]
    if not calls:
        fail("tool_calls в потоке не было")
        return
    for idx in sorted(calls):
        slot = calls[idx]
        print(f"  [{idx}] id={slot['id']} name={slot['name']!r}")
        print(f"      arguments: {slot['arguments']!r}")
        try:
            parsed = json.loads(slot["arguments"]) if slot["arguments"] else None
            print(f"      разобрались как JSON: {parsed!r}")
        except json.JSONDecodeError as e:
            fail(f"      arguments — НЕ валидный JSON: {e}")


def check_tools_accepted(key: str) -> bool:
    section("1. Принимает ли модель массив tools")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {"role": "user", "content": "Прочитай документ home/air-purifier.md"},
            ],
            "tools": TOOLS,
            "temperature": 0,
            "stream": False,
        },
        key,
        stream=False,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    body = resp.read().decode("utf-8", "replace")
    if status != 200:
        fail(f"HTTP {status}: {body[:400]}")
        return False

    data = json.loads(body)
    message = data["choices"][0]["message"]
    print(f"  finish_reason: {data['choices'][0].get('finish_reason')}")
    calls = message.get("tool_calls") or []
    if calls:
        ok(f"модель запросила инструмент (без стриминга), вызовов: {len(calls)}")
        for c in calls:
            print(f"  type={c.get('type')} id={c.get('id')} name={c['function']['name']!r}")
            print(f"    arguments: {c['function']['arguments']!r}")
    else:
        warn("модель ответила текстом вместо вызова")
        print(f"  текст: {(message.get('content') or '')[:200]!r}")
    return True


def check_stream_call(key: str, raw: bool) -> dict | None:
    section("2. tool_calls в стриминге: формат и фрагментация")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {"role": "user", "content": "Прочитай документ home/air-purifier.md, чтобы вспомнить, как сбросить ошибку"},
            ],
            "tools": TOOLS,
            "temperature": 0,
            "stream": True,
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        fail(f"HTTP {status}: {resp.read().decode('utf-8', 'replace')[:300]}")
        return None

    result = read_sse(resp, raw)
    print(f"  чанков: {result['chunks']}, чанков с arguments: {result['arg_chunks']}")
    print(f"  finish_reason: {result['finish_reason']!r}")
    show_calls(result)

    if result["finish_reason"] == "tool_calls":
        ok("finish_reason == 'tool_calls' — по нему и узнаём, что нужен инструмент")
    elif result["tool_calls"]:
        warn(f"finish_reason = {result['finish_reason']!r}, а не 'tool_calls' — ориентироваться на набор tool_calls")
    if result["arg_chunks"] > 1:
        ok(f"arguments делятся на {result['arg_chunks']} кусков — склеивать обязательно")
    elif result["arg_chunks"] == 1:
        print("  arguments пришли одним куском (склеивание всё равно нужно: это не гарантия)")
    if result["id_repeats"]:
        print(f"  id повторялся в {result['id_repeats']} чанках — учитывать при склейке")
    if result["usage"]:
        ok(f"usage при вызове инструмента приходит: {json.dumps(result['usage'], ensure_ascii=False)}")
    else:
        warn("usage при вызове инструмента НЕ пришёл")
    if result["reasoning"]:
        print(f"  рассуждения были ({len(result['reasoning'])} символов)")
    return result


def check_tool_result_roundtrip(key: str, first: dict | None) -> None:
    section("3. Возврат результата инструмента (второй ход)")
    if not first or not first["tool_calls"]:
        warn("нет первого вызова — пропускаем")
        return

    idx = sorted(first["tool_calls"])[0]
    call = first["tool_calls"][idx]
    tool_content = (
        "# Сброс ошибки очистителя\n\n1. Выключите прибор.\n"
        "2. Зажмите кнопку питания на 5 секунд.\n3. Фильтр меняется раз в 6 месяцев."
    )

    # Ровно тот вид, который понадобится прошивке: эхо вызова + ответ инструмента.
    messages = [
        {"role": "user", "content": "Прочитай документ home/air-purifier.md, чтобы вспомнить, как сбросить ошибку"},
        {
            "role": "assistant",
            "content": None,
            "tool_calls": [
                {
                    "id": call["id"],
                    "type": "function",
                    "function": {"name": call["name"], "arguments": call["arguments"]},
                }
            ],
        },
        {"role": "tool", "tool_call_id": call["id"], "content": tool_content},
    ]

    resp = post(
        {
            "model": MODEL,
            "messages": messages,
            "tools": TOOLS,
            "temperature": 0,
            "stream": True,
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        body = resp.read().decode("utf-8", "replace")
        fail(f"HTTP {status}: {body[:400]}")
        return

    result = read_sse(resp, False)
    print(f"  finish_reason: {result['finish_reason']!r}")
    print(f"  ответ: {result['content'][:300]!r}")
    if result["content"]:
        ok("модель приняла результат инструмента и ответила текстом")
    else:
        warn("текста нет — возможно, попросила ещё инструмент")
        show_calls(result)


def check_parallel(key: str) -> None:
    section("4. Несколько инструментов за один ход")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {
                    "role": "user",
                    "content": "Найди в архиве всё про фильтр и прочитай home/air-purifier.md",
                },
            ],
            "tools": TOOLS,
            "temperature": 0,
            "stream": True,
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        fail(f"HTTP {status}: {resp.read().decode('utf-8', 'replace')[:300]}")
        return

    result = read_sse(resp, False)
    n = len(result["tool_calls"])
    print(f"  вызовов за ход: {n}")
    show_calls(result)
    if n > 1:
        ok("модель умеет просить несколько инструментов сразу — цикл нужен по всем")
    elif n == 1:
        print("  пока по одному вызову за ход (но полагаться на это нельзя)")


def check_tools_must_be_present(key: str, first: dict | None) -> None:
    section("5. Обязательно ли передавать tools в КАЖДОМ запросе цикла")
    if not first or not first["tool_calls"]:
        warn("нет первого вызова — пропускаем")
        return

    idx = sorted(first["tool_calls"])[0]
    call = first["tool_calls"][idx]
    messages = [
        {"role": "user", "content": "Прочитай home/air-purifier.md, чтобы вспомнить, как сбросить ошибку"},
        {
            "role": "assistant",
            "content": None,
            "tool_calls": [
                {
                    "id": call["id"],
                    "type": "function",
                    "function": {"name": call["name"], "arguments": call["arguments"]},
                }
            ],
        },
        {"role": "tool", "tool_call_id": call["id"], "content": "# Сброс\n1. Выключите.\n2. Держите кнопку 5 с."},
    ]

    # Поведение НЕдетерминированное: на одних и тех же данных без tools модель
    # иногда отвечает нормально, а иногда печатает DSML прямо в текст. Одного
    # прогона мало для вывода — делаем несколько.
    stats = {}
    for label, extra in (("с tools", {"tools": TOOLS}), ("без tools", {})):
        dsml = 0
        runs = 3
        for _ in range(runs):
            resp = post(
                {"model": MODEL, "messages": messages, "temperature": 0.3, "stream": True, **extra},
                key,
                stream=True,
            )
            status = getattr(resp, "status", None) or resp.getcode()
            if status != 200:
                fail(f"{label}: HTTP {status}")
                break
            result = read_sse(resp, False)
            if "DSML" in result["content"]:
                dsml += 1
                if dsml == 1:
                    print(f"    пример: {result['content'][:110]!r}")
        stats[label] = dsml
        print(f"  {label}: DSML в тексте в {dsml} из {runs} прогонов")

    if stats.get("с tools", 0) == 0 and stats.get("без tools", 0) > 0:
        ok("подтверждено: без tools модель иногда печатает DSML в текст")
        print("    → tools обязательны в каждом запросе цикла")
        print("    → и всё равно нужен фильтр на DSML как страховка: поведение недетерминированное")
    elif stats.get("без tools", 0) == 0:
        warn("в этот раз DSML не воспроизвёлся — риск недетерминированный, но подтверждался раньше")
        print("    → tools передавать всегда, фильтр на DSML держать")
    else:
        warn(f"DSML и с tools ({stats.get('с tools')}) — нужен фильтр в любом случае")


def check_partial_tool_results(key: str) -> None:
    section("6. Ответ на ЧАСТЬ вызовов (модель попросила два — отвечаем одним)")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "Найди про фильтр и прочитай home/air-purifier.md"}],
            "tools": TOOLS,
            "temperature": 0,
            "stream": True,
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        fail(f"HTTP {status}")
        return
    result = read_sse(resp, False)
    calls = [result["tool_calls"][i] for i in sorted(result["tool_calls"])]
    if len(calls) < 2:
        warn(f"модель попросила {len(calls)} инструмент(ов) — проверить нечего")
        return

    messages = [
        {"role": "user", "content": "Найди про фильтр и прочитай home/air-purifier.md"},
        {
            "role": "assistant",
            "content": None,
            "tool_calls": [
                {
                    "id": c["id"],
                    "type": "function",
                    "function": {"name": c["name"], "arguments": c["arguments"]},
                }
                for c in calls
            ],
        },
        # Отвечаем только на первый.
        {"role": "tool", "tool_call_id": calls[0]["id"], "content": "результат"},
    ]
    resp = post(
        {"model": MODEL, "messages": messages, "tools": TOOLS, "temperature": 0, "stream": True},
        key,
        stream=True,
    )
    if (getattr(resp, "status", None) or resp.getcode()) != 200:
        fail("HTTP не 200")
        return
    result = read_sse(resp, False)
    print(f"  finish_reason={result['finish_reason']!r}, ответ: {result['content'][:100]!r}")
    if result["content"]:
        warn("сервер принял неполный набор ответов — но полагаться на это нельзя")


def check_unknown_tool_result(key: str) -> None:
    section("7. Ошибка инструмента как текст — модель её понимает")
    resp = post(
        {
            "model": MODEL,
            "messages": [
                {"role": "user", "content": "Прочитай home/x.md"},
                {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": "call_x",
                            "type": "function",
                            "function": {"name": "read_doc", "arguments": '{"path":"home/x.md"}'},
                        }
                    ],
                },
                {"role": "tool", "tool_call_id": "call_x", "content": "Ошибка: файл home/x.md не найден"},
            ],
            "tools": TOOLS,
            "temperature": 0,
            "stream": True,
        },
        key,
        stream=True,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    if status != 200:
        body = resp.read().decode("utf-8", "replace")
        fail(f"HTTP {status}: {body[:400]}")
        return
    result = read_sse(resp, False)
    print(f"  finish_reason: {result['finish_reason']!r}")
    print(f"  ответ: {result['content'][:300]!r}")
    if result["content"]:
        ok("ошибку инструмента модель принимает текстом и объясняет пользователю")
    else:
        warn("модель промолчала на ошибку инструмента")
        show_calls(result)


def check_ascii_path_hint(key: str) -> None:
    section("8. Соблюдает ли модель ASCII-слаг, если это написано в описании")
    tools = [
        {
            "type": "function",
            "function": {
                "name": "write_doc",
                "description": (
                    "Сохранить документ в архив. Путь — только латиница, цифры, "
                    "дефис и слэш, вида home/air-filter.md"
                ),
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {
                            "type": "string",
                            "description": "ASCII-слаг, например home/air-purifier-filter.md",
                        },
                        "title": {"type": "string"},
                        "content": {"type": "string"},
                    },
                    "required": ["path", "title", "content"],
                },
            },
        }
    ]

    messages = [{"role": "user", "content": "Сохрани заметку про замену фильтра очистителя воздуха"}]
    resp = post(
        {"model": MODEL, "messages": messages, "tools": tools, "temperature": 0, "stream": True},
        key,
        stream=True,
    )
    if (getattr(resp, "status", None) or resp.getcode()) != 200:
        fail("HTTP не 200")
        return
    result = read_sse(resp, False)
    if not result["tool_calls"]:
        warn("вызова не было")
        return
    call = result["tool_calls"][sorted(result["tool_calls"])[0]]
    try:
        path = json.loads(call["arguments"]).get("path", "")
    except json.JSONDecodeError:
        fail("arguments не JSON")
        return
    print(f"  путь от модели: {path!r}")
    if path.isascii():
        ok("модель соблюдает ASCII-слаг из описания — грубый фильтр не нужен")
    else:
        warn("модель вернула кириллицу в пути — валидация с подсказкой обязательна")

    # Теперь то же, но с ошибкой валидации в ответ: исправляется ли модель.
    messages += [
        {
            "role": "assistant",
            "content": None,
            "tool_calls": [
                {
                    "id": call["id"],
                    "type": "function",
                    "function": {"name": call["name"], "arguments": call["arguments"]},
                }
            ],
        },
        {
            "role": "tool",
            "tool_call_id": call["id"],
            "content": (
                "Ошибка: путь должен содержать только латиницу, цифры, дефис, "
                "подчёркивание и слэш. Пример правильного: home/air-purifier-filter.md"
            ),
        },
    ]
    resp = post(
        {"model": MODEL, "messages": messages, "tools": tools, "temperature": 0, "stream": True},
        key,
        stream=True,
    )
    if (getattr(resp, "status", None) or resp.getcode()) != 200:
        fail("HTTP не 200 на повторном ходе")
        return
    result = read_sse(resp, False)
    if result["tool_calls"]:
        call = result["tool_calls"][sorted(result["tool_calls"])[0]]
        try:
            path2 = json.loads(call["arguments"]).get("path", "")
        except json.JSONDecodeError:
            path2 = "?"
        print(f"  после ошибки: {path2!r}")
        if path2.isascii():
            ok("модель исправляет путь по тексту ошибки — подсказку в ошибке писать стоит")
        else:
            warn("модель не исправилась — нужен перебор/навязанный путь")
    else:
        print(f"  модель ответила текстом: {result['content'][:150]!r}")


def check_bad_schema(key: str) -> None:
    section("9. Невалидная схема инструмента — как ругается сервер")
    resp = post(
        {
            "model": MODEL,
            "messages": [{"role": "user", "content": "1"}],
            "tools": [{"type": "function", "function": {"name": "broken"}}],  # нет parameters
            "stream": False,
        },
        key,
        stream=False,
    )
    status = getattr(resp, "status", None) or resp.getcode()
    body = resp.read().decode("utf-8", "replace")
    print(f"  HTTP {status}: {body[:300]}")
    if status == 200:
        warn("схему без parameters сервер принял — валидировать самим")
    else:
        ok(f"невалидную схему сервер отверг кодом {status}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", action="store_true", help="дамп сырого потока SSE")
    args = parser.parse_args()

    key = load_key()
    print(f"Модель: {MODEL}")
    print(f"Ключ: {key[:6]}…{key[-4:]} ({len(key)} символов)")  # не целиком
    t0 = time.time()

    if not check_tools_accepted(key):
        print("\n\033[31mДальше проверять нечего: tools не принимаются.\033[0m")
        return

    first = check_stream_call(key, args.raw)
    check_tool_result_roundtrip(key, first)
    check_parallel(key)
    check_tools_must_be_present(key, first)
    check_partial_tool_results(key)
    check_unknown_tool_result(key)
    check_ascii_path_hint(key)
    check_bad_schema(key)

    print(f"\n\033[32mГотово за {time.time() - t0:.1f} с.\033[0m")


if __name__ == "__main__":
    main()
