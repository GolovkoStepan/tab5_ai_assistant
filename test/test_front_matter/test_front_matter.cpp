/*
 * Тесты YAML-фронтматтера документа.
 *
 * Фронтматтер ломается молча: если собрать его неправильно, документ на
 * устройстве откроется, а на компьютере — нет (или наоборот). Поэтому
 * главный тест здесь — «собрать → разобрать → снова собрать»: обе стороны
 * должны понимать друг друга без потерь.
 */

#include <string.h>

#include <unity.h>

#include "util/front_matter.h"

void setUp(void) {}
void tearDown(void) {}

using doc::Meta;

namespace {

/// Заполнить Meta с нуля — так тесты не зависят от прошлого содержимого.
Meta make_meta()
{
    Meta m;
    strcpy(m.title, "Сброс ошибки очистителя воздуха");
    strcpy(m.tags, "бытовая-техника, очиститель");
    strcpy(m.source_url, "https://example.com/purifier-error-reset");
    strcpy(m.created, "2026-11-23T20:31:00+03:00");
    strcpy(m.updated, "2026-11-25T09:00:00+03:00");
    return m;
}

}  // namespace

// --- Разбор -----------------------------------------------------------------

void test_parse_full_document()
{
    const char* text =
        "---\n"
        "title: Сброс ошибки очистителя воздуха\n"
        "tags: [бытовая-техника, очиститель]\n"
        "source_url: https://example.com/purifier\n"
        "created: 2026-11-23T20:31:00+03:00\n"
        "updated: 2026-11-25T09:00:00+03:00\n"
        "---\n"
        "\n"
        "# Сброс ошибки\n"
        "\n"
        "1. Выключить прибор.\n";

    Meta m;
    const char* body = nullptr;
    size_t body_len = 0;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m, &body, &body_len));

    TEST_ASSERT_EQUAL_STRING("Сброс ошибки очистителя воздуха", m.title);
    TEST_ASSERT_EQUAL_STRING("бытовая-техника, очиститель", m.tags);
    TEST_ASSERT_EQUAL_STRING("https://example.com/purifier", m.source_url);
    TEST_ASSERT_EQUAL_STRING("2026-11-23T20:31:00+03:00", m.created);
    TEST_ASSERT_EQUAL_STRING("2026-11-25T09:00:00+03:00", m.updated);

    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_UINT32(strlen(text) - static_cast<size_t>(body - text), body_len);
    TEST_ASSERT_EQUAL_STRING("# Сброс ошибки\n\n1. Выключить прибор.\n", body);
}

void test_parse_strips_quotes()
{
    const char* text =
        "---\n"
        "title: \"Сброс: ошибки # дома\"\n"
        "---\n"
        "тело\n";

    Meta m;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m));
    // Кавычки — синтаксис YAML, в значение они не входят.
    TEST_ASSERT_EQUAL_STRING("Сброс: ошибки # дома", m.title);
}

void test_parse_unescapes_double_quotes()
{
    const char* text =
        "---\n"
        "title: \"Он сказал \\\"стоп\\\" и ушёл\"\n"
        "---\n";

    Meta m;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m));
    TEST_ASSERT_EQUAL_STRING("Он сказал \"стоп\" и ушёл", m.title);
}

void test_parse_crlf_document()
{
    // Документ мог быть отредактирован на Windows. \r не должен попасть в
    // заголовок: иначе сравнение заголовков и поиск по нему ломаются.
    const char* text =
        "---\r\n"
        "title: Фильтр\r\n"
        "---\r\n"
        "\r\n"
        "тело\r\n";

    Meta m;
    const char* body = nullptr;
    size_t body_len = 0;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m, &body, &body_len));
    TEST_ASSERT_EQUAL_STRING("Фильтр", m.title);
    TEST_ASSERT_TRUE(strstr(body, "тело") != nullptr);
}

void test_parse_ignores_unknown_keys()
{
    // Будущая версия прошивки допишет своё поле — документ не должен из-за
    // этого перестать открываться.
    const char* text =
        "---\n"
        "title: Заметка\n"
        "color: blue\n"
        "priority: 3\n"
        "---\n"
        "тело";

    Meta m;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m));
    TEST_ASSERT_EQUAL_STRING("Заметка", m.title);
}

