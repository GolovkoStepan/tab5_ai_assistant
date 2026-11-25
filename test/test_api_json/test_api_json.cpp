/*
 * Тесты сборки запроса и разбора ответа API.
 *
 * Формат тела запроса проверяется сравнением с эталонной строкой: это
 * единственный способ заметить пропавшую запятую или незакрытую кавычку,
 * которые на плате выглядят как «модель не ответила».
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/api_json.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

char g_buf[8192];

api::OutMessage user_msg(const char* text)
{
    api::OutMessage m;
    m.role = api::Role::User;
    m.content = text;
    return m;
}

api::RequestOptions options(const char* model = "test/model")
{
    api::RequestOptions o;
    o.model = model;
    o.temperature_tenths = 3;
    return o;
}

}  // namespace

// --- Сборка запроса ---

void test_request_one_message()
{
    const api::OutMessage msgs[] = {user_msg("привет")};
    const size_t n = api::build_request(msgs, 1, options(), g_buf, sizeof(g_buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING(
        "{\"model\":\"test/model\",\"messages\":[{\"role\":\"user\",\"content\":\"привет\"}],"
        "\"stream\":true,\"temperature\":0.3}",
        g_buf);
}

/// Температура хранится десятыми: в JSON она обязана быть числом, а не
/// строкой, и с одной цифрой после точки — иначе разные прошивки будут
/// присылать 0.30000000000000004.
void test_temperature_is_number_with_one_digit()
{
    api::RequestOptions o = options();
    o.temperature_tenths = 12;

    const size_t n = api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"temperature\":1.2"));
    TEST_ASSERT_NULL(strstr(g_buf, "\"temperature\":\""));
}

void test_temperature_zero()
{
    api::RequestOptions o = options();
    o.temperature_tenths = 0;

    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"temperature\":0.0"));
}

void test_stream_flag_false()
{
    api::RequestOptions o = options();
    o.stream = false;

    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"stream\":false"));
}

/// reasoning_effort у RouterAI — строка. Число сервер молча проигнорирует,
/// и «размышления» окажутся выключены без единого сообщения об ошибке.
void test_reasoning_effort_as_string()
{
    api::RequestOptions o = options();
    o.reasoning_effort = "low";

    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"reasoning_effort\":\"low\""));
}

void test_reasoning_effort_omitted_when_null()
{
    api::RequestOptions o = options();
    o.reasoning_effort = nullptr;

    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NULL(strstr(g_buf, "reasoning_effort"));
}

void test_max_tokens_only_when_set()
{
    api::RequestOptions o = options();
    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NULL(strstr(g_buf, "max_tokens"));

    o.max_tokens = 2048;
    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"max_tokens\":2048"));
}

/// Кавычки и переводы строк в тексте должны быть экранированы, иначе тело
/// запроса перестанет быть JSON при первом же ответе с цитатой.
void test_content_is_escaped()
{
    const char* text = "он сказал \"да\"\nи ушёл \\ домой";
    const api::OutMessage msgs[] = {user_msg(text)};

    TEST_ASSERT_GREATER_THAN(0, api::build_request(msgs, 1, options(), g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\\\"да\\\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\\\\"));
}

void test_multiple_messages_in_order()
{
    const api::OutMessage msgs[] = {user_msg("первое"), user_msg("второе")};

    TEST_ASSERT_GREATER_THAN(0, api::build_request(msgs, 2, options(), g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(
        strstr(g_buf, "{\"role\":\"user\",\"content\":\"первое\"},{\"role\":\"user\",\"content\":\"второе\"}"));
}

/// Результат инструмента без tool_call_id сервер отклонит: он не знает, на
/// какой вызов отвечает это сообщение.
void test_tool_message_carries_call_id()
{
    api::OutMessage tool;
    tool.role = api::Role::Tool;
    tool.content = "найдено 3 документа";
    tool.tool_call_id = "call_1";
    tool.name = "search_docs";

    TEST_ASSERT_GREATER_THAN(0, api::build_request(&tool, 1, options(), g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"role\":\"tool\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"tool_call_id\":\"call_1\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"name\":\"search_docs\""));
}

void test_null_content_becomes_empty_string()
{
    api::OutMessage msg;
    msg.role = api::Role::Assistant;
    msg.content = nullptr;

    TEST_ASSERT_GREATER_THAN(0, api::build_request(&msg, 1, options(), g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"content\":\"\""));
}

/// Маленький буфер — честный отказ, а не обрезанный JSON: обрезанное тело
/// сервер отклонит с 400, и причина будет выглядеть загадочно.
void test_small_buffer_returns_zero()
{
    const api::OutMessage msgs[] = {user_msg("длинный текст, который не влезет")};

    TEST_ASSERT_EQUAL_UINT(0, api::build_request(msgs, 1, options(), g_buf, 32));
}

void test_null_model_returns_zero()
{
    api::RequestOptions o = options(nullptr);
    TEST_ASSERT_EQUAL_UINT(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
}

// --- Разбор чанка ---

void test_chunk_delta_text()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"Привет\"},\"index\":0}]}";
    char text[64] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_text);
    TEST_ASSERT_EQUAL_STRING("Привет", text);
    // text_len — в байтах: это длина записанного в буфер, а не число символов.
    TEST_ASSERT_EQUAL_UINT32(12, chunk.text_len);
}

/// Роль приходит первым чанком, текста в нём нет. Это не ошибка: иначе первый
/// чанк считался бы пустым ответом.
void test_chunk_role_only()
{
    const char* json = "{\"choices\":[{\"delta\":{\"role\":\"assistant\"},\"index\":0}]}";
    char text[64] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_FALSE(chunk.has_text);
    TEST_ASSERT_FALSE(chunk.has_finish);
}

/// `finish_reason: null` в промежуточных чанках — норма. Если считать null
/// завершением, ответ обрывался бы после первого же куска.
void test_chunk_null_finish_reason_ignored()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"а\"},\"finish_reason\":null}]}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_FALSE(chunk.has_finish);
}

void test_chunk_finish_reason_stop()
{
    const char* json = "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_finish);
    TEST_ASSERT_EQUAL_STRING("stop", chunk.finish_reason);
}

/// Достигнут лимит токенов — ответ обрезан, и в чате это надо показать иначе,
/// чем естественное завершение.
void test_chunk_finish_reason_length()
{
    const char* json = "{\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("length", chunk.finish_reason);
}

/// RouterAI присылает usage с именами input_tokens/output_tokens.
void test_chunk_usage_routerai_names()
{
    const char* json = "{\"choices\":[],\"usage\":{\"input_tokens\":120,\"output_tokens\":45,\"total_tokens\":165}}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_usage);
    TEST_ASSERT_EQUAL_INT(120, chunk.input_tokens);
    TEST_ASSERT_EQUAL_INT(45, chunk.output_tokens);
}

/// OpenAI-совместимые клиенты и другие провайдеры отдают prompt_tokens и
/// completion_tokens. Без этой ветки счётчик на них показывал бы нули.
void test_chunk_usage_openai_names()
{
    const char* json = "{\"choices\":[],\"usage\":{\"prompt_tokens\":300,\"completion_tokens\":70}}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_usage);
    TEST_ASSERT_EQUAL_INT(300, chunk.input_tokens);
    TEST_ASSERT_EQUAL_INT(70, chunk.output_tokens);
}

void test_chunk_without_usage()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"x\"}}]}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_FALSE(chunk.has_usage);
}

/// Промежуточные чанки присылают `"usage":null` явно. Это не ошибка и не
/// повод считать, что usage пришёл.
void test_chunk_null_usage_ignored()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"x\"}}],\"usage\":null}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_FALSE(chunk.has_usage);
}

/// Проверено на живом RouterAI: при reasoning_effort != "none" модель сначала
/// стримит рассуждения в delta.reasoning, и только потом текст ответа. Пауза
/// доходит до полутора секунд — без индикатора экран выглядит зависшим.
void test_chunk_reasoning_separate_from_text()
{
    const char* json =
        "{\"choices\":[{\"delta\":{\"content\":\"\",\"role\":\"assistant\",\"reasoning\":\"We need answer\"}}]}";
    char text[64] = {};
    char reasoning[64] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text), reasoning, sizeof(reasoning)));
    TEST_ASSERT_FALSE(chunk.has_text);
    TEST_ASSERT_TRUE(chunk.has_reasoning);
    TEST_ASSERT_EQUAL_STRING("We need answer", reasoning);
    TEST_ASSERT_EQUAL_UINT32(14, chunk.reasoning_len);
}

/// Пустая строка рассуждений в первом чанке — не повод показывать «думаю».
void test_chunk_empty_reasoning_ignored()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"\",\"reasoning\":\"\"}}]}";
    char text[16] = {};
    char reasoning[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text), reasoning, sizeof(reasoning)));
    TEST_ASSERT_FALSE(chunk.has_reasoning);
    TEST_ASSERT_FALSE(chunk.has_text);
}

/// Если буфера под рассуждения нет, они просто не копятся, а текст ответа
/// разбирается как обычно.
void test_chunk_reasoning_skipped_without_buffer()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"ответ\",\"reasoning\":\"думаю\"}}]}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text), nullptr, 0));
    TEST_ASSERT_TRUE(chunk.has_text);
    TEST_ASSERT_FALSE(chunk.has_reasoning);
    TEST_ASSERT_EQUAL_STRING("ответ", text);
}

/// Токены рассуждений тарифицируются как выходные, и при reasoning_effort они
/// бывают больше самого ответа: 48 против 12 в живом ответе. Без отдельного
/// счётчика «почему так дорого» не объяснить.
void test_chunk_reasoning_tokens_parsed()
{
    const char* json =
        "{\"choices\":[],\"usage\":{\"prompt_tokens\":38,\"completion_tokens\":60,"
        "\"completion_tokens_details\":{\"reasoning_tokens\":48}}}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_usage);
    TEST_ASSERT_EQUAL_INT(38, chunk.input_tokens);
    TEST_ASSERT_EQUAL_INT(60, chunk.output_tokens);
    TEST_ASSERT_EQUAL_INT(48, chunk.reasoning_tokens);
}

/// Буфер текста меньше дельты: текст обрезается по буферу, а не пишется за
/// его пределами.
void test_chunk_text_truncated_to_buffer()
{
    const char* json = "{\"choices\":[{\"delta\":{\"content\":\"abcdefghij\"}}]}";
    char text[5] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("abcd", text);
    TEST_ASSERT_EQUAL_UINT32(4, chunk.text_len);
}

void test_chunk_broken_json_returns_false()
{
    const char* json = "{\"choices\":[";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_FALSE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
}

void test_chunk_not_object_returns_false()
{
    const char* json = "[1,2,3]";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_FALSE(api::parse_chunk(json, strlen(json), chunk, text, sizeof(text)));
}

// --- Полный ответ без стриминга ---

/// Если сервер проигнорирует stream:true, SSE-парсер не увидит ни одного
/// события, и экран провисит до таймаута. Разбираем такой ответ как обычный.
void test_full_response_text_and_finish()
{
    const char* json =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"красный\"},"
        "\"finish_reason\":\"stop\"}]}";
    char text[64] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_full_response(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_text);
    TEST_ASSERT_EQUAL_STRING("красный", text);
    TEST_ASSERT_TRUE(chunk.has_finish);
    TEST_ASSERT_EQUAL_STRING("stop", chunk.finish_reason);
}

/// Рассуждения в полном ответе — отдельное поле message.reasoning. Если их не
/// отделить, они попадут в чат как ответ модели.
void test_full_response_reasoning_separate()
{
    const char* json =
        "{\"choices\":[{\"message\":{\"content\":\"привет\",\"reasoning\":\"надо поздороваться\"},"
        "\"finish_reason\":\"stop\"}]}";
    char text[64] = {};
    char reasoning[64] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(
        api::parse_full_response(json, strlen(json), chunk, text, sizeof(text), reasoning, sizeof(reasoning)));
    TEST_ASSERT_EQUAL_STRING("привет", text);
    TEST_ASSERT_TRUE(chunk.has_reasoning);
    TEST_ASSERT_EQUAL_STRING("надо поздороваться", reasoning);
    TEST_ASSERT_TRUE(chunk.has_text);
}

void test_full_response_usage()
{
    const char* json =
        "{\"choices\":[{\"message\":{\"content\":\"ок\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":38,\"completion_tokens\":60,"
        "\"completion_tokens_details\":{\"reasoning_tokens\":48}}}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_TRUE(api::parse_full_response(json, strlen(json), chunk, text, sizeof(text)));
    TEST_ASSERT_TRUE(chunk.has_usage);
    TEST_ASSERT_EQUAL_INT(38, chunk.input_tokens);
    TEST_ASSERT_EQUAL_INT(60, chunk.output_tokens);
    TEST_ASSERT_EQUAL_INT(48, chunk.reasoning_tokens);
}

/// Тело не похоже на ответ модели (например, {"object":"list"} от /models).
void test_full_response_rejects_non_completion()
{
    const char* json = "{\"object\":\"list\",\"data\":[]}";
    char text[16] = {};
    api::Chunk chunk;

    TEST_ASSERT_FALSE(api::parse_full_response(json, strlen(json), chunk, text, sizeof(text)));
}

// --- Ошибки ---

void test_error_plain_string()
{
    const char* json = "{\"error\":\"Invalid API key\"}";
    char out[96];

    TEST_ASSERT_TRUE(api::parse_error(json, strlen(json), 401, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Invalid API key", out);
}

void test_error_nested_message()
{
    const char* json = "{\"error\":{\"message\":\"Модель не найдена\",\"code\":404}}";
    char out[96];

    TEST_ASSERT_TRUE(api::parse_error(json, strlen(json), 404, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Модель не найдена", out);
}

/// Пустое тело (или HTML от прокси) — не повод показывать на экране мусор:
/// подставляем текст по коду.
void test_error_empty_body_uses_code_text()
{
    char out[96];

    TEST_ASSERT_FALSE(api::parse_error("", 0, 401, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Неверный API-ключ", out);
}

void test_error_html_body_uses_code_text()
{
    char out[96];
    const char* html = "<html><body>502 Bad Gateway</body></html>";

    TEST_ASSERT_FALSE(api::parse_error(html, strlen(html), 502, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Сервис недоступен", out);
}

void test_error_402_balance()
{
    char out[96];

    api::parse_error("", 0, 402, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Закончился баланс", out);
}

/// tools передаются массивом как есть: реестр собирает схему, builder только
/// переносит её в тело запроса.
void test_tools_are_passed_through()
{
    api::RequestOptions o = options();
    o.tools_json = "[{\"type\": \"function\", \"function\": {\"name\": \"read_doc\"}}]";

    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"tools\":[{\"type\": \"function\""));
}

void test_tools_omitted_when_null()
{
    api::RequestOptions o = options();
    o.tools_json = nullptr;
    TEST_ASSERT_GREATER_THAN(0, api::build_request(nullptr, 0, o, g_buf, sizeof(g_buf)));
    TEST_ASSERT_NULL(strstr(g_buf, "\"tools\""));
}

/// Ассистент с вызовами: content = null, tool_calls — массив. Это форма,
/// которую RouterAI ждёт при возврате результата инструмента (Р-025).
void test_assistant_message_with_tool_calls()
{
    api::OutMessage msgs[1];
    msgs[0].role = api::Role::Assistant;
    msgs[0].content = nullptr;
    msgs[0].tool_calls_json =
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_doc\",\"arguments\":\"{}\"}}]";

    TEST_ASSERT_GREATER_THAN(0, api::build_request(msgs, 1, options(), g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"content\":null"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"tool_calls\":[{\"id\":\"call_1\""));
}

/// Ответ инструмента: role=tool с tool_call_id. Без идентификатора модель не
/// сопоставит результат с вызовом.
void test_tool_message_has_call_id()
{
    api::OutMessage msgs[1];
    msgs[0].role = api::Role::Tool;
    msgs[0].content = "Создан документ home/x.md";
    msgs[0].tool_call_id = "call_abc";
    msgs[0].name = "write_doc";

    TEST_ASSERT_GREATER_THAN(0, api::build_request(msgs, 1, options(), g_buf, sizeof(g_buf)));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"role\":\"tool\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"tool_call_id\":\"call_abc\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"name\":\"write_doc\""));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_one_message);
    RUN_TEST(test_temperature_is_number_with_one_digit);
    RUN_TEST(test_temperature_zero);
    RUN_TEST(test_stream_flag_false);
    RUN_TEST(test_reasoning_effort_as_string);
    RUN_TEST(test_reasoning_effort_omitted_when_null);

    RUN_TEST(test_tools_are_passed_through);
    RUN_TEST(test_tools_omitted_when_null);
    RUN_TEST(test_assistant_message_with_tool_calls);
    RUN_TEST(test_tool_message_has_call_id);
    RUN_TEST(test_max_tokens_only_when_set);
    RUN_TEST(test_content_is_escaped);
    RUN_TEST(test_multiple_messages_in_order);
    RUN_TEST(test_tool_message_carries_call_id);
    RUN_TEST(test_null_content_becomes_empty_string);
    RUN_TEST(test_small_buffer_returns_zero);
    RUN_TEST(test_null_model_returns_zero);
    RUN_TEST(test_chunk_delta_text);
    RUN_TEST(test_chunk_role_only);
    RUN_TEST(test_chunk_null_finish_reason_ignored);
    RUN_TEST(test_chunk_finish_reason_stop);
    RUN_TEST(test_chunk_finish_reason_length);
    RUN_TEST(test_chunk_usage_routerai_names);
    RUN_TEST(test_chunk_usage_openai_names);
    RUN_TEST(test_chunk_without_usage);
    RUN_TEST(test_chunk_null_usage_ignored);
    RUN_TEST(test_chunk_reasoning_separate_from_text);
    RUN_TEST(test_chunk_empty_reasoning_ignored);
    RUN_TEST(test_chunk_reasoning_skipped_without_buffer);
    RUN_TEST(test_chunk_reasoning_tokens_parsed);
    RUN_TEST(test_chunk_text_truncated_to_buffer);
    RUN_TEST(test_chunk_broken_json_returns_false);
    RUN_TEST(test_chunk_not_object_returns_false);
    RUN_TEST(test_full_response_text_and_finish);
    RUN_TEST(test_full_response_reasoning_separate);
    RUN_TEST(test_full_response_usage);
    RUN_TEST(test_full_response_rejects_non_completion);
    RUN_TEST(test_error_plain_string);
    RUN_TEST(test_error_nested_message);
    RUN_TEST(test_error_empty_body_uses_code_text);
    RUN_TEST(test_error_html_body_uses_code_text);
    RUN_TEST(test_error_402_balance);
    return UNITY_END();
}
