/*
 * Тесты разбора источников веб-поиска и сборки поля plugins.
 *
 * Что здесь закрывается:
 *   - форма `annotations` из живого ответа RouterAI (Р-036) — разбор,
 *     выжимка, обрезка по границе UTF-8;
 *   - дедупликация: источники приходят одним чанком, но это наблюдение,
 *     а не гарантия;
 *   - `max_results`: вне диапазона 1..8 поднимаем/опускаем до границы,
 *     потому что 0 сервер заменит своим умолчанием;
 *   - домен для подписи: без схемы, без `www.`, без порта.
 */

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "util/web_sources.h"

void setUp(void) {}
void tearDown(void) {}

// Ответ ровно в том виде, в каком его прислал RouterAI (сокращённый content).
static const char* kChunk =
    R"J({"choices":[{"index":0,"delta":{"annotations":[
        {"type":"url_citation","url_citation":{
            "url":"https://www.mi.com/ru/support/article/KA-14885/",
            "title":"Почему Mi Air Purifier предложил заменить фильтр",
            "content":"Почему Mi Air Purifier\n\nпредложил заменить новый фильтр"},
         "start_index":0,"end_index":10}]}}]})J";

void test_feed_parses_url_citation()
{
    web::List list;
    const size_t added = web::feed(kChunk, strlen(kChunk), list);

    TEST_ASSERT_EQUAL_UINT(1, added);
    TEST_ASSERT_EQUAL_UINT(1, list.count);
    TEST_ASSERT_EQUAL_STRING("https://www.mi.com/ru/support/article/KA-14885/", list.items[0].url);
    TEST_ASSERT_EQUAL_STRING("Почему Mi Air Purifier предложил заменить фильтр", list.items[0].title);
}

void test_feed_flattens_multiline_content()
{
    web::List list;
    web::feed(kChunk, strlen(kChunk), list);

    // content пришёл с "\n\n" — в чате источник занимает одну строку.
    TEST_ASSERT_EQUAL_STRING("Почему Mi Air Purifier предложил заменить новый фильтр", list.items[0].snippet);
}

void test_feed_reads_message_annotations_too()
{
    // Без стриминга источники лежат в message, а не в delta — обе формы.
    const char* json =
        R"J({"choices":[{"message":{"annotations":[
            {"type":"url_citation","url_citation":{"url":"https://a.ru/x","title":"А"}}]}}]})J";
    web::List list;
    TEST_ASSERT_EQUAL_UINT(1, web::feed(json, strlen(json), list));
    TEST_ASSERT_EQUAL_STRING("https://a.ru/x", list.items[0].url);
}

void test_feed_ignores_chunk_without_annotations()
{
    const char* json = R"J({"choices":[{"delta":{"content":"привет"}}]})J";
    web::List list;
    TEST_ASSERT_EQUAL_UINT(0, web::feed(json, strlen(json), list));
    TEST_ASSERT_EQUAL_UINT(0, list.count);
}

void test_feed_skips_annotation_without_url()
{
    const char* json =
        R"J({"choices":[{"delta":{"annotations":[
            {"type":"url_citation","url_citation":{"title":"без адреса"}}]}}]})J";
    web::List list;
    TEST_ASSERT_EQUAL_UINT(0, web::feed(json, strlen(json), list));
}

void test_feed_deduplicates_same_url()
{
    // Если плагин однажды пришлёт список дважды, пользователь не должен
    // увидеть источник дважды.
    web::List list;
    web::feed(kChunk, strlen(kChunk), list);
    web::feed(kChunk, strlen(kChunk), list);

    TEST_ASSERT_EQUAL_UINT(1, list.count);
    TEST_ASSERT_EQUAL_UINT(1, list.dropped);
}

