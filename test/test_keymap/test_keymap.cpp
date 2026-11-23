/*
 * Тесты раскладки клавиатуры A164.
 *
 * Проверяем то, что нельзя проверить глазами: соответствие физических позиций
 * матрицы латинским и русским символам, слою символов и модификатору Aa.
 */

#include <string.h>
#include <unity.h>

#include "util/keymap.h"

void setUp(void) {}
void tearDown(void) {}

using keymap::Layout;

// --- Special-клавиши ---

void test_special_positions()
{
    TEST_ASSERT_EQUAL(keymap::Special::Escape, keymap::special(0, 0));
    TEST_ASSERT_EQUAL(keymap::Special::Enter, keymap::special(3, 13));
    TEST_ASSERT_EQUAL(keymap::Special::Backspace, keymap::special(2, 13));
    TEST_ASSERT_EQUAL(keymap::Special::Space, keymap::special(4, 13));
    TEST_ASSERT_EQUAL(keymap::Special::Up, keymap::special(3, 11));
    TEST_ASSERT_EQUAL(keymap::Special::Left, keymap::special(4, 10));
    TEST_ASSERT_EQUAL(keymap::Special::Down, keymap::special(4, 11));
    TEST_ASSERT_EQUAL(keymap::Special::Right, keymap::special(4, 12));
    TEST_ASSERT_EQUAL(keymap::Special::Sym, keymap::special(3, 0));
    TEST_ASSERT_EQUAL(keymap::Special::Aa, keymap::special(3, 1));
    TEST_ASSERT_EQUAL(keymap::Special::Ctrl, keymap::special(4, 0));
    TEST_ASSERT_EQUAL(keymap::Special::Alt, keymap::special(4, 1));
}

void test_special_out_of_range()
{
    TEST_ASSERT_EQUAL(keymap::Special::None, keymap::special(5, 0));
    TEST_ASSERT_EQUAL(keymap::Special::None, keymap::special(0, 14));
}

// --- Латинская раскладка ---

void test_latin_letters()
{
    TEST_ASSERT_EQUAL_UINT32(U'q', keymap::codepoint(2, 1, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'p', keymap::codepoint(2, 10, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'a', keymap::codepoint(3, 2, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'm', keymap::codepoint(4, 8, Layout::Latin, false, false));
}

void test_latin_shift()
{
    TEST_ASSERT_EQUAL_UINT32(U'Q', keymap::codepoint(2, 1, Layout::Latin, true, false));
    TEST_ASSERT_EQUAL_UINT32(U'A', keymap::codepoint(3, 2, Layout::Latin, true, false));
}

void test_latin_digits_and_symbols()
{
    // На A164 отдельный ряд цифр и отдельный ряд символов — сдвиг не нужен.
    TEST_ASSERT_EQUAL_UINT32(U'1', keymap::codepoint(0, 1, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'0', keymap::codepoint(0, 10, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'!', keymap::codepoint(1, 1, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'@', keymap::codepoint(1, 2, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'+', keymap::codepoint(0, 12, Layout::Latin, false, false));
}

// --- Русская раскладка (ЙЦУКЕН) ---

void test_russian_letters()
{
    TEST_ASSERT_EQUAL_UINT32(U'й', keymap::codepoint(2, 1, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'ц', keymap::codepoint(2, 2, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'ф', keymap::codepoint(3, 2, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'я', keymap::codepoint(4, 2, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'ю', keymap::codepoint(4, 9, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'ь', keymap::codepoint(4, 8, Layout::Russian, false, false));
}

void test_russian_yo()
{
    // «Ё» живёт на клавише ` — как в стандартной раскладке Windows.
    TEST_ASSERT_EQUAL_UINT32(U'ё', keymap::codepoint(1, 0, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'Ё', keymap::codepoint(1, 0, Layout::Russian, true, false));
}

void test_russian_shift()
{
    TEST_ASSERT_EQUAL_UINT32(U'Й', keymap::codepoint(2, 1, Layout::Russian, true, false));
    TEST_ASSERT_EQUAL_UINT32(U'Ж', keymap::codepoint(2, 11, Layout::Russian, true, false));
    TEST_ASSERT_EQUAL_UINT32(U'Э', keymap::codepoint(2, 12, Layout::Russian, true, false));
}

void test_russian_punctuation_keeps_latin()
{
    // Клавиши, символы которых в ЙЦУКЕН не переопределяются (цифры и знаки
    // верхнего ряда), совпадают в обеих раскладках.
    TEST_ASSERT_EQUAL_UINT32(U'1', keymap::codepoint(0, 1, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'+', keymap::codepoint(0, 12, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'(', keymap::codepoint(1, 9, Layout::Russian, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'\\', keymap::codepoint(1, 13, Layout::Russian, false, false));

    // А точка и запятая — нет: на них в ЙЦУКЕН живут «ю» и «б».
    TEST_ASSERT_EQUAL_UINT32(U'.', keymap::codepoint(4, 9, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'ю', keymap::codepoint(4, 9, Layout::Russian, false, false));
}

// --- Слой символов (Sym) ---

void test_sym_layer()
{
    TEST_ASSERT_EQUAL_UINT32(U'~', keymap::codepoint(1, 0, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U'?', keymap::codepoint(1, 1, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U'{', keymap::codepoint(1, 11, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U'}', keymap::codepoint(1, 12, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U'|', keymap::codepoint(1, 13, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U':', keymap::codepoint(2, 11, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U'=', keymap::codepoint(3, 12, Layout::Latin, false, true));
}

void test_sym_does_not_touch_letters()
{
    TEST_ASSERT_EQUAL_UINT32(U'q', keymap::codepoint(2, 1, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U'й', keymap::codepoint(2, 1, Layout::Russian, false, true));
}

// --- UTF-8 ---

void test_utf8_ascii()
{
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(1, keymap::char_utf8(2, 1, Layout::Latin, false, false, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("q", buf);
}

void test_utf8_cyrillic_two_bytes()
{
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(2, keymap::char_utf8(2, 1, Layout::Russian, false, false, buf, sizeof(buf)));
    buf[2] = '\0';
    TEST_ASSERT_EQUAL_STRING("й", buf);
}

void test_utf8_yo_three_bytes()
{
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(2, keymap::char_utf8(1, 0, Layout::Russian, true, false, buf, sizeof(buf)));
    buf[2] = '\0';
    TEST_ASSERT_EQUAL_STRING("Ё", buf);
}

void test_utf8_no_room()
{
    char buf[1];
    TEST_ASSERT_EQUAL_UINT(0, keymap::char_utf8(2, 1, Layout::Russian, false, false, buf, sizeof(buf)));
}

void test_utf8_special_gives_zero()
{
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(0, keymap::char_utf8(3, 13, Layout::Latin, false, false, buf, sizeof(buf)));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_special_positions);
    RUN_TEST(test_special_out_of_range);

    RUN_TEST(test_latin_letters);
    RUN_TEST(test_latin_shift);
    RUN_TEST(test_latin_digits_and_symbols);

    RUN_TEST(test_russian_letters);
    RUN_TEST(test_russian_yo);
    RUN_TEST(test_russian_shift);
    RUN_TEST(test_russian_punctuation_keeps_latin);

    RUN_TEST(test_sym_layer);
    RUN_TEST(test_sym_does_not_touch_letters);

    RUN_TEST(test_utf8_ascii);
    RUN_TEST(test_utf8_cyrillic_two_bytes);
    RUN_TEST(test_utf8_yo_three_bytes);
    RUN_TEST(test_utf8_no_room);
    RUN_TEST(test_utf8_special_gives_zero);

    return UNITY_END();
}
