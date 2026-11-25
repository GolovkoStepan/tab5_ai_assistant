/*
 * Тесты склейки tool_calls и фильтра DSML.
 *
 * Склейка дельт — место, где ошибиться глазами невозможно: короткий вызов
 * приходит за 12 чанков, и «на глаз» от обрывка JSON не отличить. Поэтому
 * тесты построены на реальных чанках, снятых с живого RouterAI
 * (scripts/tool_call_smoke.py), а не на придуманных.
 */

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>
#include <unity.h>

#include "util/tool_calls.h"

void setUp(void) {}
void tearDown(void) {}

// --- Склейка tool_calls -----------------------------------------------------

// Чанки как их прислал RouterAI для вызова read_doc: id в первом, name целиком,
// аргументы кусками. Значения дословно из живого прогона (Р-025).
void test_accumulator_glues_argument_pieces()
{
    tools::Accumulator acc;
    acc.reset();

    const char* chunks[] = {
        R"({"choices":[{"index":0,"delta":{"role":"assistant","content":"","tool_calls":[{"index":0,"id":"call_00_575ide6xoainqaqbv2sd435y","type":"function","function":{"name":"read_doc","arguments":""}}]}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"pa"}}]}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\": \"ho"}}]}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"me/air-"}}]}}]})",
        R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"purifier.md\"}"}}]}}]})",
    };

    for (const char* c : chunks)
        TEST_ASSERT_TRUE(acc.feed(c, strlen(c)));

    TEST_ASSERT_EQUAL_UINT32(1, acc.count());
    TEST_ASSERT_EQUAL_STRING("call_00_575ide6xoainqaqbv2sd435y", acc.at(0).id);
    TEST_ASSERT_EQUAL_STRING("read_doc", acc.at(0).name);
    TEST_ASSERT_EQUAL_STRING(R"({"path": "home/air-purifier.md"})", acc.at(0).arguments);
    TEST_ASSERT_EQUAL_STRING("", acc.error());
}

// Модель умеет просить два инструмента за один ход. Индексы приходят 0 и 1,
// куски могут чередоваться — накопитель обязан разложить их по слотам.
void test_accumulator_splits_two_calls_by_index()
{
    tools::Accumulator acc;
    acc.reset();

    const char* chunks[] = {
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","function":{"name":"search_docs","arguments":"{\"que"}}]}}]})",
        R"({"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","function":{"name":"read_doc","arguments":"{\"path\":"}}]}}]})",
        // продолжение ПЕРВОГО вызова приходит после начала второго
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"ry\":\"фильтр\"}"}}]}}]})",
        R"({"choices":[{"delta":{"tool_calls":[{"index":1,"function":{"arguments":"\"home/x.md\"}"}}]}}]})",
    };

    for (const char* c : chunks)
        TEST_ASSERT_TRUE(acc.feed(c, strlen(c)));

    TEST_ASSERT_EQUAL_UINT32(2, acc.count());
    TEST_ASSERT_EQUAL_STRING("search_docs", acc.at(0).name);
    TEST_ASSERT_EQUAL_STRING(R"({"query":"фильтр"})", acc.at(0).arguments);
    TEST_ASSERT_EQUAL_STRING("read_doc", acc.at(1).name);
    TEST_ASSERT_EQUAL_STRING(R"({"path":"home/x.md"})", acc.at(1).arguments);
}

// Id повторяется в нескольких чанках (наблюдали в живом прогоне). Второй раз
// дописывать его нельзя: получилось бы «call_acall_a».
void test_accumulator_does_not_duplicate_id()
{
    tools::Accumulator acc;
    acc.reset();

    const char* a =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read_doc"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(a, strlen(a)));
    TEST_ASSERT_TRUE(acc.feed(a, strlen(a)));

    TEST_ASSERT_EQUAL_STRING("call_1", acc.at(0).id);
    TEST_ASSERT_EQUAL_STRING("read_doc", acc.at(0).name);
}

