/*
 * Тесты поискового подзапроса и сборки результата инструмента web_search.
 *
 * Здесь проверяется то, что можно проверить без платы: тело запроса с плагином
 * и без tools, формат результата, честное «ничего не нашлось» и обработчик,
 * который сам в сеть не ходит.
 */

#include <string.h>
#include <unity.h>

#include <ArduinoJson.h>

#include "util/tool_registry.h"
#include "util/web_search.h"

namespace {

/// JSON тела запроса — разобранный, чтобы проверять поля, а не подстроки.
bool parse_body(const char* body, JsonDocument& doc)
{
    return deserializeJson(doc, body) == DeserializationError::Ok;
}

}  // namespace

// --- Тело поискового запроса ------------------------------------------------

void test_body_has_plugin_and_no_tools()
{
    char body[2048];
    const size_t n = wsearch::build_body("сброс фильтра Xiaomi", "deepseek-v4-flash", 5, body, sizeof(body));
    TEST_ASSERT_GREATER_THAN(0, n);

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));

    // Плагин — единственное место, где он вообще должен появляться (Р-040).
    JsonArray plugins = doc["plugins"];
    TEST_ASSERT_EQUAL_UINT(1, plugins.size());
    TEST_ASSERT_EQUAL_STRING("web", plugins[0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT(5, plugins[0]["max_results"].as<unsigned>());

    // tools нет: иначе поисковый подзапрос мог бы рекурсивно попросить
    // инструмент. Это главная защита от зацикливания.
    TEST_ASSERT_TRUE(doc["tools"].isNull());
}

void test_body_reasoning_disabled()
{
    char body[2048];
    TEST_ASSERT_GREATER_THAN(0, wsearch::build_body("вопрос", "m", 5, body, sizeof(body)));

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));
    // Рассуждения в поиске не нужны, а тарифицируются как выходные токены.
    TEST_ASSERT_EQUAL_STRING("none", doc["reasoning_effort"].as<const char*>());
}

void test_body_stream_requested()
{
    char body[2048];
    TEST_ASSERT_GREATER_THAN(0, wsearch::build_body("вопрос", "m", 5, body, sizeof(body)));

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));
    TEST_ASSERT_TRUE(doc["stream"].as<bool>());
}

void test_body_two_messages_system_and_user()
{
    char body[2048];
    TEST_ASSERT_GREATER_THAN(0, wsearch::build_body("как сбросить фильтр", "m", 5, body, sizeof(body)));

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));

    JsonArray msgs = doc["messages"];
    TEST_ASSERT_EQUAL_UINT(2, msgs.size());
    TEST_ASSERT_EQUAL_STRING("system", msgs[0]["role"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("user", msgs[1]["role"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("как сбросить фильтр", msgs[1]["content"].as<const char*>());
}

void test_body_escapes_query()
{
    char body[2048];
    // Кавычка и перевод строки в запросе: без экранирования JSON развалился бы.
    const size_t n = wsearch::build_body("что такое \"фильтр\" HEPA\n", "m", 5, body, sizeof(body));
    TEST_ASSERT_GREATER_THAN(0, n);

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));
    TEST_ASSERT_EQUAL_STRING("что такое \"фильтр\" HEPA\n", doc["messages"][1]["content"].as<const char*>());
}

void test_body_default_results()
{
    char body[2048];
    TEST_ASSERT_GREATER_THAN(0, wsearch::build_body("вопрос", "m", 0, body, sizeof(body)));

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));
    TEST_ASSERT_EQUAL_UINT(wsearch::kDefaultResults, doc["plugins"][0]["max_results"].as<unsigned>());
}

void test_body_clamps_results()
{
    char body[2048];
    TEST_ASSERT_GREATER_THAN(0, wsearch::build_body("вопрос", "m", 50, body, sizeof(body)));

    JsonDocument doc;
    TEST_ASSERT_TRUE(parse_body(body, doc));
    TEST_ASSERT_EQUAL_UINT(10, doc["plugins"][0]["max_results"].as<unsigned>());
}

void test_body_rejects_empty()
{
    char body[256];
    TEST_ASSERT_EQUAL_UINT(0, wsearch::build_body("", "m", 5, body, sizeof(body)));
    TEST_ASSERT_EQUAL_UINT(0, wsearch::build_body(nullptr, "m", 5, body, sizeof(body)));
    TEST_ASSERT_EQUAL_UINT(0, wsearch::build_body("вопрос", "", 5, body, sizeof(body)));
}