void test_feed_stops_at_limit()
{
    web::List list;
    // На один больше предела: лишний должен посчитаться в dropped.
    for (size_t i = 0; i <= web::kMaxSources; ++i) {
        char json[256];
        snprintf(json, sizeof(json),
            R"J({"choices":[{"delta":{"annotations":[{"url_citation":{"url":"https://x.ru/%d"}}]}}]})J", i);
        web::feed(json, strlen(json), list);
    }
    TEST_ASSERT_EQUAL_UINT(web::kMaxSources, list.count);
    TEST_ASSERT_EQUAL_UINT(1, list.dropped);
}

void test_feed_tolerates_broken_json()
{
    web::List list;
    TEST_ASSERT_EQUAL_UINT(0, web::feed("{не json", 8, list));
    TEST_ASSERT_EQUAL_UINT(0, web::feed(nullptr, 0, list));
}

void test_feed_accepts_flat_url_field()
{
    // Второй возможный вид: поля на верхнем уровне, без url_citation.
    const char* json = R"J({"choices":[{"delta":{"annotations":[{"url":"https://b.ru/y","title":"Б"}]}}]})J";
    web::List list;
    TEST_ASSERT_EQUAL_UINT(1, web::feed(json, strlen(json), list));
    TEST_ASSERT_EQUAL_STRING("https://b.ru/y", list.items[0].url);
}

void test_mentions_annotations()
{
    TEST_ASSERT_TRUE(web::mentions_annotations(kChunk, strlen(kChunk)));
    // Пустой массив — поиск был, но ничего не нашёл: поле есть.
    const char* empty = R"J({"choices":[{"delta":{"annotations":[]}}]})J";
    TEST_ASSERT_TRUE(web::mentions_annotations(empty, strlen(empty)));
    const char* none = R"J({"choices":[{"delta":{"content":"ок"}}]})J";
    TEST_ASSERT_FALSE(web::mentions_annotations(none, strlen(none)));
    TEST_ASSERT_FALSE(web::mentions_annotations("мусор", 5));
}

void test_append_merges_without_duplicates()
{
    // За ход агента ответов бывает несколько, и второй часто ссылается на те
    // же страницы. Показывать их дважды нельзя.
    web::List a;
    web::List b;
    web::feed(kChunk, strlen(kChunk), a);

    const char* second =
        R"J({"choices":[{"delta":{"annotations":[
            {"url_citation":{"url":"https://www.mi.com/ru/support/article/KA-14885/"}},
            {"url_citation":{"url":"https://habr.com/ru/articles/789836/"}}]}}]})J";
    web::feed(second, strlen(second), b);

    TEST_ASSERT_EQUAL_UINT(1, web::append(a, b));
    TEST_ASSERT_EQUAL_UINT(2, a.count);
    TEST_ASSERT_EQUAL_STRING("https://habr.com/ru/articles/789836/", a.items[1].url);
}