// Имя инструмента тоже может прийти частями («read_» + «doc») или повтором
// с начала («read_» → «read_doc»). Склеивать целиком нельзя: получилось бы
// «read_read_doc».
void test_accumulator_glues_name_pieces()
{
    tools::Accumulator acc;
    acc.reset();

    const char* p1 = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c","function":{"name":"read_"}}]}}]})";
    const char* p2 = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"doc"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(p1, strlen(p1)));
    TEST_ASSERT_TRUE(acc.feed(p2, strlen(p2)));
    TEST_ASSERT_EQUAL_STRING("read_doc", acc.at(0).name);

    // А теперь имя пришло с начала и целиком — замена, а не дописывание.
    const char* full = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"read_doc"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(full, strlen(full)));
    TEST_ASSERT_EQUAL_STRING("read_doc", acc.at(0).name);
}

// Без stream:true сервер шлёт ту же структуру, но в message, а не delta.
// Парсер обязан понимать обе формы, иначе ответ без стриминга потеряет вызов.
void test_accumulator_reads_message_form()
{
    tools::Accumulator acc;
    acc.reset();

    const char* full =
        R"({"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"call_x","type":"function","function":{"name":"write_doc","arguments":"{\"path\":\"a.md\"}"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(full, strlen(full)));

    TEST_ASSERT_EQUAL_UINT32(1, acc.count());
    TEST_ASSERT_EQUAL_STRING("write_doc", acc.at(0).name);
    TEST_ASSERT_EQUAL_STRING(R"({"path":"a.md"})", acc.at(0).arguments);
}

// Чанк без tool_calls — обычный текст. Накопитель не должен ни падать, ни
// считать это ошибкой.
void test_accumulator_ignores_text_chunks()
{
    tools::Accumulator acc;
    acc.reset();

    const char* text = R"({"choices":[{"delta":{"content":"Привет"}}]})";
    TEST_ASSERT_TRUE(acc.feed(text, strlen(text)));
    TEST_ASSERT_FALSE(acc.any());
    TEST_ASSERT_EQUAL_STRING("", acc.error());
}

void test_accumulator_ignores_broken_json()
{
    tools::Accumulator acc;
    acc.reset();

    // Обрывок чанка: поток не должен рваться из-за него.
    TEST_ASSERT_FALSE(acc.feed("{\"choices\":[", 12));
    TEST_ASSERT_FALSE(acc.any());

    // А следующий нормальный чанк по-прежнему принимается.
    const char* good =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read_doc"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(good, strlen(good)));
    TEST_ASSERT_EQUAL_UINT32(1, acc.count());
}

// Переполнение аргументов не должно портить соседние вызовы: ошибка пишется,
// а то, что уже собрано, остаётся целым.
void test_accumulator_reports_argument_overflow()
{
    tools::Accumulator acc;
    acc.reset();

    char buf[700];
    int written = snprintf(buf, sizeof(buf),
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"write_doc","arguments":"{"}}]}}]})");
    TEST_ASSERT_TRUE(acc.feed(buf, static_cast<size_t>(written)));

    // Досыпаем куски по 500 байт, пока не переполним kMaxArgsLen. Число
    // кусков считается от предела, а не зашито: предел менялся (2 КБ → 4 КБ
    // под русский текст документа), и тест не должен ломаться от этого.
    char chunk[512];
    memset(chunk, 'a', sizeof(chunk) - 1);
    chunk[sizeof(chunk) - 1] = '\0';
    const int chunks = static_cast<int>(tools::kMaxArgsLen / 400) + 1;
    for (int i = 0; i < chunks; ++i) {
        written = snprintf(buf, sizeof(buf),
            R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"%s"}}]}}]})", chunk);
        TEST_ASSERT_TRUE(written > 0 && static_cast<size_t>(written) < sizeof(buf));
        TEST_ASSERT_TRUE(acc.feed(buf, static_cast<size_t>(written)));
    }

    TEST_ASSERT_TRUE(acc.error()[0] != '\0');
    TEST_ASSERT_EQUAL_UINT32(1, acc.count());
    TEST_ASSERT_EQUAL_STRING("write_doc", acc.at(0).name);
    // Начало аргументов на месте, не затёрто переполнением.
    TEST_ASSERT_EQUAL_CHAR('{', acc.at(0).arguments[0]);
}

