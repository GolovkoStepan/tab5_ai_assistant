// Раскладка текста на строки экрана: реализация чистых функций.
//
// Всё считается по байтам UTF-8, а ширина строки — в СИМВОЛАХ. Разница
// принципиальная: «ш» занимает два байта и один столбец экрана, и если считать
// ширину в байтах, то строка из кириллицы ломалась бы вдвое раньше латинской,
// а каретка уезжала бы вправо от символа.

#include "util/text_layout.h"

#include <string.h>

namespace textlayout {
namespace {

/// Длина символа UTF-8 по ведущему байту (никогда 0 — на битом байте 1).
size_t char_len(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

/// Сколько байт занимают `chars` символов, начиная с pos, но не дальше конца
/// строки ('\n') и не дальше len.
size_t advance_chars(const char* text, size_t len, size_t pos, size_t chars)
{
    size_t p = pos;
    for (size_t i = 0; i < chars && p < len; ++i) {
        if (text[p] == '\n') break;
        const size_t step = char_len(static_cast<unsigned char>(text[p]));
        p += step < len - p ? step : len - p;
    }
    return p;
}

/// Сколько строк занимает текст при данной ширине.
size_t rows_count(const char* text, size_t len, size_t width)
{
    if (text == nullptr || len == 0) return 1;

    size_t rows = 1;
    size_t pos = 0;
    size_t col = 0;

    while (pos < len) {
        if (text[pos] == '\n') {
            ++pos;
            ++rows;
            col = 0;
            continue;
        }
        if (col == width) {
            col = 0;
            ++rows;
            continue;
        }
        const size_t step = char_len(static_cast<unsigned char>(text[pos]));
        pos += step < len - pos ? step : len - pos;
        ++col;
    }
    return rows;
}

}  // namespace

size_t layout(const char* text, size_t len, size_t width, uint32_t* offsets, size_t max_rows)
{
    if (offsets == nullptr || max_rows == 0) return 0;
    if (width == 0) width = 1;

    offsets[0] = 0;
    if (text == nullptr || len == 0) {
        offsets[1] = 0;
        return 1;
    }

    size_t rows = 1;
    size_t pos = 0;
    size_t col = 0;

    while (pos < len) {
        if (text[pos] == '\n') {
            ++pos;
            // Строчный предел достигнут: новых начал не записываем, иначе
            // вылезли бы за offsets. Остаток текста в окно всё равно не попал.
            if (rows >= max_rows) break;
            col = 0;
            offsets[rows++] = static_cast<uint32_t>(pos);
            continue;
        }
        // Перенос по ширине, а не по словам: каретке нужно однозначное
        // соответствие «символ → строка/столбец», а перенос по словам сделал бы
        // его зависящим от всего абзаца.
        if (col == width) {
            if (rows >= max_rows) break;
            col = 0;
            offsets[rows++] = static_cast<uint32_t>(pos);
            continue;
        }
        const size_t step = char_len(static_cast<unsigned char>(text[pos]));
        pos += step < len - pos ? step : len - pos;
        ++col;
    }

    // Конец последней строки — это и есть место, где началась бы следующая.
    offsets[rows] = static_cast<uint32_t>(pos);
    return rows;
}

void caret_row_col(const char* text, size_t len, size_t caret, size_t width, size_t* out_row, size_t* out_col)
{
    size_t row = 0;
    size_t col = 0;

    if (text != nullptr && len > 0) {
        if (width == 0) width = 1;
        if (caret > len) caret = len;

        size_t pos = 0;
        while (pos < caret) {
            if (text[pos] == '\n') {
                ++pos;
                ++row;
                col = 0;
                continue;
            }
            if (col == width) {
                col = 0;
                ++row;
                continue;
            }
            const size_t step = char_len(static_cast<unsigned char>(text[pos]));
            if (pos + step > caret) break;  // каретка внутри символа — не наш случай
            pos += step < len - pos ? step : len - pos;
            ++col;
        }

        // Позиция ровно на границе переноса принадлежит СЛЕДУЮЩЕЙ строке — так
        // же, как её видит layout(). Но только если за границей ещё есть текст:
        // каретка в конце десятисимвольной строки остаётся на этой строке.
        if (col == width && pos < len) {
            ++row;
            col = 0;
        }
    }

    if (out_row != nullptr) *out_row = row;
    if (out_col != nullptr) *out_col = col;
}

size_t caret_pos(const char* text, size_t len, size_t row, size_t col, size_t width)
{
    if (text == nullptr || len == 0) return 0;
    if (width == 0) width = 1;

    // Идём по строкам от начала: начало следующей определяется тем же
    // правилом, что и в layout(), иначе «строка 5» значила бы разное в
    // отрисовке и в навигации.
    size_t pos = 0;
    for (size_t r = 0; r < row && pos < len; ++r) {
        if (text[pos] == '\n') {
            ++pos;
            continue;
        }
        pos = advance_chars(text, len, pos, width);
        if (pos < len && text[pos] == '\n') ++pos;
    }

    if (pos >= len) return len;  // строки за концом текста

    return advance_chars(text, len, pos, col);
}

size_t move_vertical(const char* text, size_t len, size_t caret, size_t width, int delta, size_t& desired_col)
{
    if (text == nullptr || delta == 0) return caret;
    if (width == 0) width = 1;

    size_t row = 0;
    size_t col = 0;
    caret_row_col(text, len, caret, width, &row, &col);

    // Ноль в desired_col значит «ещё не задан»: при первом движении стремимся к
    // текущему столбцу. Иначе первое нажатие «вниз» уводило бы каретку в начало
    // строки.
    if (desired_col == 0) desired_col = col;

    const long target = static_cast<long>(row) + delta;
    if (target < 0) return caret;

    const size_t rows = rows_count(text, len, width);
    if (static_cast<size_t>(target) >= rows) return caret;

    // Столбец, к которому стремимся, приходит снаружи (desired_col): при
    // движении по коротким строкам каретка сжимается к их концу, и без этой
    // памяти она уже не вернулась бы на прежнее место.
    return caret_pos(text, len, static_cast<size_t>(target), desired_col, width);
}

}  // namespace textlayout
