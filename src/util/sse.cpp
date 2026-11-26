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

    _field_len = 0;
    _field[0] = '\0';
    _in_value = false;
    _skip_space = false;
    _line_is_data = false;
    _line_has_bytes = false;

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

/// Принять байт строки. Значение `data:` уходит прямо в накопитель события;
/// имя поля собирается отдельно (не длиннее kMaxFieldBytes).
void Parser::put_char(char ch)
{
    if (!_in_value) {
        // Имя поля уже не влезло в буфер: остаток имени и значение не нужны.
        // Двоеточие просто переводит строку в режим «значение», и дальше
        // _line_is_data (false) отсекает всё остальное. Иначе хвост длинного
        // имени начал бы собираться заново и мог случайно совпасть с `data`.
        if (_skip_field) {
            if (ch == ':') _in_value = true;
            return;
        }

        if (ch == ':') {
            _in_value = true;
            _skip_space = true;
            _field[_field_len] = '\0';
            _line_is_data = (_field_len == 4 && memcmp(_field, "data", 4) == 0);
            if (_line_is_data) {
                _has_data = true;
                // Разделитель между строками многострочного data:.
                if (_event_len > 0) {
                    if (_event_len + 1 >= sizeof(_event)) {
                        _event_overflow = true;
                        return;
                    }
                    _event[_event_len++] = '\n';
                }
            }
            return;
        }

        if (_field_len + 1 < sizeof(_field)) {
            _field[_field_len++] = ch;
        } else {
            _field_len = 0;
            _field[0] = '\0';
            _skip_field = true;
        }
        return;
    }

    if (_skip_field) return;

    // Двоеточие встречено. Один пробел после него — разделитель, не данные.
    if (_skip_space) {
        _skip_space = false;
        if (ch == ' ') return;
    }

    if (!_line_is_data) return;  // значения служебных полей не нужны

    if (_event_len + 1 >= sizeof(_event)) {
        _event_overflow = true;
        return;
    }
    _event[_event_len++] = ch;
    _event[_event_len] = '\0';
}

/// Завершить строку: пустая строка выдаёт накопленное событие.
void Parser::end_line()
{
    // Пустая строка (в том числе после `\r`) — конец события.
    if (!_line_has_bytes) {
        dispatch();
    }

    // Следующая строка начинается с чистого листа.
    _field_len = 0;
    _field[0] = '\0';
    _in_value = false;
    _skip_space = false;
    _skip_field = false;
    _line_is_data = false;
    _line_has_bytes = false;
}

bool Parser::feed(const char* data, size_t len)
{
    if (data == nullptr) return !_done;

    for (size_t i = 0; i < len && !_done; ++i) {
        const char ch = data[i];

        if (ch != '\n') {
            // \r перед \n — часть разделителя строк в HTTP, в данные не входит.
            // Встретить его можно только в конце строки, поэтому запоминаем
            // факт байта, а сам \r не пишем.
            if (ch == '\r') {
                continue;
            }
            _line_has_bytes = true;
            put_char(ch);
            continue;
        }

        end_line();
    }

    return !_done;
}

}  // namespace sse