void test_parse_without_front_matter_returns_body()
{
    // Файл без фронтматтера документом не считается, но показать его надо —
    // иначе пользователь не поймёт, куда делся файл.
    const char* text = "# Просто markdown\n\nБез заголовка.\n";

    Meta m;
    const char* body = nullptr;
    size_t body_len = 0;
    TEST_ASSERT_FALSE(doc::parse(text, strlen(text), m, &body, &body_len));
    TEST_ASSERT_EQUAL_STRING("", m.title);
    TEST_ASSERT_EQUAL_STRING(text, body);
    TEST_ASSERT_EQUAL_UINT32(strlen(text), body_len);
}

void test_parse_unclosed_front_matter_is_not_a_document()
{
    // Началось с «---», а закрывающего нет: это либо обрыв записи, либо текст,
    // случайно начавшийся с черты. Документом не считаем, поля не отдаём и
    // тело — весь файл, чтобы пользователь увидел, что в нём лежит.
    const char* text =
        "---\n"
        "title: Обрыв\n"
        "и дальше сразу текст\n";

    Meta m;
    const char* body = nullptr;
    size_t body_len = 0;
    TEST_ASSERT_FALSE(doc::parse(text, strlen(text), m, &body, &body_len));
    TEST_ASSERT_EQUAL_STRING("", m.title);
    TEST_ASSERT_EQUAL_STRING(text, body);
    TEST_ASSERT_EQUAL_UINT32(strlen(text), body_len);
}

void test_parse_empty_input()
{
    Meta m;
    TEST_ASSERT_FALSE(doc::parse("", 0, m));
    TEST_ASSERT_FALSE(doc::parse(nullptr, 0, m));
}

void test_parse_empty_front_matter()
{
    const char* text = "---\n---\nтело";
    Meta m;
    const char* body = nullptr;
    size_t body_len = 0;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m, &body, &body_len));
    TEST_ASSERT_EQUAL_STRING("", m.title);
    TEST_ASSERT_EQUAL_STRING("тело", body);
    // «тело» — 8 байт UTF-8 (по два на букву).
    TEST_ASSERT_EQUAL_UINT32(8, body_len);
}

void test_parse_skips_single_blank_line_before_body()
{
    // Одна пустая строка после фронтматтера — разделитель по конвенции
    // markdown, а не часть текста. Иначе тело после «собрать → разобрать»
    // начиналось бы с лишнего перевода строки.
    const char* text = "---\ntitle: X\n---\n\nтело";
    Meta m;
    const char* body = nullptr;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m, &body, nullptr));
    TEST_ASSERT_EQUAL_STRING("тело", body);

    // Без пустой строки поведение то же.
    const char* tight = "---\ntitle: X\n---\nтело";
    TEST_ASSERT_TRUE(doc::parse(tight, strlen(tight), m, &body, nullptr));
    TEST_ASSERT_EQUAL_STRING("тело", body);
}

void test_parse_tags_with_quoted_element()
{
    // Запятая — разделитель, поэтому «сад, огород» из написанного руками
    // документа распадётся на два тега. Это осознанное упрощение: иначе
    // невозможно отличить «два тега» от «один тег с запятой» при обратной
    // сборке. Кавычки вокруг элемента при этом снимаются.
    const char* text =
        "---\n"
        "title: Заметка\n"
        "tags: [дом, \"сад\", техника]\n"
        "---\n";

    Meta m;
    TEST_ASSERT_TRUE(doc::parse(text, strlen(text), m));
    TEST_ASSERT_EQUAL_STRING("дом, сад, техника", m.tags);
}

// --- Сборка ----------------------------------------------------------------

void test_build_produces_expected_text()
{
    const Meta m = make_meta();
    char out[1024];
    const size_t n = doc::build(m, "# Тело\n", out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);

    // URL и метки времени не закавычены: двоеточие в них не ломает YAML.
    TEST_ASSERT_EQUAL_STRING(
        "---\n"
        "title: Сброс ошибки очистителя воздуха\n"
        "tags: [бытовая-техника, очиститель]\n"
        "source_url: https://example.com/purifier-error-reset\n"
        "created: 2026-11-23T20:31:00+03:00\n"
        "updated: 2026-11-25T09:00:00+03:00\n"
        "---\n"
        "\n"
        "# Тело\n",
        out);
    TEST_ASSERT_EQUAL_UINT32(strlen(out), n);
}

