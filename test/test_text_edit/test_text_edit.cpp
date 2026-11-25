/*
 * Тесты редактирования заметок.
 *
 * Все дефекты здесь тихие: каретка уезжает за конец, Backspace разрезает
 * кириллицу, отмена возвращает не тот текст. На плате это видно только по
 * испорченному файлу на карте, поэтому проверяем на хосте.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/text_edit.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kCap = 256;

/// Редактор с текстом в статическом буфере: в тестах PSRAM нет.
struct Fixture {
    char buf[kCap];
    textedit::Editor ed;

    explicit Fixture(const char* text)
    {
        memset(buf, 0, sizeof(buf));
        textedit::load(ed, buf, sizeof(buf), text, strlen(text));
    }

    const char* text() const { return buf; }
};

}  // namespace

// --- Загрузка и границы -----------------------------------------------------

void test_load_sets_text_and_caret()
{
    Fixture f("привет");
    TEST_ASSERT_EQUAL_STRING("привет", f.text());
    // Длины — в БАЙТАХ UTF-8: «привет» из шести русских букв занимает 12.
    TEST_ASSERT_EQUAL_UINT32(12, f.ed.len);
    TEST_ASSERT_EQUAL_UINT32(0, f.ed.caret);
}

void test_load_from_same_buffer()
{
    // open_doc() читает тело прямо в буфер редактора и передаёт его же сюда:
    // копирование с перекрытием не должно портить текст.
    char buf[kCap];
    snprintf(buf, sizeof(buf), "%s", "Сброс ошибки очистителя");
    textedit::Editor ed;
    TEST_ASSERT_TRUE(textedit::load(ed, buf, sizeof(buf), buf, strlen(buf)));
    TEST_ASSERT_EQUAL_STRING("Сброс ошибки очистителя", buf);
    TEST_ASSERT_EQUAL_UINT32(strlen("Сброс ошибки очистителя"), ed.len);
}

void test_editor_without_buffer_survives_keys()
{
    // load() мог не суметь привязать буфер (нет памяти). Нажатие стрелки в
    // таком редакторе не должно разыменовывать nullptr — иначе экран падал бы
    // при первом же касании клавиши.
    textedit::Editor ed;
    ed.caret = 5;
    textedit::move_left(ed);
    textedit::move_right(ed);
    TEST_ASSERT_EQUAL_UINT32(0, ed.caret);
    TEST_ASSERT_FALSE(textedit::backspace(ed));
    TEST_ASSERT_FALSE(textedit::delete_forward(ed));
    TEST_ASSERT_FALSE(textedit::insert_char(ed, "а"));
    TEST_ASSERT_FALSE(textedit::undo(ed));
    TEST_ASSERT_EQUAL_UINT32(0, textedit::line_start(ed));
    TEST_ASSERT_EQUAL_UINT32(0, textedit::line_end(ed));
}

void test_load_rejects_too_long_text()
{
    char buf[8];
    textedit::Editor ed;
    TEST_ASSERT_FALSE(textedit::load(ed, buf, sizeof(buf), "слишком длинный текст", 21));
    TEST_ASSERT_EQUAL_UINT32(0, ed.len);
}

void test_char_len_utf8()
{
    TEST_ASSERT_EQUAL_UINT32(1, textedit::char_len('a'));
    TEST_ASSERT_EQUAL_UINT32(2, textedit::char_len(0xD0));  // «п»
    TEST_ASSERT_EQUAL_UINT32(3, textedit::char_len(0xE2));  // «—»
    TEST_ASSERT_EQUAL_UINT32(4, textedit::char_len(0xF0));  // эмодзи-диапазон
    TEST_ASSERT_EQUAL_UINT32(1, textedit::char_len(0x80));  // битый продолжение
}

void test_prev_char_never_splits_cyrillic()
{
    const char* s = "аб";  // 4 байта
    TEST_ASSERT_EQUAL_UINT32(2, textedit::prev_char(s, 4));
    TEST_ASSERT_EQUAL_UINT32(0, textedit::prev_char(s, 2));
    TEST_ASSERT_EQUAL_UINT32(0, textedit::prev_char(s, 0));
}

void test_next_char_stops_at_len()
{
    const char* s = "аб";
    TEST_ASSERT_EQUAL_UINT32(2, textedit::next_char(s, 0, 4));
    TEST_ASSERT_EQUAL_UINT32(4, textedit::next_char(s, 2, 4));
    TEST_ASSERT_EQUAL_UINT32(4, textedit::next_char(s, 4, 4));
}

void test_clamp_caret_moves_out_of_multibyte()
{
    Fixture f("аб");
    // Каретка внутрь «б» — встаёт на его начало.
    f.ed.caret = 3;
    textedit::clamp_caret(f.ed);
    TEST_ASSERT_EQUAL_UINT32(2, f.ed.caret);

    f.ed.caret = 100;
    textedit::clamp_caret(f.ed);
    TEST_ASSERT_EQUAL_UINT32(4, f.ed.caret);
}

// --- Вставка ----------------------------------------------------------------

void test_insert_at_caret()
{
    Fixture f("мир");
    f.ed.caret = 2;  // после «м»
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "и"));
    TEST_ASSERT_EQUAL_STRING("миир", f.text());
    TEST_ASSERT_EQUAL_UINT32(4, f.ed.caret);
}

void test_insert_rejects_when_full()
{
    char buf[6];
    textedit::Editor ed;
    textedit::load(ed, buf, sizeof(buf), "abc", 3);
    TEST_ASSERT_FALSE(textedit::insert(ed, "def", 3));
    TEST_ASSERT_EQUAL_STRING("abc", ed.data);
}

void test_insert_into_empty()
{
    Fixture f("");
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "я"));
    TEST_ASSERT_EQUAL_STRING("я", f.text());
    TEST_ASSERT_EQUAL_UINT32(2, f.ed.caret);
}

// --- Удаление ---------------------------------------------------------------

void test_backspace_removes_whole_char()
{
    Fixture f("аб");
    f.ed.caret = 4;
    TEST_ASSERT_TRUE(textedit::backspace(f.ed));
    TEST_ASSERT_EQUAL_STRING("а", f.text());
    TEST_ASSERT_EQUAL_UINT32(2, f.ed.caret);
}

void test_backspace_at_start_does_nothing()
{
    Fixture f("аб");
    TEST_ASSERT_FALSE(textedit::backspace(f.ed));
    TEST_ASSERT_EQUAL_STRING("аб", f.text());
}

void test_backspace_inside_text_keeps_tail()
{
    Fixture f("aбв");
    f.ed.caret = 4;  // после «б»
    TEST_ASSERT_TRUE(textedit::backspace(f.ed));
    TEST_ASSERT_EQUAL_STRING("aв", f.text());
    TEST_ASSERT_EQUAL_UINT32(1, f.ed.caret);
}

void test_delete_forward_removes_under_caret()
{
    Fixture f("аб");
    TEST_ASSERT_TRUE(textedit::delete_forward(f.ed));
    TEST_ASSERT_EQUAL_STRING("б", f.text());
    TEST_ASSERT_EQUAL_UINT32(0, f.ed.caret);
}

void test_delete_forward_at_end_does_nothing()
{
    Fixture f("аб");
    f.ed.caret = 4;
    TEST_ASSERT_FALSE(textedit::delete_forward(f.ed));
    TEST_ASSERT_EQUAL_STRING("аб", f.text());
}

void test_delete_to_line_start()
{
    Fixture f("строка\nхвост");
    f.ed.caret = strlen("строка\nхвос");
    TEST_ASSERT_TRUE(textedit::delete_to_line_start(f.ed));
    TEST_ASSERT_EQUAL_STRING("строка\nт", f.text());
    TEST_ASSERT_EQUAL_UINT32(strlen("строка\n"), f.ed.caret);
}

void test_delete_to_line_start_at_start_does_nothing()
{
    Fixture f("строка");
    TEST_ASSERT_FALSE(textedit::delete_to_line_start(f.ed));
}

// --- Каретка ----------------------------------------------------------------

void test_move_left_right_over_cyrillic()
{
    Fixture f("аб");
    f.ed.caret = 2;
    textedit::move_right(f.ed);
    TEST_ASSERT_EQUAL_UINT32(4, f.ed.caret);
    // Правее конца — остаётся на конце.
    textedit::move_right(f.ed);
    TEST_ASSERT_EQUAL_UINT32(4, f.ed.caret);

    textedit::move_left(f.ed);
    TEST_ASSERT_EQUAL_UINT32(2, f.ed.caret);
    textedit::move_left(f.ed);
    TEST_ASSERT_EQUAL_UINT32(0, f.ed.caret);
}

void test_line_start_end()
{
    Fixture f("один\nдва");
    f.ed.caret = strlen("один\nдв");
    // «один» — 8 байт, перевод строки — 9-й, значит вторая строка с 9.
    TEST_ASSERT_EQUAL_UINT32(9, textedit::line_start(f.ed));
    TEST_ASSERT_EQUAL_UINT32(strlen("один\nдва"), textedit::line_end(f.ed));
}

// --- Отмена -----------------------------------------------------------------

void test_undo_insert_removes_it()
{
    Fixture f("мир");
    f.ed.caret = 2;
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "и"));
    TEST_ASSERT_EQUAL_STRING("миир", f.text());

    TEST_ASSERT_TRUE(textedit::undo(f.ed));
    TEST_ASSERT_EQUAL_STRING("мир", f.text());
    TEST_ASSERT_EQUAL_UINT32(2, f.ed.caret);
}

void test_undo_backspace_restores_char()
{
    Fixture f("аб");
    f.ed.caret = 4;
    TEST_ASSERT_TRUE(textedit::backspace(f.ed));
    TEST_ASSERT_EQUAL_STRING("а", f.text());

    TEST_ASSERT_TRUE(textedit::undo(f.ed));
    TEST_ASSERT_EQUAL_STRING("аб", f.text());
    TEST_ASSERT_EQUAL_UINT32(4, f.ed.caret);
}

void test_undo_many_steps_in_order()
{
    Fixture f("");
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "а"));
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "б"));
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "в"));
    TEST_ASSERT_EQUAL_STRING("абв", f.text());
    TEST_ASSERT_EQUAL_UINT32(3, textedit::undo_depth(f.ed));

    TEST_ASSERT_TRUE(textedit::undo(f.ed));
    TEST_ASSERT_EQUAL_STRING("аб", f.text());
    TEST_ASSERT_TRUE(textedit::undo(f.ed));
    TEST_ASSERT_EQUAL_STRING("а", f.text());
    TEST_ASSERT_TRUE(textedit::undo(f.ed));
    TEST_ASSERT_EQUAL_STRING("", f.text());
    TEST_ASSERT_FALSE(textedit::undo(f.ed));
}

void test_undo_history_is_bounded()
{
    Fixture f("");
    for (size_t i = 0; i < textedit::kMaxUndo + 10; ++i) {
        TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "я"));
    }
    TEST_ASSERT_EQUAL_UINT32(textedit::kMaxUndo, textedit::undo_depth(f.ed));

    // Отменяются последние kMaxUndo вставок, остальное остаётся — и это
    // ожидаемо: история короткая, но текст цел.
    for (size_t i = 0; i < textedit::kMaxUndo; ++i) {
        TEST_ASSERT_TRUE(textedit::undo(f.ed));
    }
    // Осталось ровно столько вставок, сколько не влезло в историю. «я» —
    // 2 байта, значит длина 20, а не 10.
    TEST_ASSERT_EQUAL_UINT32(20, f.ed.len);
}

void test_clear_undo_forgets_history()
{
    Fixture f("");
    TEST_ASSERT_TRUE(textedit::insert_char(f.ed, "а"));
    textedit::clear_undo(f.ed);
    TEST_ASSERT_FALSE(textedit::undo(f.ed));
    TEST_ASSERT_EQUAL_STRING("а", f.text());
}

// --- Кириллица и границы ----------------------------------------------------

void test_insert_delete_roundtrip_keeps_text_intact()
{
    Fixture f("Сброс ошибки очистителя");
    f.ed.caret = f.ed.len;

    // Набираем и стираем «фильтра» по символу — текст обязан совпасть.
    const char* word = "фильтра";
    size_t p = 0;
    while (word[p] != '\0') {
        const size_t step = textedit::char_len(static_cast<unsigned char>(word[p]));
        char piece[8];
        memcpy(piece, word + p, step);
        piece[step] = '\0';
        TEST_ASSERT_TRUE(textedit::insert_char(f.ed, piece));
        p += step;
    }
    TEST_ASSERT_EQUAL_STRING("Сброс ошибки очистителяфильтра", f.text());

    for (int i = 0; i < 7; ++i) {
        TEST_ASSERT_TRUE(textedit::backspace(f.ed));
    }
    TEST_ASSERT_EQUAL_STRING("Сброс ошибки очистителя", f.text());
    TEST_ASSERT_EQUAL_UINT32(f.ed.len, f.ed.caret);
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_load_sets_text_and_caret);
    RUN_TEST(test_load_from_same_buffer);
    RUN_TEST(test_editor_without_buffer_survives_keys);
    RUN_TEST(test_load_rejects_too_long_text);
    RUN_TEST(test_char_len_utf8);
    RUN_TEST(test_prev_char_never_splits_cyrillic);
    RUN_TEST(test_next_char_stops_at_len);
    RUN_TEST(test_clamp_caret_moves_out_of_multibyte);

    RUN_TEST(test_insert_at_caret);
    RUN_TEST(test_insert_rejects_when_full);
    RUN_TEST(test_insert_into_empty);

    RUN_TEST(test_backspace_removes_whole_char);
    RUN_TEST(test_backspace_at_start_does_nothing);
    RUN_TEST(test_backspace_inside_text_keeps_tail);
    RUN_TEST(test_delete_forward_removes_under_caret);
    RUN_TEST(test_delete_forward_at_end_does_nothing);
    RUN_TEST(test_delete_to_line_start);
    RUN_TEST(test_delete_to_line_start_at_start_does_nothing);

    RUN_TEST(test_move_left_right_over_cyrillic);
    RUN_TEST(test_line_start_end);

    RUN_TEST(test_undo_insert_removes_it);
    RUN_TEST(test_undo_backspace_restores_char);
    RUN_TEST(test_undo_many_steps_in_order);
    RUN_TEST(test_undo_history_is_bounded);
    RUN_TEST(test_clear_undo_forgets_history);

    RUN_TEST(test_insert_delete_roundtrip_keeps_text_intact);

    return UNITY_END();
}