void test_accumulator_stops_at_call_limit()
{
    tools::Accumulator acc;
    acc.reset();

    char buf[256];
    for (size_t i = 0; i <= tools::kMaxCallsPerTurn; ++i) {
        const int written = snprintf(buf, sizeof(buf),
            R"({"choices":[{"delta":{"tool_calls":[{"index":%u,"id":"c%u","function":{"name":"read_doc"}}]}}]})",
            (unsigned)i, (unsigned)i);
        TEST_ASSERT_TRUE(acc.feed(buf, static_cast<size_t>(written)));
    }

    TEST_ASSERT_EQUAL_UINT32(tools::kMaxCallsPerTurn, acc.count());
    TEST_ASSERT_TRUE(acc.error()[0] != '\0');
}

void test_accumulator_reset_starts_over()
{
    tools::Accumulator acc;
    acc.reset();

    const char* c =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read_doc"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(c, strlen(c)));
    TEST_ASSERT_EQUAL_UINT32(1, acc.count());

    acc.reset();
    TEST_ASSERT_FALSE(acc.any());
    TEST_ASSERT_EQUAL_STRING("", acc.error());

    // Новый ход может содержать вызов с тем же индексом — это другой вызов.
    const char* d =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_2","function":{"name":"search_docs"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(d, strlen(d)));
    TEST_ASSERT_EQUAL_UINT32(1, acc.count());
    TEST_ASSERT_EQUAL_STRING("call_2", acc.at(0).id);
}

// --- Фильтр DSML ------------------------------------------------------------

void test_dsml_marker_length()
{
    // '<', два символа U+FF5C по 3 байта и четыре латинские буквы.
    TEST_ASSERT_EQUAL_UINT32(11, tools::DsmlFilter::markerLen());
}

void test_dsml_passes_clean_text()
{
    tools::DsmlFilter f;
    f.reset();

    char out[128];
    const char* text = "Сброс ошибки: держите кнопку 5 секунд.";
    const size_t n = f.feed(text, strlen(text), out, sizeof(out));
    out[n] = '\0';
    TEST_ASSERT_EQUAL_UINT32(strlen(text), n);
    TEST_ASSERT_EQUAL_STRING(text, out);
    TEST_ASSERT_FALSE(f.triggered());
}

// Главный случай: маркер пришёл в тексте, всё после него — машинная разметка.
void test_dsml_cuts_at_marker()
{
    tools::DsmlFilter f;
    f.reset();

    char out[256];
    const char* text =
        "Инструмент вернул пустой ответ.\n\n"
        "<\xEF\xBD\x9C\xEF\xBD\x9C"
        "DSML\xEF\xBD\x9C\xEF\xBD\x9C calls>\n"
        "<\xEF\xBD\x9C\xEF\xBD\x9C"
        "DSML\xEF\xBD\x9C\xEF\xBD\x9C invoke name=\"read_doc\">";
    const size_t n = f.feed(text, strlen(text), out, sizeof(out));
    out[n] = '\0';

    TEST_ASSERT_TRUE(f.triggered());
    TEST_ASSERT_EQUAL_STRING("Инструмент вернул пустой ответ.\n\n", out);
}

// Маркер может разъехаться по границе чанков: дельты бывают по 2–3 байта,
// а маркер — 10. Фильтр обязан склеить и всё равно распознать.
void test_dsml_marker_split_across_chunks()
{
    const char* text =
        "Готово.<\xEF\xBD\x9C\xEF\xBD\x9C"
        "DSML\xEF\xBD\x9C\xEF\xBD\x9C calls>мусор";

    for (size_t piece = 1; piece <= 12; ++piece) {
        tools::DsmlFilter f;
        f.reset();

        char out[256];
        size_t written = 0;
        const size_t total = strlen(text);
        for (size_t i = 0; i < total; i += piece) {
            const size_t take = (total - i) < piece ? (total - i) : piece;
            written += f.feed(text + i, take, out + written, sizeof(out) - written);
        }
        out[written] = '\0';

        TEST_ASSERT_TRUE(f.triggered());
        TEST_ASSERT_EQUAL_STRING("Готово.", out);
    }
}

