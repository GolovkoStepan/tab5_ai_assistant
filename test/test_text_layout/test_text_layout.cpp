/*
 * Тесты раскладки текста на строки и связи «каретка ↔ строка/столбец».
 *
 * Ошибки здесь выглядят как «стрелка вниз прыгает на две строки», «каретка
 * уехала в конец файла» или «на экране 90 символов, а на 91-м строка не
 * переносится». На плате такое ищут, листая заметку руками, поэтому проверяем
 * на хосте.
 */

#include <string.h>

#include <unity.h>

#include "util/text_layout.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

uint32_t g_offsets[64];

constexpr size_t kWidth = 10;

size_t rows_of(const char* text)
{
    return textlayout::layout(text, strlen(text), kWidth, g_offsets, 64);
}

size_t row_of(const char* text, size_t caret)
{
    size_t row = 0;
    size_t col = 0;
    textlayout::caret_row_col(text, strlen(text), caret, kWidth, &row, &col);
    return row;
}

size_t col_of(const char* text, size_t caret)
{
    size_t row = 0;
    size_t col = 0;
    textlayout::caret_row_col(text, strlen(text), caret, kWidth, &row, &col);
    return col;
}

}  // namespace

// --- Раскладка --------------------------------------------------------------

void test_layout_empty_is_one_row()
{
    TEST_ASSERT_EQUAL_UINT32(1, textlayout::layout("", 0, kWidth, g_offsets, 64));
    TEST_ASSERT_EQUAL_UINT32(0, g_offsets[0]);
    TEST_ASSERT_EQUAL_UINT32(0, g_offsets[1]);
}

void test_layout_short_text_is_one_row()
{
    TEST_ASSERT_EQUAL_UINT32(1, rows_of("привет"));
    TEST_ASSERT_EQUAL_UINT32(0, g_offsets[0]);
}

void test_layout_wraps_at_width()
{
    // Ширина 10: 12 символов — две строки, перенос после десятого.
    TEST_ASSERT_EQUAL_UINT32(2, rows_of("abcdefghijkl"));
    TEST_ASSERT_EQUAL_UINT32(10, g_offsets[1]);
}

void test_layout_counts_chars_not_bytes()
{
    // «ш» — 2 байта, 1 столбец. 12 ш = 12 столбцов = 2 строки, а в байтах 24.
    TEST_ASSERT_EQUAL_UINT32(2, rows_of("шшшшшшшшшшшш"));
}

void test_layout_explicit_newline()
{
    TEST_ASSERT_EQUAL_UINT32(2, rows_of("abc\ndef"));
    TEST_ASSERT_EQUAL_UINT32(4, g_offsets[1]);
}

void test_layout_newline_at_width_boundary()
{
    // Ровно 10 символов и перевод строки: лишней пустой строки не нужно,
    // но строка после '\n' обязана начаться.
    TEST_ASSERT_EQUAL_UINT32(2, rows_of("abcdefghij\n"));
    TEST_ASSERT_EQUAL_UINT32(11, g_offsets[1]);
}

void test_layout_trailing_newline_adds_empty_row()
{
    TEST_ASSERT_EQUAL_UINT32(2, rows_of("abc\n"));
    TEST_ASSERT_EQUAL_UINT32(4, g_offsets[1]);
}

void test_layout_respects_max_rows()
{
    // Просим две строки из пяти — получаем две и не лезем за offsets.
    const char* text = "aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee";
    const size_t n = textlayout::layout(text, strlen(text), kWidth, g_offsets, 2);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32(10, g_offsets[1]);
    TEST_ASSERT_EQUAL_UINT32(20, g_offsets[2]);
}

void test_layout_zero_width_survives()
{
    // Ширина 0 — не падение и не зацикливание: считаем по одному символу.
    TEST_ASSERT_EQUAL_UINT32(3, textlayout::layout("abc", 3, 0, g_offsets, 64));
}