void test_build_requires_title()
{
    Meta m = make_meta();
    m.title[0] = '\0';
    char out[256];
    TEST_ASSERT_EQUAL_UINT32(0, doc::build(m, "тело", out, sizeof(out)));
}

void test_build_skips_empty_fields()
{
    Meta m{};
    strcpy(m.title, "Только заголовок");
    char out[256];
    TEST_ASSERT_TRUE(doc::build(m, "", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING(
        "---\n"
        "title: Только заголовок\n"
        "---\n",
        out);
}

void test_build_adds_trailing_newline_to_body()
{
    Meta m{};
    strcpy(m.title, "Заметка");
    char out[256];
    doc::build(m, "без перевода строки", out, sizeof(out));
    TEST_ASSERT_TRUE(out[strlen(out) - 1] == '\n');
}

void test_build_quotes_dangerous_values()
{
    Meta m{};
    strcpy(m.title, "Сброс: ошибки # дома");
    char out[512];
    TEST_ASSERT_TRUE(doc::build(m, "", out, sizeof(out)) > 0);
    // Двоеточие перед пробелом и решётка после пробела ломают YAML.
    TEST_ASSERT_NOT_NULL(strstr(out, "title: \"Сброс: ошибки # дома\"\n"));
}

void test_build_keeps_simple_values_unquoted()
{
    // Кавычки «на всякий случай» — шум: документ должен читаться глазами.
    Meta m{};
    strcpy(m.title, "Замена фильтра");
    char out[256];
    doc::build(m, "", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "title: Замена фильтра\n"));
}

void test_build_splits_tags_by_comma()
{
    Meta m{};
    strcpy(m.title, "Заметка");
    strcpy(m.tags, "дом, техника");
    char out[512];
    TEST_ASSERT_TRUE(doc::build(m, "", out, sizeof(out)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "tags: [дом, техника]\n"));
}

void test_build_returns_zero_when_too_small()
{
    const Meta m = make_meta();
    char out[32];
    TEST_ASSERT_EQUAL_UINT32(0, doc::build(m, "тело", out, sizeof(out)));
}

// --- Обратимость -----------------------------------------------------------

void test_build_then_parse_roundtrip()
{
    const Meta m = make_meta();
    char out[1024];
    TEST_ASSERT_TRUE(doc::build(m, "тело документа\n", out, sizeof(out)) > 0);

    Meta back;
    const char* body = nullptr;
    size_t body_len = 0;
    TEST_ASSERT_TRUE(doc::parse(out, strlen(out), back, &body, &body_len));
    TEST_ASSERT_EQUAL_STRING(m.title, back.title);
    TEST_ASSERT_EQUAL_STRING(m.tags, back.tags);
    TEST_ASSERT_EQUAL_STRING(m.source_url, back.source_url);
    TEST_ASSERT_EQUAL_STRING(m.created, back.created);
    TEST_ASSERT_EQUAL_STRING(m.updated, back.updated);
    TEST_ASSERT_TRUE(m == back);

    // Тело вернулось без лишнего перевода строки в начале.
    TEST_ASSERT_EQUAL_STRING("тело документа\n", body);
}

void test_roundtrip_with_dangerous_title()
{
    // Главный риск: заголовок с двоеточием и кавычками. После сборки и разбора
    // он обязан вернуться байт в байт, иначе пользователь увидит в списке
    // обрезанный заголовок.
    Meta m{};
    strcpy(m.title, "Он сказал \"стоп\": и ушёл # домой");
    char out[512];
    TEST_ASSERT_TRUE(doc::build(m, "x", out, sizeof(out)) > 0);

    Meta back;
    TEST_ASSERT_TRUE(doc::parse(out, strlen(out), back));
    TEST_ASSERT_EQUAL_STRING(m.title, back.title);
}

void test_roundtrip_is_stable()
{
    // Второй проход не должен ничего менять: иначе документ «плыл» бы при
    // каждой перезаписи.
    const Meta m = make_meta();
    char first[1024];
    char second[1024];
    TEST_ASSERT_TRUE(doc::build(m, "текст\n", first, sizeof(first)) > 0);

    Meta back;
    const char* body = nullptr;
    TEST_ASSERT_TRUE(doc::parse(first, strlen(first), back, &body, nullptr));
    TEST_ASSERT_TRUE(doc::build(back, body, second, sizeof(second)) > 0);
    TEST_ASSERT_EQUAL_STRING(first, second);
}

// --- Экранирование ---------------------------------------------------------

void test_escape_plain_value()
{
    char out[64];
    // Возвращается длина в БАЙТАХ: «дом» — шесть байт UTF-8.
    TEST_ASSERT_EQUAL_UINT32(6, doc::escape_value("дом", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("дом", out);
}

void test_escape_empty_returns_zero()
{
    // 0 означает «писать нечего»: пустые поля в фронтматтер не попадают.
    char out[64];
    TEST_ASSERT_EQUAL_UINT32(0, doc::escape_value("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(0, doc::escape_value(nullptr, out, sizeof(out)));
}

void test_escape_leading_dash_is_quoted()
{
    // «- пункт» YAML прочитает как начало списка, а не как значение.
    char out[64];
    TEST_ASSERT_TRUE(doc::escape_value("- пункт", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("\"- пункт\"", out);
}

void test_escape_newline_becomes_space()
{
    char out[64];
    TEST_ASSERT_TRUE(doc::escape_value("две\nстроки", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("\"две строки\"", out);
}

void test_escape_leading_space_is_quoted()
{
    char out[64];
    TEST_ASSERT_TRUE(doc::escape_value(" с пробела", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("\" с пробела\"", out);
}

void test_escape_url_stays_plain()
{
    char out[128];
    TEST_ASSERT_TRUE(doc::escape_value("https://example.com/a?b=c#d", out, sizeof(out)) > 0);
    // Двоеточие в URL — не синтаксис YAML (после него не пробел), а решётка
    // в середине — не комментарий. Кавычки здесь были бы ложной тревогой.
    TEST_ASSERT_EQUAL_STRING("https://example.com/a?b=c#d", out);
}

void test_escape_iso_time_stays_plain()
{
    char out[64];
    TEST_ASSERT_TRUE(doc::escape_value("2026-11-23T20:31:00+03:00", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("2026-11-23T20:31:00+03:00", out);
}

void test_escape_column_in_middle_is_quoted()
{
    // «ключ: значение: ещё» развалится, если не закавычить: двоеточие перед
    // пробелом внутри значения YAML считает разделителем.
    char out[64];
    TEST_ASSERT_TRUE(doc::escape_value("итог: готово", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("\"итог: готово\"", out);
}

void test_escape_trailing_colon_is_quoted()
{
    char out[64];
    TEST_ASSERT_TRUE(doc::escape_value("итог:", out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("\"итог:\"", out);
}

// --- Заголовок из тела (Р-050) ----------------------------------------------

void test_title_from_body_first_line()
{
    char out[128];
    const char* body = "Как достать фильтр из пылесоса\n\nПодробности далее.\n";
    const size_t n = doc::title_from_body(body, strlen(body), out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(strlen("Как достать фильтр из пылесоса"), n);
    TEST_ASSERT_EQUAL_STRING("Как достать фильтр из пылесоса", out);
}

void test_title_from_body_skips_leading_blanks()
{
    char out[128];
    // Тело после фронтматтера часто начинается с пустой строки.
    const char* body = "\n   \n\t\nСброс ошибки\r\nдальше текст\n";
    const size_t n = doc::title_from_body(body, strlen(body), out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(strlen("Сброс ошибки"), n);
    // \r в заголовок попасть не должен: он ломает фронтматтер.
    TEST_ASSERT_EQUAL_STRING("Сброс ошибки", out);
}

void test_title_from_body_strips_markup()
{
    char out[128];

    // Заголовок markdown: в списке нужно название, а не «# Название».
    TEST_ASSERT_TRUE(doc::title_from_body("# Замена фильтра\n", 0, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("Замена фильтра", out);

    // Маркер списка — тоже не часть названия.
    TEST_ASSERT_TRUE(doc::title_from_body("- Почистить щёткой\n", 0, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("Почистить щёткой", out);

    // А решётка без пробела — часть текста: «#1 способ» это не заголовок.
    TEST_ASSERT_TRUE(doc::title_from_body("#1 способ\n", 0, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("#1 способ", out);
}

void test_title_from_body_empty_gives_nothing()
{
    char out[128];
    TEST_ASSERT_EQUAL_UINT(0, doc::title_from_body("", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, doc::title_from_body("\n \n\t\n", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0, doc::title_from_body(nullptr, 0, out, sizeof(out)));
    // Одна разметка без текста — тоже не заголовок.
    TEST_ASSERT_EQUAL_UINT(0, doc::title_from_body("#\n", 0, out, sizeof(out)));
}

void test_title_from_body_truncates_on_utf8_boundary()
{
    // «Ж» — 2 байта. Буфер на 6 байт: под текст с многоточием остаётся 2 байта,
    // то есть один целый символ. Обрезка взяла бы полтора — и в списке был бы
    // мусорный глиф вместо буквы.
    char small[6];
    const char* body = "ЖЖЖЖЖЖЖЖ\n";
    const size_t n = doc::title_from_body(body, strlen(body), small, sizeof(small));
    TEST_ASSERT_EQUAL_UINT(5, n);  // «Ж» (2) + «…» (3)
    TEST_ASSERT_EQUAL_MEMORY("Ж…", small, 5);
    TEST_ASSERT_EQUAL_CHAR('\0', small[5]);
}

void test_title_from_body_truncates_at_word_boundary()
{
    // Длинный заголовок обрезается по границе слова и получает многоточие:
    // обрубок «...связь и» читается как настоящий конец названия.
    char out[32];
    const char* body = "Привет. Вижу, что вы пишете одно и то же — возможно\n";
    const size_t n = doc::title_from_body(body, strlen(body), out, sizeof(out));

    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(n + 1 <= sizeof(out));
    // Заканчивается многоточием, а не союзом или половиной буквы.
    TEST_ASSERT_NOT_NULL(strstr(out, "…"));
    TEST_ASSERT_EQUAL_CHAR('\0', out[n]);
    // Последний символ перед многоточием — не пробел и не запятая.
    const size_t before = n - 3;
    TEST_ASSERT_TRUE(before > 0);
    TEST_ASSERT_TRUE(out[before - 1] != ' ' && out[before - 1] != ',');
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_full_document);
    RUN_TEST(test_parse_strips_quotes);
    RUN_TEST(test_parse_unescapes_double_quotes);
    RUN_TEST(test_parse_crlf_document);
    RUN_TEST(test_parse_ignores_unknown_keys);
    RUN_TEST(test_parse_without_front_matter_returns_body);
    RUN_TEST(test_parse_unclosed_front_matter_is_not_a_document);
    RUN_TEST(test_parse_empty_input);
    RUN_TEST(test_parse_empty_front_matter);
    RUN_TEST(test_parse_skips_single_blank_line_before_body);
    RUN_TEST(test_parse_tags_with_quoted_element);

    RUN_TEST(test_build_produces_expected_text);
    RUN_TEST(test_build_requires_title);
    RUN_TEST(test_build_skips_empty_fields);
    RUN_TEST(test_build_adds_trailing_newline_to_body);
    RUN_TEST(test_build_quotes_dangerous_values);
    RUN_TEST(test_build_keeps_simple_values_unquoted);
    RUN_TEST(test_build_splits_tags_by_comma);
    RUN_TEST(test_build_returns_zero_when_too_small);

    RUN_TEST(test_build_then_parse_roundtrip);
    RUN_TEST(test_roundtrip_with_dangerous_title);
    RUN_TEST(test_roundtrip_is_stable);

    RUN_TEST(test_escape_plain_value);
    RUN_TEST(test_escape_empty_returns_zero);
    RUN_TEST(test_escape_leading_dash_is_quoted);
    RUN_TEST(test_escape_newline_becomes_space);
    RUN_TEST(test_escape_leading_space_is_quoted);
    RUN_TEST(test_escape_url_stays_plain);
    RUN_TEST(test_escape_iso_time_stays_plain);
    RUN_TEST(test_escape_column_in_middle_is_quoted);
    RUN_TEST(test_escape_trailing_colon_is_quoted);

    RUN_TEST(test_title_from_body_first_line);
    RUN_TEST(test_title_from_body_skips_leading_blanks);
    RUN_TEST(test_title_from_body_strips_markup);
    RUN_TEST(test_title_from_body_empty_gives_nothing);
    RUN_TEST(test_title_from_body_truncates_on_utf8_boundary);
    RUN_TEST(test_title_from_body_truncates_at_word_boundary);

    return UNITY_END();
}