// Хвост, который так и не стал маркером, обязан выйти в flush(). Иначе у
// ответа откусывались бы последние байты: фильтр держит до 9 байт «на всякий».
void test_dsml_flush_returns_pending_tail()
{
    tools::DsmlFilter f;
    f.reset();

    char out[64];
    // Заканчивается на «<», а маркером не является.
    const char* text = "Температура <";
    size_t written = f.feed(text, strlen(text), out, sizeof(out));
    written += f.flush(out + written, sizeof(out) - written);
    out[written] = '\0';

    TEST_ASSERT_EQUAL_STRING(text, out);
    TEST_ASSERT_FALSE(f.triggered());
}

// Обычный текст с угловыми скобками и латиницей не должен выглядеть маркером.
void test_dsml_does_not_trip_on_similar_text()
{
    tools::DsmlFilter f;
    f.reset();

    char out[256];
    const char* text = "Код: <div>DSML</div> и <DSML> — не маркер, нужны вертикальные черты.";
    const size_t n = f.feed(text, strlen(text), out, sizeof(out));
    out[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(text, out);
    TEST_ASSERT_EQUAL_UINT32(strlen(text), n);
    TEST_ASSERT_FALSE(f.triggered());
}

void test_dsml_flush_is_empty_after_trigger()
{
    tools::DsmlFilter f;
    f.reset();

    char out[64];
    const char* text =
        "ок<\xEF\xBD\x9C\xEF\xBD\x9C"
        "DSML\xEF\xBD\x9C\xEF\xBD\x9C x";
    f.feed(text, strlen(text), out, sizeof(out));
    TEST_ASSERT_TRUE(f.triggered());

    // После срабатывания отдавать нечего — иначе мусор просочился бы в конце.
    TEST_ASSERT_EQUAL_UINT32(0, f.flush(out, sizeof(out)));
}

// Несколько чанков подряд: обычный текст, потом маркер. Проверяем, что
// накопление между вызовами работает и текст не дублируется.
void test_dsml_across_multiple_feeds()
{
    tools::DsmlFilter f;
    f.reset();

    char out[128];
    size_t written = 0;
    const char* parts[] = {"Ответ: ", "всё ", "хорошо", ".<\xEF\xBD\x9C\xEF\xBD",
        "\x9C"
        "DSML\xEF\xBD\x9C\xEF\xBD\x9C calls>"};
    for (const char* p : parts)
        written += f.feed(p, strlen(p), out + written, sizeof(out) - written);
    out[written] = '\0';

    TEST_ASSERT_TRUE(f.triggered());
    TEST_ASSERT_EQUAL_STRING("Ответ: всё хорошо.", out);
}

// --- Сборка массива вызовов -------------------------------------------------

/// Массив, который уходит обратно в API в сообщении ассистента. Он обязан
/// быть валидным JSON и содержать id, имя и аргументы — иначе модель не
/// свяжет свой вызов с результатом.
void test_build_calls_json_produces_valid_array()
{
    tools::Accumulator acc;
    acc.reset();

    const char* chunk =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read_doc","arguments":"{\"path\": \"home/x.md\"}"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(chunk, strlen(chunk)));

    char out[512];
    const size_t n = tools::build_calls_json(acc, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, n);

    JsonDocument doc;
    TEST_ASSERT_EQUAL_STRING("Ok", deserializeJson(doc, out).c_str());
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT32(1, arr.size());

    JsonObjectConst call = arr[0];
    TEST_ASSERT_EQUAL_STRING("call_1", call["id"] | "");
    TEST_ASSERT_EQUAL_STRING("function", call["type"] | "");
    TEST_ASSERT_EQUAL_STRING("read_doc", call["function"]["name"] | "");

    // Аргументы — СТРОКА с JSON внутри, а не вложенный объект: ровно в таком
    // виде их присылает сервер и ждёт обратно.
    const char* args = call["function"]["arguments"] | "";
    JsonDocument inner;
    TEST_ASSERT_EQUAL_STRING("Ok", deserializeJson(inner, args).c_str());
    TEST_ASSERT_EQUAL_STRING("home/x.md", inner["path"] | "");
}

/// Вызов без id получает синтетический: сервер не прислал id — не повод
/// отправлять сообщение tool с пустой ссылкой.
void test_build_calls_json_invents_id_when_missing()
{
    tools::Accumulator acc;
    acc.reset();

    const char* chunk = R"({"choices":[{"delta":{"tool_calls":[{"index":2,"function":{"name":"list_docs"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(chunk, strlen(chunk)));

    char out[256];
    TEST_ASSERT_GREATER_THAN(0, tools::build_calls_json(acc, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"id\":\"call_2\""));
    // Аргументов не было — уходит пустой объект, а не пустая строка.
    TEST_ASSERT_NOT_NULL(strstr(out, "\"arguments\":\"{}\""));
}

/// Кавычки и переводы строк в тексте аргументов должны быть экранированы:
/// иначе массив перестанет быть JSON, и сервер ответит 400 на всю историю.
void test_build_calls_json_escapes_quotes()
{
    tools::Accumulator acc;
    acc.reset();

    const char* chunk =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c","function":{"name":"write_doc","arguments":"{\"content\":\"строка 1\\nстрока \\\"2\\\"\"}"}}]}}]})";
    TEST_ASSERT_TRUE(acc.feed(chunk, strlen(chunk)));

    char out[512];
    TEST_ASSERT_GREATER_THAN(0, tools::build_calls_json(acc, out, sizeof(out)));

    JsonDocument doc;
    TEST_ASSERT_EQUAL_STRING("Ok", deserializeJson(doc, out).c_str());

    // А аргументы после развнешнения обязаны снова стать разбираемым JSON.
    const char* args = doc[0]["function"]["arguments"] | "";
    JsonDocument inner;
    TEST_ASSERT_EQUAL_STRING("Ok", deserializeJson(inner, args).c_str());

    // Перевод строки и кавычки дошли до модели в исходном виде: двойное
    // экранирование (массив → строка аргументов → JSON внутри) съело бы их.
    const char* content = inner["content"] | "";
    TEST_ASSERT_NOT_NULL(strstr(content, "строка 1"));
    TEST_ASSERT_NOT_NULL(strchr(content, '\n'));
    TEST_ASSERT_NOT_NULL(strchr(content, '"'));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_accumulator_glues_argument_pieces);
    RUN_TEST(test_accumulator_splits_two_calls_by_index);
    RUN_TEST(test_accumulator_does_not_duplicate_id);
    RUN_TEST(test_accumulator_glues_name_pieces);
    RUN_TEST(test_accumulator_reads_message_form);
    RUN_TEST(test_accumulator_ignores_text_chunks);
    RUN_TEST(test_accumulator_ignores_broken_json);
    RUN_TEST(test_accumulator_reports_argument_overflow);
    RUN_TEST(test_accumulator_stops_at_call_limit);
    RUN_TEST(test_accumulator_reset_starts_over);

    RUN_TEST(test_dsml_marker_length);
    RUN_TEST(test_dsml_passes_clean_text);
    RUN_TEST(test_dsml_cuts_at_marker);
    RUN_TEST(test_dsml_marker_split_across_chunks);
    RUN_TEST(test_dsml_flush_returns_pending_tail);
    RUN_TEST(test_dsml_does_not_trip_on_similar_text);
    RUN_TEST(test_dsml_flush_is_empty_after_trigger);
    RUN_TEST(test_dsml_across_multiple_feeds);

    RUN_TEST(test_build_calls_json_produces_valid_array);
    RUN_TEST(test_build_calls_json_invents_id_when_missing);
    RUN_TEST(test_build_calls_json_escapes_quotes);

    return UNITY_END();
}
