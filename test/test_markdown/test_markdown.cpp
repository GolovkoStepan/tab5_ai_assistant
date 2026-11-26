#include <unity.h>

#include <string.h>

#include "util/markdown.h"

namespace {

/// Развернуть разметку и сверить результат.
void check(const char* src, const char* expected)
{
    static char out[4096];
    const size_t n = md::plain(src, strlen(src), out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(strlen(expected), n);
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

}  // namespace

/// Ссылка markdown: человек должен увидеть подпись, а не скобки с адресом.
void test_link_keeps_text()
{
    check("По данным [mi.com](https://www.mi.com/uk/x/) кнопка сзади.", "По данным mi.com кнопка сзади.");
}

/// Несколько ссылок в одной строке.
void test_multiple_links()
{
    check("[a](http://a.ru) и [b](http://b.ru)", "a и b");
}

/// Пустая подпись: терять адрес нельзя, показываем его.
void test_empty_link_text_keeps_url()
{
    check("см. [](https://example.com/x)", "см. https://example.com/x");
}

/// Скобки в адресе — обычное дело у вики-ссылок; внешние не должны ломать срез.
void test_link_with_parens_in_url()
{
    check("[wiki](https://ru.wikipedia.org/wiki/Foo_(bar)) — так", "wiki — так");
}

/// Жирный и курсив снимаются, текст остаётся.
void test_bold_and_italic()
{
    check("**важно**: это *курсив* и `код`.", "важно: это курсив и код.");
}

/// `snake_case` — идентификатор, а не курсив: подчёркивания остаются.
void test_snake_case_kept()
{
    check("файл home/xiaomi_filter_reset.md", "файл home/xiaomi_filter_reset.md");
}

/// Одиночная звёздочка — умножение, её убирать нельзя.
void test_single_asterisk_kept()
{
    check("2 * 3 = 6", "2 * 3 = 6");
}

/// Знак списка остаётся: пункт иначе слипнется с абзацем.
void test_bullet_kept()
{
    check("- первый\n* второй\n+ третий", "- первый\n* второй\n+ третий");
}

/// Заголовок: решётки и пробел после них не нужны.
void test_heading_markers_removed()
{
    check("# Заголовок\n\n## Поменьше", "Заголовок\n\nПоменьше");
}

/// Решётка в середине строки — «№5», «C#», её трогать нельзя.
void test_hash_inside_line_kept()
{
    check("пункт №5 и язык C#", "пункт №5 и язык C#");
}

/// Переводы строк сохраняются: лента считает строки по ним.
void test_newlines_preserved()
{
    check("раз\nдва\n\nтри", "раз\nдва\n\nтри");
}

/// Текст без разметки не меняется ни на байт.
void test_plain_text_untouched()
{
    check(
        "Обычный ответ без разметки. Даже с точкой и запятой.", "Обычный ответ без разметки. Даже с точкой и запятой.");
}

/// `needed()` отвечает, нужна ли чистка: у обычного текста — нет.
void test_needed()
{
    TEST_ASSERT_FALSE(md::needed("обычный текст", 0));
    TEST_ASSERT_TRUE(md::needed("ссылка [a](b)", 0));
    TEST_ASSERT_TRUE(md::needed("**жирный**", 0));
    TEST_ASSERT_TRUE(md::needed("# заголовок", 0));
    TEST_ASSERT_FALSE(md::needed("", 0));
    TEST_ASSERT_FALSE(md::needed(nullptr, 0));
}

/// Буфер мал для самого текста — честный ноль, а не обрывок.
void test_small_buffer_returns_zero()
{
    char out[8];
    TEST_ASSERT_EQUAL_UINT(0, md::plain("[длинная подпись ссылки](http://x.ru)", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/// Ссылка, разорванная переводом строки, разметкой не считается: срез через
/// строки съел бы обычный текст.
void test_link_not_crossing_line()
{
    check("[не ссылка\n(http://x.ru)", "[не ссылка\n(http://x.ru)");
}

/// Кириллица в подписи ссылки не портится.
void test_cyrillic_link_text()
{
    check("[Инструкция по фильтру](https://mi.com/x) — тут", "Инструкция по фильтру — тут");
}

/// nullptr и нулевая длина не должны ничего ломать.
void test_null_src()
{
    char out[8];
    TEST_ASSERT_EQUAL_UINT(0, md::plain(nullptr, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/// Строка из живого ответа модели (26.11.2026): в треде она лежит с разметкой,
/// на экране должна читаться без скобок и адресов.
void test_real_answer_line()
{
    check("Ориентир по срокам: около 3-6 месяцев [mi.com](https://www.mi.com/ru/support/article/KA-14636/).",
        "Ориентир по срокам: около 3-6 месяцев mi.com.");
}

/// Весь ответ целиком: несколько ссылок, пункты списка, пустые строки.
/// Пункты и заголовок блока источников должны остаться на месте.
void test_real_full_answer()
{
    check(
        "1. Прибор сам считает остаток [mi.com](https://www.mi.com/ru/x/).\n"
        "2. Ориентир: 3-6 месяцев [mi.com](https://www.mi.com/ru/y/).\n"
        "\n"
        "Источники:\n"
        "1. mi.com — Срок службы фильтра\n",
        "1. Прибор сам считает остаток mi.com.\n"
        "2. Ориентир: 3-6 месяцев mi.com.\n"
        "\n"
        "Источники:\n"
        "1. mi.com — Срок службы фильтра\n");
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_link_keeps_text);
    RUN_TEST(test_multiple_links);
    RUN_TEST(test_empty_link_text_keeps_url);
    RUN_TEST(test_link_with_parens_in_url);
    RUN_TEST(test_bold_and_italic);
    RUN_TEST(test_snake_case_kept);
    RUN_TEST(test_single_asterisk_kept);
    RUN_TEST(test_bullet_kept);
    RUN_TEST(test_heading_markers_removed);
    RUN_TEST(test_hash_inside_line_kept);
    RUN_TEST(test_newlines_preserved);
    RUN_TEST(test_plain_text_untouched);
    RUN_TEST(test_needed);
    RUN_TEST(test_small_buffer_returns_zero);
    RUN_TEST(test_link_not_crossing_line);
    RUN_TEST(test_cyrillic_link_text);
    RUN_TEST(test_null_src);
    RUN_TEST(test_real_answer_line);
    RUN_TEST(test_real_full_answer);
    return UNITY_END();
}