// --- Каретка -----------------------------------------------------------------

void test_caret_at_start()
{
    TEST_ASSERT_EQUAL_UINT32(0, row_of("abcdefghijkl", 0));
    TEST_ASSERT_EQUAL_UINT32(0, col_of("abcdefghijkl", 0));
}

void test_caret_mid_first_row()
{
    TEST_ASSERT_EQUAL_UINT32(0, row_of("abcdefghijkl", 5));
    TEST_ASSERT_EQUAL_UINT32(5, col_of("abcdefghijkl", 5));
}

void test_caret_on_wrap_boundary_belongs_to_next_row()
{
    // Позиция 10 — ровно граница переноса: она уже на второй строке в начале.
    TEST_ASSERT_EQUAL_UINT32(1, row_of("abcdefghijkl", 10));
    TEST_ASSERT_EQUAL_UINT32(0, col_of("abcdefghijkl", 10));
}

void test_caret_after_newline()
{
    TEST_ASSERT_EQUAL_UINT32(1, row_of("abc\ndef", 4));
    TEST_ASSERT_EQUAL_UINT32(0, col_of("abc\ndef", 4));
}

void test_caret_never_splits_cyrillic()
{
    // Позиция 1 внутри «ш» (2 байта) — не наш случай, но модуль не должен
    // уехать в бесконечность: остаётся на начале символа.
    const char* text = "шшш";
    TEST_ASSERT_EQUAL_UINT32(0, row_of(text, 1));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1, col_of(text, 1));
}

void test_caret_pos_roundtrip()
{
    // Столбцы проверяем только до фактической длины строки: за её концом
    // caret_pos возвращает конец, и столбец там уже другой.
    const char* text = "abcdefghijklmno\npqrstu\nxyz";
    const size_t len = strlen(text);
    const size_t rows = textlayout::layout(text, len, kWidth, g_offsets, 64);
    // «abcdefghijklmno» — две строки по 10, плюс «pqrstu» и «xyz».
    TEST_ASSERT_EQUAL_UINT32(4, rows);

    for (size_t row = 0; row < rows; ++row) {
        const uint32_t from = g_offsets[row];
        const uint32_t to = g_offsets[row + 1];
        // Длина строки в символах — без завершающего '\n'.
        size_t chars = 0;
        for (size_t p = from; p < to; ++p) {
            if (text[p] == '\n') break;
            ++chars;
        }

        // Если строка кончилась переносом по ширине (а не '\n' и не концом
        // текста), каретка за последним символом принадлежит уже следующей
        // строке — как курсор в любом редакторе на границе переноса.
        const bool wrapped = to < len && text[to] != '\n';
        const size_t last_col = wrapped ? chars - 1 : chars;

        for (size_t col = 0; col <= last_col; ++col) {
            const size_t pos = textlayout::caret_pos(text, len, row, col, kWidth);
            TEST_ASSERT_EQUAL_UINT32(row, row_of(text, pos));
            TEST_ASSERT_EQUAL_UINT32(col, col_of(text, pos));
        }
    }
}

void test_caret_pos_clamps_to_line_end()
{
    const char* text = "abc\ndefghijklmnop";
    // Вторая строка, столбец 9 — за концом короткой первой строки не уходим.
    const size_t pos = textlayout::caret_pos(text, strlen(text), 0, 9, kWidth);
    TEST_ASSERT_EQUAL_UINT32(3, pos);  // конец «abc», перед '\n'
}

void test_caret_pos_beyond_text_returns_len()
{
    const char* text = "abc";
    TEST_ASSERT_EQUAL_UINT32(3, textlayout::caret_pos(text, strlen(text), 5, 0, kWidth));
}

// --- Вертикальное движение ---------------------------------------------------