void test_body_small_buffer()
{
    char body[16];
    TEST_ASSERT_EQUAL_UINT(0, wsearch::build_body("вопрос", "m", 5, body, sizeof(body)));
}

// --- Результат инструмента --------------------------------------------------

void test_result_answer_only()
{
    char out[wsearch::kMaxResultBytes + 1];
    const char* answer = "Фильтр меняется раз в год.";
    const size_t n = wsearch::format_result(answer, strlen(answer), nullptr, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(answer, out);
}

void test_result_answer_and_sources()
{
    web::List list;
    list.count = 2;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://mi.com/filter");
    snprintf(list.items[0].title, sizeof(list.items[0].title), "Замена фильтра");
    snprintf(list.items[1].url, sizeof(list.items[1].url), "https://example.org/a");
    snprintf(list.items[1].title, sizeof(list.items[1].title), "Инструкция");

    char out[wsearch::kMaxResultBytes + 1];
    const char* answer = "Кратко о замене.";
    const size_t n = wsearch::format_result(answer, strlen(answer), &list, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);

    // Пересказ идёт первым: модель должна читать факты, а не список ссылок.
    TEST_ASSERT_NOT_NULL(strstr(out, "Кратко о замене."));
    TEST_ASSERT_NOT_NULL(strstr(out, "Источники:"));

    // Полные адреса, а не домены: они нужны для поля source_url заметки.
    TEST_ASSERT_NOT_NULL(strstr(out, "https://mi.com/filter"));
    TEST_ASSERT_NOT_NULL(strstr(out, "https://example.org/a"));

    // Порядок: пересказ, потом источники.
    TEST_ASSERT_TRUE(strstr(out, "Кратко") < strstr(out, "Источники:"));
}

void test_result_sources_only()
{
    web::List list;
    list.count = 1;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://a.example/x");
    snprintf(list.items[0].title, sizeof(list.items[0].title), "Заголовок");

    char out[wsearch::kMaxResultBytes + 1];
    const size_t n = wsearch::format_result(nullptr, 0, &list, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);

    // Пустая строка перед «Источники:» не нужна: пересказа не было.
    TEST_ASSERT_EQUAL_STRING("Источники:\n1. https://a.example/x — Заголовок", out);
}

void test_result_nothing_found_is_explicit()
{
    char out[wsearch::kMaxResultBytes + 1];
    const size_t n = wsearch::format_result(nullptr, 0, nullptr, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);

    // Модель должна узнать о пустом поиске явно, иначе ответит по памяти так,
    // будто поиск что-то дал.
    TEST_ASSERT_NOT_NULL(strstr(out, "ничего не нашёл"));
}

void test_result_uses_url_when_title_empty()
{
    web::List list;
    list.count = 1;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://a.example/x");

    char out[wsearch::kMaxResultBytes + 1];
    TEST_ASSERT_GREATER_THAN(0, wsearch::format_result(nullptr, 0, &list, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "https://a.example/x — https://a.example/x"));
}

void test_result_truncates_by_utf8_boundary()
{
    // Строка из русских букв «а» (D0 B0): обрезка посреди символа дала бы
    // битый UTF-8 в контексте модели.
    char answer[400];
    for (size_t i = 0; i < sizeof(answer) - 2; i += 2) {
        answer[i] = static_cast<char>(0xD0);
        answer[i + 1] = static_cast<char>(0xB0);
    }
    const size_t answer_len = sizeof(answer) - 2;
    answer[answer_len] = '\0';

    char out[64];
    const size_t n = wsearch::format_result(answer, answer_len, nullptr, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(out), n + 1);

    // Хвост — целый символ. Идём к началу последней последовательности: она
    // должна быть длиной ровно 2 байта, иначе последняя буква разрезана.
    size_t last = n;
    while (last > 0 && (static_cast<unsigned char>(out[last - 1]) & 0xC0) == 0x80)
        --last;
    TEST_ASSERT_EQUAL_UINT(2, n - (last - 1));
    TEST_ASSERT_EQUAL_CHAR('\0', out[n]);
}

void test_result_respects_buffer()
{
    web::List list;
    list.count = 3;
    for (size_t i = 0; i < list.count; ++i) {
        snprintf(list.items[i].url, sizeof(list.items[i].url), "https://example.com/very/long/path/%u", (unsigned)i);
        snprintf(list.items[i].title, sizeof(list.items[i].title), "Заголовок номер %u", (unsigned)i);
    }

    char out[80];
    const size_t n = wsearch::format_result("Пересказ.", 9, &list, out, sizeof(out));
    // Хотя бы текст записался, и нуль на месте: обрезали, но не развалили.
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_LESS_THAN(sizeof(out), n + 1);
    TEST_ASSERT_EQUAL_CHAR('\0', out[n]);
}

// --- Обработчик инструмента -------------------------------------------------

void test_tool_defers_and_asks_search()
{
    tools::Context ctx;
    char out[512];
    ctx.out = out;
    ctx.out_cap = sizeof(out);

    const char* args = "{\"query\": \"  сброс фильтра Xiaomi  \", \"max_results\": 7}";
    const bool ok = wsearch::web_search_tool(args, strlen(args), ctx);

    // Результата ещё нет: сеть — дело экрана, а не обработчика.
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(ctx.needs_search);
    TEST_ASSERT_TRUE(ctx.deferred);
    // Ведущие пробелы срезаны: поисковик на них отвечает хуже.
    TEST_ASSERT_EQUAL_STRING("сброс фильтра Xiaomi", ctx.search_query);
    TEST_ASSERT_EQUAL_UINT(7, ctx.search_max_results);
}

void test_tool_unspecified_results()
{
    tools::Context ctx;
    char out[256];
    ctx.out = out;
    ctx.out_cap = sizeof(out);

    const char* args = "{\"query\": \"вопрос\"}";
    TEST_ASSERT_FALSE(wsearch::web_search_tool(args, strlen(args), ctx));
    TEST_ASSERT_TRUE(ctx.needs_search);
    // 0 — «модель не назвала число»: экран подставит настройку «Источников в
    // ответе». Иначе этот пункт в Ctrl+A ни на что бы не влиял.
    TEST_ASSERT_EQUAL_UINT(0, ctx.search_max_results);
}

void test_tool_clamps_results()
{
    tools::Context ctx;
    char out[256];
    ctx.out = out;
    ctx.out_cap = sizeof(out);

    const char* args = "{\"query\": \"вопрос\", \"max_results\": 99}";
    TEST_ASSERT_FALSE(wsearch::web_search_tool(args, strlen(args), ctx));
    TEST_ASSERT_EQUAL_UINT(10, ctx.search_max_results);
}

void test_tool_without_query_is_error()
{
    tools::Context ctx;
    char out[256];
    ctx.out = out;
    ctx.out_cap = sizeof(out);

    const char* args = "{}";
    TEST_ASSERT_FALSE(wsearch::web_search_tool(args, strlen(args), ctx));
    TEST_ASSERT_FALSE(ctx.needs_search);
    TEST_ASSERT_NOT_NULL(strstr(out, "Ошибка"));
    // В отказе есть пример правильного значения: по нему модель исправляется.
    TEST_ASSERT_NOT_NULL(strstr(out, "Пример"));
}

void test_tool_empty_query_is_error()
{
    tools::Context ctx;
    char out[256];
    ctx.out = out;
    ctx.out_cap = sizeof(out);

    const char* args = "{\"query\": \"   \"}";
    TEST_ASSERT_FALSE(wsearch::web_search_tool(args, strlen(args), ctx));
    TEST_ASSERT_FALSE(ctx.needs_search);
    TEST_ASSERT_NOT_NULL(strstr(out, "Ошибка"));
}

void test_tool_schema_is_valid_json()
{
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, wsearch::kToolSchema);
    TEST_ASSERT_EQUAL_STRING("Ok", err.c_str());
    TEST_ASSERT_EQUAL_STRING("web_search", doc["name"].as<const char*>());
    TEST_ASSERT_TRUE(doc["parameters"]["properties"]["query"].is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_STRING("query", doc["parameters"]["required"][0].as<const char*>());
}

void test_tool_registered_in_registry()
{
    tools::Registry reg;
    TEST_ASSERT_TRUE(wsearch::register_tool(reg));
    TEST_ASSERT_NOT_NULL(reg.find("web_search"));
    // Дважды тот же инструмент — отказ, а не дубль в схеме.
    TEST_ASSERT_FALSE(wsearch::register_tool(reg));
}

void test_registry_skips_named_tool()
{
    tools::Registry reg;
    TEST_ASSERT_TRUE(wsearch::register_tool(reg));

    char all[4096];
    TEST_ASSERT_TRUE(reg.build_tools_json(all, sizeof(all)));
    TEST_ASSERT_NOT_NULL(strstr(all, "web_search"));

    char without[4096];
    TEST_ASSERT_TRUE(reg.build_tools_json(without, sizeof(without), "web_search"));

    // Выключенный поиск — это отсутствующий инструмент, а не поле в запросе.
    TEST_ASSERT_NULL(strstr(without, "web_search"));
    TEST_ASSERT_EQUAL_STRING("[]", without);
}

void test_result_skips_sources_from_earlier_searches()
{
    // Список источников один на весь ход: первый поиск уже положил два, второй
    // добавил один. Результат второго поиска не должен выдавать чужие ссылки за
    // свои — иначе «ничего не нашёл» никогда не наступит.
    web::List list;
    list.count = 3;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://old.example/1");
    snprintf(list.items[0].title, sizeof(list.items[0].title), "Старый");
    snprintf(list.items[1].url, sizeof(list.items[1].url), "https://old.example/2");
    snprintf(list.items[1].title, sizeof(list.items[1].title), "Тоже старый");
    snprintf(list.items[2].url, sizeof(list.items[2].url), "https://new.example/3");
    snprintf(list.items[2].title, sizeof(list.items[2].title), "Новый");

    char out[wsearch::kMaxResultBytes + 1];
    const size_t n = wsearch::format_result(nullptr, 0, &list, out, sizeof(out), 2);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "https://new.example/3"));
    TEST_ASSERT_NULL(strstr(out, "https://old.example/1"));
    // Нумерация своя, с единицы: это первый источник ЭТОГО поиска.
    TEST_ASSERT_NOT_NULL(strstr(out, "1. https://new.example/3"));
}