void test_append_counts_overflow()
{
    web::List a;
    for (int i = 0; i < web::kMaxSources; ++i) {
        char json[192];
        snprintf(json, sizeof(json),
            R"J({"choices":[{"delta":{"annotations":[{"url_citation":{"url":"https://a.ru/%d"}}]}}]})J", i);
        web::feed(json, strlen(json), a);
    }
    TEST_ASSERT_EQUAL_UINT(web::kMaxSources, a.count);

    web::List b;
    const char* extra = R"J({"choices":[{"delta":{"annotations":[{"url_citation":{"url":"https://b.ru/new"}}]}}]})J";
    web::feed(extra, strlen(extra), b);

    TEST_ASSERT_EQUAL_UINT(0, web::append(a, b));
    TEST_ASSERT_EQUAL_UINT(1, a.dropped);
}

// --- Поле plugins ---
void test_build_plugins_default()
{
    char out[96];
    const size_t n = web::build_plugins_json(3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(R"J([{"id":"web","max_results":3}])J", out);
    TEST_ASSERT_EQUAL_UINT(strlen(out), n);
}

void test_build_plugins_clamps_zero()
{
    // 0 значило бы «ноль результатов»; сервер молча подставит своё умолчание,
    // а мы хотим предсказуемость.
    char out[96];
    web::build_plugins_json(0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(R"J([{"id":"web","max_results":1}])J", out);
}

void test_build_plugins_clamps_above_limit()
{
    char out[96];
    web::build_plugins_json(50, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(R"J([{"id":"web","max_results":10}])J", out);
}

void test_build_plugins_small_buffer()
{
    char out[16];
    TEST_ASSERT_EQUAL_UINT(0, web::build_plugins_json(3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

// --- Адрес для фронтматтера ---

void test_primary_url_none_when_empty()
{
    web::List list;
    TEST_ASSERT_NULL(web::primary_url(list));
}

void test_primary_url_is_first()
{
    web::List list;
    web::feed(kChunk, strlen(kChunk), list);
    TEST_ASSERT_EQUAL_STRING("https://www.mi.com/ru/support/article/KA-14885/", web::primary_url(list));
}

// --- Подпись источника ---

void test_domain_strips_scheme_www_and_path()
{
    char out[64];
    web::domain_of("https://www.mi.com/ru/support/article/KA-14885/", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("mi.com", out);

    web::domain_of("http://example.org", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("example.org", out);

    web::domain_of("habr.com/post", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("habr.com", out);
}

void test_domain_strips_port()
{
    char out[64];
    web::domain_of("http://localhost:8080/x", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("localhost", out);
}

void test_domain_empty_on_garbage()
{
    char out[64];
    TEST_ASSERT_EQUAL_UINT(0, web::domain_of("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, web::domain_of(nullptr, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0, web::domain_of("https:///path", out, sizeof(out)));
}

void test_format_source_with_title()
{
    web::Source src;
    strcpy(src.url, "https://www.mi.com/ru/x");
    strcpy(src.title, "Как сбросить фильтр");

    char out[256];
    web::format_source(src, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("mi.com — Как сбросить фильтр", out);
}

void test_format_source_without_title()
{
    web::Source src;
    strcpy(src.url, "https://habr.com/post");

    char out[256];
    web::format_source(src, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("habr.com", out);
}

void test_format_source_keeps_domain_when_title_too_long()
{
    web::Source src;
    strcpy(src.url, "https://habr.com/post");
    memset(src.title, 'A', sizeof(src.title) - 1);

    char out[32];
    web::format_source(src, out, sizeof(out));
    // Не влезло целиком — отдаём хотя бы домен, а не обрывок заголовка.
    TEST_ASSERT_EQUAL_STRING("habr.com", out);
}

// --- Обрезка по границе UTF-8 ---

// Строка — корректный UTF-8? Нужно именно это: обрезка не должна оставить
// недописанный многобайтовый символ в конце.
static bool utf8_valid(const char* s)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0) {
        size_t len = 0;
        if (*p < 0x80)
            len = 1;
        else if ((*p & 0xE0) == 0xC0)
            len = 2;
        else if ((*p & 0xF0) == 0xE0)
            len = 3;
        else if ((*p & 0xF8) == 0xF0)
            len = 4;
        else
            return false;
        for (size_t i = 1; i < len; ++i) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += len;
    }
    return true;
}

void test_long_russian_title_is_not_cut_mid_character()
{
    // Заголовок из русских букв (2 байта каждая) длиннее буфера: обрезка
    // должна встать на границу символа, иначе в конце окажется «квадратик».
    char json[2048];
    char title[280];
    size_t p = 0;
    for (int i = 0; i < 130; ++i) {
        title[p++] = static_cast<char>(0xD0);
        title[p++] = static_cast<char>(0x90 + (i % 32));  // кириллица
    }
    title[p] = '\0';
    snprintf(json, sizeof(json),
        R"J({"choices":[{"delta":{"annotations":[{"url_citation":{"url":"https://x.ru/1","title":"%s"}}]}}]})J", title);

    web::List list;
    TEST_ASSERT_EQUAL_UINT(1, web::feed(json, strlen(json), list));

    const char* kept = list.items[0].title;
    const size_t len = strlen(kept);
    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_LESS_OR_EQUAL(web::kMaxTitleLen - 1, len);
    // Обрезка не оставила недописанный символ.
    TEST_ASSERT_TRUE(utf8_valid(kept));
    // И это правда обрезка, а не целый заголовок.
    TEST_ASSERT_LESS_THAN((int)strlen(title), (int)len);
}

/// Полный список из десяти источников обязан влезть в kMaxListText целиком:
/// иначе буфер экрана и предел источников разъедутся, и список будет
/// обрываться на «…» ровно тогда, когда пользователь попросил десять.
///
/// Считаем ХУДШИЙ случай по пределам структуры: заголовок `kMaxTitleLen - 1`
/// байт и самый длинный домен, какой влезает в подпись. Точный расчёт:
/// 6 + 63 + 5 + 127 = 200 байт на строку, 2021 байт на десять.
void test_format_list_fits_all_sources()
{
    web::List list;
    for (size_t i = 0; i < web::kMaxSources; ++i) {
        // Заголовок максимальной длины — худший случай. Кириллица по 2 байта,
        // поэтому предел в байтах, а не в символах.
        size_t n = 0;
        while (n + 2 <= web::kMaxTitleLen - 1) {
            list.items[i].title[n++] = static_cast<char>(0xD0);
            list.items[i].title[n++] = static_cast<char>(0xB0);
        }
        list.items[i].title[n] = '\0';
        snprintf(list.items[i].url, sizeof(list.items[i].url), "https://site%d.example.com/some/path/that/is/long/%d",
            (int)i, (int)i);
    }
    list.count = web::kMaxSources;

    static char out[web::kMaxListText];
    const size_t len = web::format_list(list, out, sizeof(out));

    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_EQUAL_UINT(len, strlen(out));
    // Ни многоточия, ни обрыва: все десять пунктов на месте.
    TEST_ASSERT_NULL(strstr(out, "\xe2\x80\xa6"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\n10. "));
    TEST_ASSERT_EQUAL_STRING("\n\nИсточники:", strndup(out, strlen("\n\nИсточники:")));
    // Запас обязателен: буфер не должен быть «впритык» к худшему случаю.
    TEST_ASSERT_GREATER_THAN((int)len + 100, (int)sizeof(out));
}

/// Обрезка идёт по границе пункта: последняя строка не рвётся посередине и
/// список помечен многоточием, а не выглядит законченным.
void test_format_list_truncates_on_item_boundary()
{
    web::List list;
    for (size_t i = 0; i < web::kMaxSources; ++i) {
        memset(list.items[i].title, 'b', web::kMaxTitleLen - 1);
        list.items[i].title[web::kMaxTitleLen - 1] = '\0';
        snprintf(list.items[i].url, sizeof(list.items[i].url), "https://s%d.example.com/", (int)i);
    }
    list.count = web::kMaxSources;

    char out[160];
    const size_t len = web::format_list(list, out, sizeof(out));

    TEST_ASSERT_GREATER_THAN(0, (int)len);
    TEST_ASSERT_EQUAL_UINT(len, strlen(out));
    // Обрыв помечен, и он — последняя строка.
    TEST_ASSERT_NOT_NULL(strstr(out, "\n\xe2\x80\xa6"));
    TEST_ASSERT_TRUE(strcmp(out + len - 4, "\n\xe2\x80\xa6") == 0);
    // Ни один пункт не обрезан посередине: все строки кончаются заголовком.
    TEST_ASSERT_NULL(strstr(out, "…b"));
}

/// Пустой список — пустой текст, а не заголовок «Источники:» без пунктов.
void test_format_list_empty()
{
    web::List list;
    char out[256];
    TEST_ASSERT_EQUAL_UINT(0, web::format_list(list, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, web::format_list(list, nullptr, 0));
}

/// Меньше заголовка не помещается — честный ноль, а не обрывок.
void test_format_list_small_buffer()
{
    web::List list;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://a.ru/");
    list.count = 1;

    char out[8];
    TEST_ASSERT_EQUAL_UINT(0, web::format_list(list, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_feed_parses_url_citation);
    RUN_TEST(test_feed_flattens_multiline_content);
    RUN_TEST(test_feed_reads_message_annotations_too);
    RUN_TEST(test_feed_ignores_chunk_without_annotations);
    RUN_TEST(test_feed_skips_annotation_without_url);
    RUN_TEST(test_feed_deduplicates_same_url);
    RUN_TEST(test_feed_stops_at_limit);
    RUN_TEST(test_feed_tolerates_broken_json);
    RUN_TEST(test_feed_accepts_flat_url_field);
    RUN_TEST(test_mentions_annotations);
    RUN_TEST(test_append_merges_without_duplicates);
    RUN_TEST(test_append_counts_overflow);
    RUN_TEST(test_build_plugins_default);
    RUN_TEST(test_build_plugins_clamps_zero);
    RUN_TEST(test_build_plugins_clamps_above_limit);
    RUN_TEST(test_build_plugins_small_buffer);
    RUN_TEST(test_primary_url_none_when_empty);
    RUN_TEST(test_primary_url_is_first);
    RUN_TEST(test_domain_strips_scheme_www_and_path);
    RUN_TEST(test_domain_strips_port);
    RUN_TEST(test_domain_empty_on_garbage);
    RUN_TEST(test_format_source_with_title);
    RUN_TEST(test_format_source_without_title);
    RUN_TEST(test_format_source_keeps_domain_when_title_too_long);
    RUN_TEST(test_long_russian_title_is_not_cut_mid_character);
    RUN_TEST(test_format_list_fits_all_sources);
    RUN_TEST(test_format_list_truncates_on_item_boundary);
    RUN_TEST(test_format_list_empty);
    RUN_TEST(test_format_list_small_buffer);
    return UNITY_END();
}