void test_move_down_keeps_column()
{
    const char* text = "abcdefghij\nklmnopqrst";
    size_t desired = 4;
    const size_t pos = textlayout::move_vertical(text, strlen(text), 4, kWidth, 1, desired);
    TEST_ASSERT_EQUAL_UINT32(15, pos);  // 11 + 4
    TEST_ASSERT_EQUAL_UINT32(4, col_of(text, pos));
}

void test_move_up_keeps_column()
{
    const char* text = "abcdefghij\nklmnopqrst";
    size_t desired = 4;
    const size_t pos = textlayout::move_vertical(text, strlen(text), 15, kWidth, -1, desired);
    TEST_ASSERT_EQUAL_UINT32(4, pos);
}

void test_move_down_through_short_line_remembers_column()
{
    const char* text = "a\nbcdefghij";
    size_t desired = 5;
    const size_t pos = textlayout::move_vertical(text, strlen(text), 0, kWidth, 1, desired);
    // Вторая строка длинная — столбец 5 достижим: 2 байта «a\n» + 5.
    TEST_ASSERT_EQUAL_UINT32(7, pos);
    TEST_ASSERT_EQUAL_UINT32(5, col_of(text, pos));

    // Обратно наверх — но там строка из одного символа: каретка в её конец.
    const size_t back = textlayout::move_vertical(text, strlen(text), pos, kWidth, -1, desired);
    TEST_ASSERT_EQUAL_UINT32(1, back);
    TEST_ASSERT_EQUAL_UINT32(1, col_of(text, back));
    TEST_ASSERT_EQUAL_UINT32(5, desired);  // желаемый столбец не потерян
}

void test_move_down_at_last_row_does_nothing()
{
    const char* text = "abc";
    size_t desired = 0;
    TEST_ASSERT_EQUAL_UINT32(3, textlayout::move_vertical(text, strlen(text), 3, kWidth, 1, desired));
}

void test_move_up_at_first_row_does_nothing()
{
    const char* text = "abc";
    size_t desired = 2;
    TEST_ASSERT_EQUAL_UINT32(2, textlayout::move_vertical(text, strlen(text), 2, kWidth, -1, desired));
}

void test_move_down_over_wrapped_rows()
{
    // Одна длинная строка без '\n' — это три строки сетки.
    const char* text = "abcdefghijklmnopqrstuvwxyz";
    size_t desired = 3;
    const size_t pos = textlayout::move_vertical(text, strlen(text), 3, kWidth, 1, desired);
    TEST_ASSERT_EQUAL_UINT32(13, pos);
    TEST_ASSERT_EQUAL_UINT32(1, row_of(text, pos));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_layout_empty_is_one_row);
    RUN_TEST(test_layout_short_text_is_one_row);
    RUN_TEST(test_layout_wraps_at_width);
    RUN_TEST(test_layout_counts_chars_not_bytes);
    RUN_TEST(test_layout_explicit_newline);
    RUN_TEST(test_layout_newline_at_width_boundary);
    RUN_TEST(test_layout_trailing_newline_adds_empty_row);
    RUN_TEST(test_layout_respects_max_rows);
    RUN_TEST(test_layout_zero_width_survives);

    RUN_TEST(test_caret_at_start);
    RUN_TEST(test_caret_mid_first_row);
    RUN_TEST(test_caret_on_wrap_boundary_belongs_to_next_row);
    RUN_TEST(test_caret_after_newline);
    RUN_TEST(test_caret_never_splits_cyrillic);
    RUN_TEST(test_caret_pos_roundtrip);
    RUN_TEST(test_caret_pos_clamps_to_line_end);
    RUN_TEST(test_caret_pos_beyond_text_returns_len);

    RUN_TEST(test_move_down_keeps_column);
    RUN_TEST(test_move_up_keeps_column);
    RUN_TEST(test_move_down_through_short_line_remembers_column);
    RUN_TEST(test_move_down_at_last_row_does_nothing);
    RUN_TEST(test_move_up_at_first_row_does_nothing);
    RUN_TEST(test_move_down_over_wrapped_rows);

    return UNITY_END();
}