void test_result_marks_second_search_as_empty()
{
    web::List list;
    list.count = 1;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://old.example/1");

    char out[wsearch::kMaxResultBytes + 1];
    const size_t n = wsearch::format_result(nullptr, 0, &list, out, sizeof(out), 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    // Все источники — от прошлого поиска. Значит этот ничего не нашёл, и
    // модель должна услышать это прямо.
    TEST_ASSERT_NOT_NULL(strstr(out, "ничего не нашёл"));
}

void test_result_list_from_beyond_count_is_safe()
{
    web::List list;
    list.count = 1;
    snprintf(list.items[0].url, sizeof(list.items[0].url), "https://a.example/1");

    char out[wsearch::kMaxResultBytes + 1];
    // Отметка больше списка — так быть не должно, но выйти за границу массива
    // хуже, чем соврать: читаем как «новых источников нет».
    const size_t n = wsearch::format_result(nullptr, 0, &list, out, sizeof(out), 5);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "ничего не нашёл"));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_body_has_plugin_and_no_tools);
    RUN_TEST(test_body_reasoning_disabled);
    RUN_TEST(test_body_stream_requested);
    RUN_TEST(test_body_two_messages_system_and_user);
    RUN_TEST(test_body_escapes_query);
    RUN_TEST(test_body_default_results);
    RUN_TEST(test_body_clamps_results);
    RUN_TEST(test_body_rejects_empty);
    RUN_TEST(test_body_small_buffer);

    RUN_TEST(test_result_answer_only);
    RUN_TEST(test_result_answer_and_sources);
    RUN_TEST(test_result_sources_only);
    RUN_TEST(test_result_nothing_found_is_explicit);
    RUN_TEST(test_result_uses_url_when_title_empty);
    RUN_TEST(test_result_truncates_by_utf8_boundary);
    RUN_TEST(test_result_respects_buffer);
    RUN_TEST(test_result_skips_sources_from_earlier_searches);
    RUN_TEST(test_result_marks_second_search_as_empty);
    RUN_TEST(test_result_list_from_beyond_count_is_safe);

    RUN_TEST(test_tool_defers_and_asks_search);
    RUN_TEST(test_tool_unspecified_results);
    RUN_TEST(test_tool_clamps_results);
    RUN_TEST(test_tool_without_query_is_error);
    RUN_TEST(test_tool_empty_query_is_error);
    RUN_TEST(test_tool_schema_is_valid_json);
    RUN_TEST(test_tool_registered_in_registry);
    RUN_TEST(test_registry_skips_named_tool);

    return UNITY_END();
}
