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

    // На клавише `.` в ЙЦУКЕН живёт «ю» — как на обычной клавиатуре.
    TEST_ASSERT_EQUAL_UINT32(U'.', keymap::codepoint(4, 9, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'ю', keymap::codepoint(4, 9, Layout::Russian, false, false));
}

// --- Пунктуация и «б» в кириллице ---
//
// На A164 в нижнем ряду одна пунктуационная клавиша (`.`), а на обычной
// клавиатуре их две: `,` и `/.`. В ЙЦУКЕН на них живут «б» и «ю/», поэтому
// по-русски нельзя было поставить ни точку, ни запятую, а буквы «б» не было
// вовсе. Потеря точки заметнее потери «б»: она нужна в каждом предложении.

void test_russian_period_on_underscore_key()
{
    // Точка — на клавишу `_`: рука ищет её вслепую, через Sym каждый раз
    // искать неудобно.
    TEST_ASSERT_EQUAL_UINT32(U'.', keymap::codepoint(3, 12, Layout::Russian, false, false));
    // С Aa — запятая, как Shift+`.` на обычной клавиатуре.
    TEST_ASSERT_EQUAL_UINT32(U',', keymap::codepoint(3, 12, Layout::Russian, true, false));

    // В латинице та же клавиша остаётся подчёркиванием: код и пути пишутся
    // по-английски, и `snake_case` должен набираться.
    TEST_ASSERT_EQUAL_UINT32(U'_', keymap::codepoint(3, 12, Layout::Latin, false, false));
    TEST_ASSERT_EQUAL_UINT32(U'=', keymap::codepoint(3, 12, Layout::Latin, false, true));
}

void test_russian_letter_be_on_sym_underscore()
{
    TEST_ASSERT_EQUAL_UINT32(U'б', keymap::codepoint(3, 12, Layout::Russian, false, true));
    // Заглавная тоже нужна: «Б» начинает предложения и имена.
    TEST_ASSERT_EQUAL_UINT32(U'Б', keymap::codepoint(3, 12, Layout::Russian, true, true));
}

void test_comma_on_sym_dot()
{
    // Так же поступает и сама M5Stack в HID-таблицах этой клавиатуры:
    // «Row 4 — Sym delta: . → ,».
    TEST_ASSERT_EQUAL_UINT32(U',', keymap::codepoint(4, 9, Layout::Latin, false, true));
    TEST_ASSERT_EQUAL_UINT32(U',', keymap::codepoint(4, 9, Layout::Russian, false, true));
}

