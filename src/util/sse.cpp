// Разбор SSE: накопитель строк и событий.
//
// Нарочно ничего не знает про JSON: здесь только транспортный уровень
// «байты → события». Разбор содержимого — в util/api_json, чтобы тесты на
// транспорт не падали от изменения формата ответа модели, и наоборот.

#include "util/sse.h"

#include <string.h>

namespace sse {
namespace {

/// Маркер конца потока, который присылает OpenAI-совместимый API.
constexpr const char* kDoneMarker = "[DONE]";

bool equals(const char* a, size_t len, const char* b)
{
    const size_t b_len = strlen(b);
    return len == b_len && memcmp(a, b, len) == 0;
}

}  // namespace

void Parser::reset()
{
    _event_len = 0;
    _event[0] = '\0';
    _event_overflow = false;
    _has_data = false;

    _line_len = 0;
    _line[0] = '\0';
    _line_overflow = false;

    _done = false;
    _dropped = 0;
}

bool Parser::dispatch()
{
    // Ни одного поля `data:` — это разделитель или служебные поля
    // (event:, id:, retry:). Такое событие обработчику не отдаётся.
    if (!_has_data) {
        if (_event_overflow) ++_dropped;
        _event_overflow = false;
        return true;
    }

    const bool is_done = !_event_overflow && equals(_event, _event_len, kDoneMarker);

    if (_event_overflow) {
        // Накопитель переполнился: отдавать нечего, но поток продолжаем
        // читать — это мог быть просто мусор.
        ++_dropped;
    } else if (!is_done && _handler != nullptr) {
        _handler(_event, _event_len, _ctx);
    }

    _event_len = 0;
    _event[0] = '\0';
    _event_overflow = false;
    _has_data = false;

    if (is_done) {
        _done = true;
        return false;
    }
    return true;
}

void Parser::handle_line(const char* line, size_t len)
{
    // Пустая строка завершает событие. Именно здесь выдаётся результат, а не
    // на каждом `data:` — иначе многострочное событие развалилось бы на части.
    if (len == 0) {
        dispatch();
        return;
    }

    // Строка-комментарий (начинается с двоеточия) — heartbeat сервера.
    if (line[0] == ':') return;

    // Поле и значение: `data: значение`. Пробел после двоеточия по формату
    // необязателен и, если есть, в значение не входит.
    size_t colon = 0;
    while (colon < len && line[colon] != ':')
        ++colon;

    const char* value = line + (colon < len ? colon + 1 : len);
    size_t value_len = len - (colon < len ? colon + 1 : len);
    if (value_len > 0 && *value == ' ') {
        ++value;
        --value_len;
    }

    if (colon != 4 || memcmp(line, "data", 4) != 0) return;  // не data:

    _has_data = true;

    // Добавляем перевод строки как разделитель многострочного `data:` —
    // формат SSE требует склеивать строки именно через \n.
    if (_event_len > 0) {
        if (_event_len + 1 >= sizeof(_event)) {
            _event_overflow = true;
            return;
        }
        _event[_event_len++] = '\n';
    }

    if (_event_len + value_len >= sizeof(_event)) {
        _event_overflow = true;
        return;
    }

    memcpy(_event + _event_len, value, value_len);
    _event_len += value_len;
    _event[_event_len] = '\0';
}

bool Parser::feed(const char* data, size_t len)
{
    if (data == nullptr) return !_done;

    for (size_t i = 0; i < len && !_done; ++i) {
        const char ch = data[i];

        if (ch != '\n') {
            // Строка длиннее буфера — значит, пришло что-то не то. Событие
            // будет выброшено, но парсер не должен «залипнуть» на строке.
            if (_line_len + 1 < sizeof(_line)) {
                _line[_line_len++] = ch;
            } else {
                _line_overflow = true;
            }
            continue;
        }

        // \r перед \n — часть разделителя строк в HTTP, в данные не входит.
        if (_line_len > 0 && _line[_line_len - 1] == '\r') --_line_len;

        if (_line_overflow) {
            _line_overflow = false;
            _line_len = 0;
            // Обрывок строки мог быть частью события — выбрасываем его целиком.
            _event_overflow = true;
            continue;
        }

        _line[_line_len] = '\0';
        handle_line(_line, _line_len);
        _line_len = 0;
    }

    return !_done;
}

}  // namespace sse
