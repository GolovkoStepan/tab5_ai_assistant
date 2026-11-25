/*
 * Тесты поиска по архиву и сниппетов.
 *
 * Поиск — главная функция архива: если он не находит «фильтр» в «Замена
 * ФИЛЬТРА», смысл всего этапа теряется. Ошибки здесь не видны глазами, поэтому
 * проверяем на хосте.
 */

#include <string.h>

#include <unity.h>

#include "util/doc_search.h"

void setUp(void) {}
void tearDown(void) {}

// --- Поиск без учёта регистра ----------------------------------------------

void test_find_ci_latin_lower()
{
    const char* hay = "Air purifier filter reset";
    const char* p = docsearch::find_ci(hay, strlen(hay), "filter", 6);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(13, static_cast<uint32_t>(p - hay));
}

void test_find_ci_latin_upper_needle()
{
    const char* hay = "air purifier filter";
    TEST_ASSERT_NOT_NULL(docsearch::find_ci(hay, strlen(hay), "FILTER", 6));
}

void test_find_ci_cyrillic_both_directions()
{
    // Главный случай: регистр в кириллице — разные байты.
    const char* hay = "Замена ФИЛЬТРА очистителя";
    TEST_ASSERT_NOT_NULL(docsearch::find_ci(hay, strlen(hay), "фильтр", strlen("фильтр")));
    TEST_ASSERT_NOT_NULL(docsearch::find_ci(hay, strlen(hay), "ФИЛЬТР", strlen("ФИЛЬТР")));
}

void test_find_ci_yo()
{
    // Ё — отдельный символ, а не «Е с точками»: без явной пары «Ё/ё» поиск
    // «ёлочь» не находил бы «ЩЁЛОЧЬ».
    const char* hay = "ЩЁЛОЧЬ в составе";
    TEST_ASSERT_NOT_NULL(docsearch::find_ci(hay, strlen(hay), "щёлочь", strlen("щёлочь")));
}

void test_find_ci_mixed()
{
    const char* hay = "Сброс ошибки Air Purifier";
    TEST_ASSERT_NOT_NULL(docsearch::find_ci(hay, strlen(hay), "air", 3));
    TEST_ASSERT_NOT_NULL(docsearch::find_ci(hay, strlen(hay), "СБРОС", strlen("СБРОС")));
}

void test_find_ci_not_found()
{
    const char* hay = "Замена фильтра";
    TEST_ASSERT_NULL(docsearch::find_ci(hay, strlen(hay), "очиститель", strlen("очиститель")));
}

void test_find_ci_does_not_match_inside_multibyte()
{
    // Байт 0x90 есть внутри «А» (D0 90), но это не «\x90» как ASCII и уж
    // точно не латинская буква. Поиск со сдвигом на середину символа мог бы
    // «найти» совпадение там, где его нет.
    const char* hay = "ААА";
    TEST_ASSERT_NULL(docsearch::find_ci(hay, strlen(hay), "\x90", 1));
}

void test_find_ci_empty_needle()
{
    const char* hay = "текст";
    TEST_ASSERT_EQUAL_PTR(hay, docsearch::find_ci(hay, strlen(hay), "", 0));
}

void test_find_ci_larger_needle()
{
    const char* hay = "да";
    TEST_ASSERT_NULL(docsearch::find_ci(hay, strlen(hay), "давно", strlen("давно")));
}

// --- Сниппеты -----------------------------------------------------------------

void test_snippet_centers_on_hit()
{
    const char* text = "Шаг первый: выключите прибор. Шаг второй: зажмите кнопку. Шаг третий: ждите.";
    const size_t hit = static_cast<size_t>(strstr(text, "зажмите") - text);
    char out[256];
    const size_t n = docsearch::snippet(text, strlen(text), hit, strlen("зажмите"), 20, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "зажмите"));
    // Обрезано с обеих сторон — многоточие в начале и в конце.
    TEST_ASSERT_EQUAL_UINT8(0xE2, static_cast<unsigned char>(out[0]));
    TEST_ASSERT_EQUAL_STRING("…", out + n - 3);
}

void test_snippet_at_start_has_no_leading_ellipsis()
{
    const char* text = "Начало документа и продолжение текста дальше.";
    char out[256];
    const size_t n = docsearch::snippet(text, strlen(text), 0, strlen("Начало"), 20, out, sizeof(out));

    // Совпадение в самом начале: ведущего многоточия быть не должно, иначе
    // модель решила бы, что упустила что-то важное до него.
    TEST_ASSERT_NOT_EQUAL(0xE2, static_cast<unsigned char>(out[0]));
    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "Начало", 12));
    TEST_ASSERT_EQUAL_STRING("…", out + n - 3);
}

void test_snippet_replaces_newlines()
{
    // Многострочный кусок в ответе модели выглядел бы как рваная лента.
    const char* text = "первая строка\nвторая строка\nтретья";
    const size_t hit = static_cast<size_t>(strstr(text, "вторая") - text);
    char out[256];
    docsearch::snippet(text, strlen(text), hit, strlen("вторая"), 8, out, sizeof(out));
    TEST_ASSERT_NULL(strchr(out, '\n'));
    TEST_ASSERT_NOT_NULL(strstr(out, "вторая"));
}