void test_russian_alphabet_is_complete()
{
    // Ни один тест до этого не проверял, что все буквы доступны, — поэтому
    // отсутствие «б» прожило до v0.2.0. Проверяем весь алфавит.
    static const char32_t kAlphabet[] = U"абвгдеёжзийклмнопрстуфхцчшщъыьэюя";
    for (size_t i = 0; kAlphabet[i] != 0; ++i) {
        const char32_t want = kAlphabet[i];
        bool found = false;
        for (uint8_t row = 0; row < keymap::kRows && !found; ++row) {
            for (uint8_t col = 0; col < keymap::kCols && !found; ++col) {
                for (int shift = 0; shift <= 1 && !found; ++shift) {
                    for (int sym = 0; sym <= 1 && !found; ++sym) {
                        if (keymap::codepoint(row, col, Layout::Russian, shift != 0, sym != 0) == want) found = true;
                    }
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "буква недоступна в русской раскладке");
    }
}

void test_russian_basic_punctuation_is_reachable()
{
    // Точка и запятая — минимум для связного текста. Остальная пунктуация
    // (кавычки, тире) приходит от модели и в коде не набирается.
    static const char32_t kNeeded[] = U".,";
    for (size_t i = 0; kNeeded[i] != 0; ++i) {
        const char32_t want = kNeeded[i];
        bool found = false;
        for (uint8_t row = 0; row < keymap::kRows && !found; ++row) {
            for (uint8_t col = 0; col < keymap::kCols && !found; ++col) {
                for (int shift = 0; shift <= 1 && !found; ++shift) {
                    for (int sym = 0; sym <= 1 && !found; ++sym) {
                        if (keymap::codepoint(row, col, Layout::Russian, shift != 0, sym != 0) == want) found = true;
                    }
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "знак недоступен в русской раскладке");
    }
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

// Контракт char_utf8(): возвращает число записанных байт БЕЗ завершающего
// нуля. Поэтому буфер обнуляем сами — иначе TEST_ASSERT_EQUAL_STRING читает
// неинициализированную память и тест может «пройти» случайно.

void test_utf8_ascii()
{
    char buf[8] = {};
    TEST_ASSERT_EQUAL_UINT(1, keymap::char_utf8(2, 1, Layout::Latin, false, false, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("q", buf);
}

void test_utf8_cyrillic_two_bytes()
{
    char buf[8] = {};
    TEST_ASSERT_EQUAL_UINT(2, keymap::char_utf8(2, 1, Layout::Russian, false, false, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("й", buf);
}

void test_utf8_yo_two_bytes()
{
    // «Ё» = U+0401 = D0 81: два байта, а не три. Название теста раньше врало.
    char buf[8] = {};
    TEST_ASSERT_EQUAL_UINT(2, keymap::char_utf8(1, 0, Layout::Russian, true, false, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Ё", buf);
    TEST_ASSERT_EQUAL_UINT8(0xD0, static_cast<uint8_t>(buf[0]));
    TEST_ASSERT_EQUAL_UINT8(0x81, static_cast<uint8_t>(buf[1]));
}

void test_utf8_no_room()
{
    char buf[1] = {};
    TEST_ASSERT_EQUAL_UINT(0, keymap::char_utf8(2, 1, Layout::Russian, false, false, buf, sizeof(buf)));
}

void test_utf8_special_gives_zero()
{
    char buf[8] = {};
    TEST_ASSERT_EQUAL_UINT(0, keymap::char_utf8(3, 13, Layout::Latin, false, false, buf, sizeof(buf)));
}

// --- Полнота матрицы ---

/// Каждая из 70 позиций должна чем-то быть: спецклавишей или символом.
///
/// Тест-инвариант появился после бага на железе: модификаторы (Sym, Aa, Ctrl,
/// Alt) отдавались драйвером как «событие без символа» и молча терялись, из-за
/// чего диагностика показывала 66 из 70 нажатых клавиш. Позиция в матрице без
/// определения - та же ошибка, только в таблице.
void test_every_position_is_defined()
{
    int silent = 0;
    for (uint8_t row = 0; row < keymap::kRows; ++row) {
        for (uint8_t col = 0; col < keymap::kCols; ++col) {
            const bool is_special = keymap::special(row, col) != keymap::Special::None;
            const bool has_latin = keymap::codepoint(row, col, Layout::Latin, false, false) != 0;
            const bool has_russian = keymap::codepoint(row, col, Layout::Russian, false, false) != 0;
            if (!is_special && !has_latin && !has_russian) ++silent;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, silent);
}

/// Все четыре модификатора должны быть видны как спецклавиши.
/// Их пропажа в потоке событий - это баг драйвера, а не таблицы.
void test_modifiers_are_visible()
{
    const keymap::Special mods[] = {
        keymap::Special::Sym, keymap::Special::Aa, keymap::Special::Ctrl, keymap::Special::Alt};
    for (keymap::Special want : mods) {
        bool found = false;
        for (uint8_t row = 0; row < keymap::kRows && !found; ++row) {
            for (uint8_t col = 0; col < keymap::kCols && !found; ++col) {
                if (keymap::special(row, col) == want) {
                    found = true;
                    // У модификатора не должно быть печатного символа: иначе он
                    // попадёт в текст вместе с нажатием.
                    TEST_ASSERT_EQUAL_UINT32(0, keymap::codepoint(row, col, Layout::Latin, false, false));
                }
            }
        }
        TEST_ASSERT_TRUE(found);
    }
}

/// Спецзначения должны быть уникальны: они передаются номером, и совпадение
/// двух номеров проявилось бы на устройстве как «нажал одно, сработало другое».
void test_special_values_are_unique()
{
    const keymap::Special all[] = {keymap::Special::None, keymap::Special::Escape, keymap::Special::Tab,
        keymap::Special::Backspace, keymap::Special::Enter, keymap::Special::Space, keymap::Special::Delete,
        keymap::Special::Up, keymap::Special::Down, keymap::Special::Left, keymap::Special::Right, keymap::Special::Sym,
        keymap::Special::Aa, keymap::Special::Ctrl, keymap::Special::Alt, keymap::Special::LayoutToggle};

    const size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            TEST_ASSERT_NOT_EQUAL(all[i], all[j]);
        }
    }
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
    RUN_TEST(test_russian_period_on_underscore_key);
    RUN_TEST(test_russian_letter_be_on_sym_underscore);
    RUN_TEST(test_comma_on_sym_dot);
    RUN_TEST(test_russian_alphabet_is_complete);
    RUN_TEST(test_russian_basic_punctuation_is_reachable);

    RUN_TEST(test_sym_layer);
    RUN_TEST(test_sym_does_not_touch_letters);

    RUN_TEST(test_utf8_ascii);
    RUN_TEST(test_utf8_cyrillic_two_bytes);
    RUN_TEST(test_utf8_yo_two_bytes);
    RUN_TEST(test_utf8_no_room);
    RUN_TEST(test_utf8_special_gives_zero);

    RUN_TEST(test_every_position_is_defined);
    RUN_TEST(test_modifiers_are_visible);
    RUN_TEST(test_special_values_are_unique);

    return UNITY_END();
}
