// Склейка дельт tool_calls и фильтр DSML. Без Arduino — тестируется на хосте.

#include "util/tool_calls.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

namespace tools {
namespace {

/// Внутренняя разметка DeepSeek. Байты — не литералом: в исходнике он выглядел
/// бы как обычный текст и однажды был бы «причёсан» редактором. Полные
/// вертикальные черты U+FF5C кодируются как EF BD 9C.
///
/// Маркер начинается с '<': в живом ответе разметка выглядит как
/// `<｜｜DSML｜｜ calls>`, и без '<' в чат просочился бы одинокий уголок.
constexpr char kDsmlMarker[] =
    "<"
    "\xEF\xBD\x9C\xEF\xBD\x9C"
    "DSML";
constexpr size_t kDsmlMarkerLen = sizeof(kDsmlMarker) - 1;

/// Сравнение с учётом того, что в буфере может быть меньше байт, чем в маркере:
/// тогда сравниваем доступную часть и не выдаём ничего, пока не ясно.
/// @return 1 — совпало целиком, 0 — не маркер, -1 — сравнивать пока нельзя
int matchPrefix(const char* buf, size_t buf_len)
{
    const size_t n = buf_len < kDsmlMarkerLen ? buf_len : kDsmlMarkerLen;
    if (memcmp(buf, kDsmlMarker, n) != 0) return 0;
    return buf_len >= kDsmlMarkerLen ? 1 : -1;
}

}  // namespace

size_t DsmlFilter::markerLen()
{
    return kDsmlMarkerLen;
}

void DsmlFilter::reset()
{
    _pending_len = 0;
    _triggered = false;
}

size_t DsmlFilter::feed(const char* in, size_t len, char* out, size_t out_cap)
{
    if (out == nullptr || out_cap == 0) return 0;
    size_t written = 0;

    for (size_t i = 0; i < len; ++i) {
        if (_triggered) break;

        // Копим байт в хвосте, пока он может оказаться началом маркера.
        _pending[_pending_len++] = in[i];

        const int m = matchPrefix(_pending, _pending_len);
        if (m == 0) {
            // Это не маркер: отдаём всё накопленное как обычный текст.
            for (size_t k = 0; k < _pending_len && written < out_cap; ++k)
                out[written++] = _pending[k];
            _pending_len = 0;
        } else if (m == 1) {
            // Маркер целиком — дальше текст машинный, не показываем.
            _pending_len = 0;
            _triggered = true;
        }
        // m == -1: пока префикс, ждём следующий байт.
    }

    return written;
}

size_t DsmlFilter::flush(char* out, size_t out_cap)
{
    if (out == nullptr || out_cap == 0) return 0;

    // Хвост, который так и не стал маркером, — законный текст. Именно здесь
    // терялись бы последние байты ответа, если бы фильтр их просто выбрасывал.
    size_t written = 0;
    if (!_triggered) {
        for (size_t k = 0; k < _pending_len && written < out_cap; ++k)
            out[written++] = _pending[k];
    }
    _pending_len = 0;
    return written;
}

// --- Accumulator ------------------------------------------------------------

void Accumulator::reset()
{
    _count = 0;
    _error[0] = '\0';
}

void Accumulator::setError(const char* text)
{
    if (_error[0] == '\0') snprintf(_error, sizeof(_error), "%s", text);
}

bool Accumulator::appendTo(char* dst, size_t cap, size_t& len, const char* src, size_t src_len)
{
    if (len + src_len + 1 > cap) return false;
    memcpy(dst + len, src, src_len);
    len += src_len;
    dst[len] = '\0';
    return true;
}

Call* Accumulator::slotFor(int index)
{
    for (size_t i = 0; i < _count; ++i) {
        if (_calls[i].index == index) return &_calls[i];
    }
    if (_count >= kMaxCallsPerTurn) {
        setError("модель попросила больше инструментов, чем можно за один ход");
        return nullptr;
    }
    Call* c = &_calls[_count++];
    *c = Call{};
    c->index = index;
    return c;
}

bool Accumulator::feed(const char* json, size_t len)
{
    if (json == nullptr || len == 0) return false;

    // Для потокового чанка достаточно 2 КБ: там только огрызок аргументов.
    // Но тот же метод принимает и полный ответ без стриминга, где аргументы
    // целиком, — поэтому запас больше.
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json, len);
    if (err) return false;

    JsonArrayConst choices = doc["choices"].as<JsonArrayConst>();
    if (choices.isNull() || choices.size() == 0) return true;

    JsonObjectConst choice = choices[0].as<JsonObjectConst>();
    JsonArrayConst calls = choice["delta"]["tool_calls"].as<JsonArrayConst>();
    if (calls.isNull()) calls = choice["message"]["tool_calls"].as<JsonArrayConst>();
    if (calls.isNull()) return true;

