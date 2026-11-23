#pragma once

// Раскладка клавиатуры A164: позиция в матрице 5x14 → символ или управляющая
// клавиша. Чистые функции без зависимостей от Arduino — тестируются на хосте.
//
// Почему своя таблица, а не режим Character прошивки клавиатуры: кириллицы в
// прошивке клавиатуры нет, а режим Character отдаёт уже готовую латинскую
// строку, из которой нельзя восстановить физическую позицию клавиши.
// Подробности — docs/ARCHITECTURE.md, раздел «Клавиатура».

#include <stddef.h>
#include <stdint.h>

namespace keymap {

constexpr uint8_t kRows = 5;
constexpr uint8_t kCols = 14;

/// Раскладка ввода. Переключается Ctrl+Space, состояние хранится в NVS.
enum class Layout : uint8_t {
    Latin,
    Russian,
};

/// Клавиши без печатного символа.
enum class Special : uint8_t {
    None,
    Escape,
    Tab,
    Backspace,
    Enter,
    Space,
    Delete,
    Up,
    Down,
    Left,
    Right,
    Sym,  // модификатор Sym — на нём лежит слой символов
    Aa,   // модификатор Aa — Caps Lock / Shift
    Ctrl,
    Alt,
};

/// Управляющая клавиша для позиции матрицы, либо Special::None.
Special special(uint8_t row, uint8_t col);

/// Символ в виде кодовой точки Unicode. 0 — на этой позиции нет символа.
///
/// @param row     строка матрицы, 0..4
/// @param col     столбец матрицы, 0..13
/// @param layout  латиница или ЙЦУКЕН
/// @param shift   зажат Aa (Shift)
/// @param sym     зажат Sym
char32_t codepoint(uint8_t row, uint8_t col, Layout layout, bool shift, bool sym);

/// То же, но результат кодируется в UTF-8.
/// @return число записанных байт без завершающего нуля (0 — нет символа)
size_t char_utf8(uint8_t row, uint8_t col, Layout layout, bool shift, bool sym, char* out,
                 size_t out_len);

/// Кодирует кодовую точку в UTF-8. Используется и в раскладке, и в UI.
/// @return число байт (1..4), 0 если код некорректен или буфер мал
size_t utf8_encode(char32_t cp, char* out, size_t out_len);

}  // namespace keymap