void test_snippet_does_not_split_utf8()
{
    // Радиус 5 байт попадает в середину кириллической буквы. Результат
    // обязан быть валидным UTF-8: иначе на экране ромбик с вопросом.
    const char* text = "Очиститель воздуха сбрасывает ошибку после замены фильтра";
    const size_t hit = static_cast<size_t>(strstr(text, "ошибку") - text);
    char out[256];
    const size_t n = docsearch::snippet(text, strlen(text), hit, strlen("ошибку"), 5, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    // Первый байт не может быть продолжением символа (10xxxxxx): иначе на
    // экране ромбик с вопросом.
    TEST_ASSERT_NOT_EQUAL(0x80, static_cast<unsigned char>(out[0]) & 0xC0);
}

void test_snippet_whole_text_no_ellipsis()
{
    const char* text = "Короткий текст";
    char out[256];
    docsearch::snippet(text, strlen(text), 0, 3, 100, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Короткий текст", out);
}

// --- Усечение --------------------------------------------------------------

void test_truncate_short_text_unchanged()
{
    char out[64];
    const size_t n = docsearch::truncate_utf8("короткий", strlen("короткий"), 50, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("короткий", out);
    TEST_ASSERT_EQUAL_UINT32(16, n);
}

void test_truncate_adds_ellipsis()
{
    const char* text = "Очень длинный текст, который не помещается целиком";
    char out[32];
    const size_t n = docsearch::truncate_utf8(text, strlen(text), 21, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "…"));
}

void test_truncate_does_not_split_utf8()
{
    // 21 байт — это 10 кириллических букв и половина одиннадцатой. Усечение
    // обязано остановиться на границе символа.
    char out[64];
    docsearch::truncate_utf8("абвгдежзийклмнопрст", strlen("абвгдежзийклмнопрст"), 21, out, sizeof(out));
    // Строка должна быть валидным UTF-8: все кириллические буквы по 2 байта,
    // плюс трёхбайтовое многоточие. Значит байт ровно вдвое больше букв + 3.
    const size_t bytes = strlen(out);
    size_t letters = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if ((static_cast<unsigned char>(out[i]) & 0xC0) != 0x80) ++letters;
    }
    TEST_ASSERT_EQUAL_UINT32(bytes, (letters - 1) * 2 + 3);
}

void test_truncate_tiny_buffer()
{
    char out[4];
    // В буфер на 4 байта влезает только многоточие с нулём — но не «ничего
    // не писать»: текст был длиннее, и это должно быть видно.
    const size_t n = docsearch::truncate_utf8("abcdefghij", 10, 3, out, sizeof(out));
    TEST_ASSERT_TRUE(n <= 3);
    TEST_ASSERT_TRUE(strlen(out) <= 3);
}

// --- Строка списка ---------------------------------------------------------

void test_format_list_row_full()
{
    char out[128];
    const size_t n = docsearch::format_list_row("home/air.md", "Сброс ошибки", "2026-11-23", out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("home/air.md — Сброс ошибки (2026-11-23)", out);
}

void test_format_list_row_without_date()
{
    char out[128];
    docsearch::format_list_row("home/air.md", "Сброс ошибки", "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("home/air.md — Сброс ошибки", out);
}

void test_format_list_row_without_title_uses_path()
{
    char out[128];
    docsearch::format_list_row("home/air.md", "", "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("home/air.md — home/air.md", out);
}

void test_format_list_row_overflow()
{
    char out[16];
    TEST_ASSERT_EQUAL_UINT32(
        0, docsearch::format_list_row("home/very-long-name.md", "Заголовок", "", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

// --- Длина UTF-8 символа ---------------------------------------------------

void test_utf8_char_len()
{
    TEST_ASSERT_EQUAL_UINT32(1, docsearch::utf8_char_len('a'));
    TEST_ASSERT_EQUAL_UINT32(1, docsearch::utf8_char_len(0x7F));
    TEST_ASSERT_EQUAL_UINT32(2, docsearch::utf8_char_len(0xD0));  // кириллица
    TEST_ASSERT_EQUAL_UINT32(3, docsearch::utf8_char_len(0xE2));  // «…»
    TEST_ASSERT_EQUAL_UINT32(4, docsearch::utf8_char_len(0xF0));  // эмодзи
    TEST_ASSERT_EQUAL_UINT32(1, docsearch::utf8_char_len(0x80));  // битый
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_find_ci_latin_lower);
    RUN_TEST(test_find_ci_latin_upper_needle);
    RUN_TEST(test_find_ci_cyrillic_both_directions);
    RUN_TEST(test_find_ci_yo);
    RUN_TEST(test_find_ci_mixed);
    RUN_TEST(test_find_ci_not_found);
    RUN_TEST(test_find_ci_does_not_match_inside_multibyte);
    RUN_TEST(test_find_ci_empty_needle);
    RUN_TEST(test_find_ci_larger_needle);

    RUN_TEST(test_snippet_centers_on_hit);
    RUN_TEST(test_snippet_at_start_has_no_leading_ellipsis);
    RUN_TEST(test_snippet_replaces_newlines);
    RUN_TEST(test_snippet_does_not_split_utf8);
    RUN_TEST(test_snippet_whole_text_no_ellipsis);

    RUN_TEST(test_truncate_short_text_unchanged);
    RUN_TEST(test_truncate_adds_ellipsis);
    RUN_TEST(test_truncate_does_not_split_utf8);
    RUN_TEST(test_truncate_tiny_buffer);

    RUN_TEST(test_format_list_row_full);
    RUN_TEST(test_format_list_row_without_date);
    RUN_TEST(test_format_list_row_without_title_uses_path);
    RUN_TEST(test_format_list_row_overflow);

    RUN_TEST(test_utf8_char_len);

    return UNITY_END();
}