    int implicit_index = 0;
    for (JsonObjectConst c : calls) {
        // index приходит почти всегда, но если его нет — полагаемся на порядок.
        // Так же поступают OpenAI-совместимые клиенты.
        const int index = c["index"].is<int>() ? c["index"].as<int>() : implicit_index;
        ++implicit_index;

        Call* slot = slotFor(index);
        if (slot == nullptr) return true;  // ошибка уже записана

        // id и name в потоке повторяются целиком (наблюдали в живом прогоне),
        // а arguments — только кусками. Дописывать id как аргументы нельзя:
        // получилось бы «call_1call_1».
        const char* id = c["id"] | "";
        if (id[0] != '\0' && slot->id[0] == '\0') {
            snprintf(slot->id, sizeof(slot->id), "%s", id);
        }

        // Имя тоже приходит один раз, но может быть разбито на части. Склеиваем
        // только то, что ещё не собрано: если имя уже есть и совпадает — это
        // повтор, а не продолжение.
        const char* name = c["function"]["name"] | "";
        if (name[0] != '\0') {
            size_t used = strlen(slot->name);
            if (used == 0) {
                if (!appendTo(slot->name, sizeof(slot->name), used, name, strlen(name))) {
                    setError("имя инструмента не поместилось");
                }
            } else if (strncmp(slot->name, name, used) == 0) {
                // Имя пришло с начала: это повтор или его продолжение одним
                // куском («read_» → «read_doc»). Заменяем целиком, иначе
                // получилось бы «read_read_doc».
                snprintf(slot->name, sizeof(slot->name), "%s", name);
            } else {
                // Имя пришло без начала: это следующий кусок («read_» + «doc»).
                if (!appendTo(slot->name, sizeof(slot->name), used, name, strlen(name))) {
                    setError("имя инструмента не поместилось");
                }
            }
        }

        const char* args = c["function"]["arguments"] | "";
        if (args[0] != '\0') {
            const size_t n = strlen(args);
            if (!appendTo(slot->arguments, kMaxArgsLen, slot->arguments_len, args, n)) {
                setError("аргументы инструмента не поместились в буфер");
            }
        }
    }

    return true;
}

void effective_id(const Call& call, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;
    if (call.id[0] != '\0') {
        snprintf(out, out_len, "%s", call.id);
        return;
    }
    snprintf(out, out_len, "call_%d", call.index);
}

size_t build_calls_json(const Accumulator& calls, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0 || !calls.any()) return 0;

    size_t used = 0;
    out[0] = '\0';

    auto put = [&](const char* s) -> bool {
        const size_t n = strlen(s);
        if (used + n + 1 > out_len) return false;
        memcpy(out + used, s, n);
        used += n;
        out[used] = '\0';
        return true;
    };

    // Экранирование руками, а не ArduinoJson: аргументы уже готовый JSON-текст,
    // и пропустить его через парсер значило бы потерять форматирование и
    // добавить копию на килобайт.
    auto put_escaped = [&](const char* s) -> bool {
        if (!put("\"")) return false;
        for (const char* p = s; *p != '\0'; ++p) {
            char buf[8];
            switch (*p) {
            case '"': snprintf(buf, sizeof(buf), "\\\""); break;
            case '\\': snprintf(buf, sizeof(buf), "\\\\"); break;
            case '\n': snprintf(buf, sizeof(buf), "\\n"); break;
            case '\r': snprintf(buf, sizeof(buf), "\\r"); break;
            case '\t': snprintf(buf, sizeof(buf), "\\t"); break;
            default:
                buf[0] = *p;
                buf[1] = '\0';
                break;
            }
            if (!put(buf)) return false;
        }
        return put("\"");
    };

    if (!put("[")) return 0;
    for (size_t i = 0; i < calls.count(); ++i) {
        const Call& c = calls.at(i);
        if (i > 0 && !put(",")) return 0;
        if (!put("{\"id\":")) return 0;
        // У вызова без id (сервер не прислал) подставляем синтетический:
        // сообщение role=tool обязано на что-то ссылаться, а пустой id сервер
        // отклонит. Мы всё равно сверяем соответствие сами (Р-025).
        char id[Call::kFallbackIdLen];
        effective_id(c, id, sizeof(id));
        if (!put_escaped(id)) return 0;
        if (!put(",\"type\":\"function\",\"function\":{\"name\":")) return 0;
        if (!put_escaped(c.name)) return 0;
        if (!put(",\"arguments\":")) return 0;
        if (!put_escaped(c.arguments_len > 0 ? c.arguments : "{}")) return 0;
        if (!put("}}")) return 0;
    }
    if (!put("]")) return 0;
    return used;
}

}  // namespace tools
