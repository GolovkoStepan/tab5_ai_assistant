/*
 * Тесты строки треда (JSONL).
 *
 * Главное, что здесь проверяется, — не «собралось ли», а совместимость с
 * файлом: строка, записанная одной версией прошивки, должна читаться следующей.
 * Тред — единственное, что у пользователя нельзя восстановить из интернета.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/chat_line.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

char g_buf[4096];
char g_content[4096];

}  // namespace

// --- Сборка ---

void test_build_user_message()
{
    const size_t n = chatline::build(api::Role::User, "привет", nullptr, chatline::Usage{}, g_buf, sizeof(g_buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("{\"role\":\"user\",\"content\":\"привет\"}", g_buf);
}

void test_build_with_timestamp()
{
    const size_t n = chatline::build(
        api::Role::User, "привет", "2026-11-24T12:00:00+03:00", chatline::Usage{}, g_buf, sizeof(g_buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"ts\":\"2026-11-24T12:00:00+03:00\""));
}

void test_build_assistant_with_usage()
{
    chatline::Usage usage;
    usage.input_tokens = 1200;
    usage.output_tokens = 340;

    const size_t n = chatline::build(api::Role::Assistant, "ответ", nullptr, usage, g_buf, sizeof(g_buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"usage\":{\"input\":1200,\"output\":340}"));
}

/// Пометка неполного ответа — единственный след того, что пользователь прервал
/// стриминг. Без неё оборванный ответ выглядит как законченный.
void test_build_partial_answer()
{
    chatline::Usage usage;
    usage.partial = true;

    const size_t n = chatline::build(api::Role::Assistant, "не до конца", nullptr, usage, g_buf, sizeof(g_buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"partial\":true"));
}

/// У пользовательского сообщения usage быть не должно: это поля ответа модели.
void test_build_user_has_no_usage()
{
    chatline::Usage usage;
    usage.input_tokens = 100;
    usage.output_tokens = 50;

    const size_t n = chatline::build(api::Role::User, "вопрос", nullptr, usage, g_buf, sizeof(g_buf));

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NULL(strstr(g_buf, "usage"));
}

/// Кавычки и переводы строк в тексте должны экранироваться, иначе файл треда
/// перестанет быть JSONL при первом же ответе с цитатой.
void test_build_escapes_content()
{
    const char* text = "он сказал \"да\"\nи ушёл \\ домой";
    TEST_ASSERT_GREATER_THAN(
        0, chatline::build(api::Role::Assistant, text, nullptr, chatline::Usage{}, g_buf, sizeof(g_buf)));

    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\\\"да\\\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\\\\"));
    // Внутри строки не должно быть настоящего перевода строки: иначе одна
    // запись развалится на две, и вторая перестанет быть JSON.
    TEST_ASSERT_NULL(strchr(g_buf, '\n'));
}

/// Маленький буфер — честный отказ, а не обрезанная строка: обрезанная строка
/// навсегда останется битой в файле треда.
void test_build_small_buffer_returns_zero()
{
    TEST_ASSERT_EQUAL_UINT32(
        0, chatline::build(api::Role::User, "длинный текст", nullptr, chatline::Usage{}, g_buf, 16));
}

void test_build_null_content_is_empty()
{
    const size_t n = chatline::build(api::Role::Assistant, nullptr, nullptr, chatline::Usage{}, g_buf, sizeof(g_buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("{\"role\":\"assistant\",\"content\":\"\"}", g_buf);
}

// --- Разбор ---

void test_parse_user_message()
{
    char line[] = "{\"role\":\"user\",\"content\":\"как сбросить ошибку?\"}";
    chatline::Entry e;

    TEST_ASSERT_TRUE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(api::Role::User), static_cast<uint8_t>(e.role));
    TEST_ASSERT_EQUAL_STRING("как сбросить ошибку?", e.content);
    TEST_ASSERT_EQUAL_UINT32(strlen("как сбросить ошибку?"), e.content_len);
    TEST_ASSERT_EQUAL_STRING("", e.ts);
}

void test_parse_assistant_role()
{
    char line[] = "{\"role\":\"assistant\",\"content\":\"вот ответ\"}";
    chatline::Entry e;

    TEST_ASSERT_TRUE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(api::Role::Assistant), static_cast<uint8_t>(e.role));
    TEST_ASSERT_EQUAL_STRING("вот ответ", e.content);
}

void test_parse_usage()
{
    char line[] = "{\"role\":\"assistant\",\"content\":\"ок\",\"usage\":{\"input\":38,\"output\":60}}";
    chatline::Entry e;

    TEST_ASSERT_TRUE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
    TEST_ASSERT_EQUAL_INT(38, e.input_tokens);
    TEST_ASSERT_EQUAL_INT(60, e.output_tokens);
}

void test_parse_partial_flag()
{
    char line[] = "{\"role\":\"assistant\",\"content\":\"обрыв\",\"partial\":true}";
    chatline::Entry e;

    TEST_ASSERT_TRUE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
    TEST_ASSERT_TRUE(e.partial);
}

/// Служебная первая строка файла (`{"type":"meta",...}`) — не сообщение.
/// Считать её ошибкой значило бы выкинуть весь тред.
void test_parse_meta_line_skipped()
{
    char line[] = "{\"type\":\"meta\",\"title\":\"Очиститель\",\"created\":\"2026-11-24T12:00:00+03:00\"}";
    chatline::Entry e;

    TEST_ASSERT_FALSE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
}

void test_parse_broken_line_returns_false()
{
    char line[] = "{\"role\":\"user\",\"content\":\"начал и не за";
    chatline::Entry e;

    TEST_ASSERT_FALSE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
}

void test_parse_line_without_role_returns_false()
{
    char line[] = "{\"content\":\"без роли\"}";
    chatline::Entry e;

    TEST_ASSERT_FALSE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
}

/// Неизвестная роль из будущей версии — показываем как пользовательскую, а не
/// теряем сообщение.
void test_parse_unknown_role_falls_back_to_user()
{
    char line[] = "{\"role\":\"future_role\",\"content\":\"текст\"}";
    chatline::Entry e;

    TEST_ASSERT_TRUE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(api::Role::User), static_cast<uint8_t>(e.role));
    TEST_ASSERT_EQUAL_STRING("текст", e.content);
}

/// Строка без content — не повод падать: пустой контент, сообщение остаётся.
void test_parse_missing_content_is_empty()
{
    char line[] = "{\"role\":\"assistant\"}";
    chatline::Entry e;

    TEST_ASSERT_TRUE(chatline::parse(line, strlen(line), e, g_content, sizeof(g_content)));
    TEST_ASSERT_EQUAL_STRING("", e.content);
}

/// Текст не помещается в буфер — честный отказ. Молча обрезанный ответ модели
/// неотличим от законченного, поэтому такой строке не место в ленте.
void test_parse_small_content_buffer_returns_false()
{
    char line[] = "{\"role\":\"assistant\",\"content\":\"длинный ответ модели\"}";
    char small[8];
    chatline::Entry e;

    TEST_ASSERT_FALSE(chatline::parse(line, strlen(line), e, small, sizeof(small)));
}

/// Круг «собрали → разобрали» должен сохранять текст без искажений: это
/// основная гарантия совместимости файла треда.
void test_build_parse_roundtrip()
{
    const char* original = "Строка с \"кавычками\", слэшем \\ и\nпереводом строки.";

    const size_t n = chatline::build(
        api::Role::Assistant, original, "2026-11-24T12:00:00+03:00", chatline::Usage{}, g_buf, sizeof(g_buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    chatline::Entry e;
    TEST_ASSERT_TRUE(chatline::parse(g_buf, n, e, g_content, sizeof(g_content)));
    TEST_ASSERT_EQUAL_STRING(original, e.content);
    TEST_ASSERT_EQUAL_STRING("2026-11-24T12:00:00+03:00", e.ts);
}

void test_estimate_size_grows_with_content()
{
    TEST_ASSERT_GREATER_THAN(chatline::estimate_size(""), chatline::estimate_size("длинный текст"));
}

// --- Сообщения агента -------------------------------------------------------

/// Результат инструмента: role=tool с tool_call_id и name. Без ссылки на вызов
/// сервер не сопоставит результат с вопросом модели.
void test_build_tool_message()
{
    const size_t n = chatline::build(api::Role::Tool, "Создан документ home/x.md", nullptr, chatline::Usage{}, g_buf,
        sizeof(g_buf), "call_1", "write_doc");
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"role\":\"tool\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"tool_call_id\":\"call_1\""));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"name\":\"write_doc\""));
}

/// Сообщение ассистента с вызовами: массив переносится как есть, байт в байт.
/// Пересобрать его из структуры значило бы разойтись с тем, что сказала модель.
void test_build_assistant_with_tool_calls()
{
    const char* calls =
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_doc\",\"arguments\":\"{}\"}}]";
    const size_t n = chatline::build(
        api::Role::Assistant, "", nullptr, chatline::Usage{}, g_buf, sizeof(g_buf), nullptr, nullptr, calls);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "\"tool_calls\":"));
    TEST_ASSERT_NOT_NULL(strstr(g_buf, "read_doc"));
}

/// Круг для результата инструмента: id и имя возвращаются теми же.
void test_tool_message_roundtrip()
{
    char calls[512];
    const size_t n = chatline::build(
        api::Role::Tool, "Ошибка: файла нет", nullptr, chatline::Usage{}, g_buf, sizeof(g_buf), "call_abc", "read_doc");
    TEST_ASSERT_GREATER_THAN(0, n);

    chatline::Entry e;
    TEST_ASSERT_TRUE(chatline::parse(g_buf, n, e, g_content, sizeof(g_content), calls, sizeof(calls)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(api::Role::Tool), static_cast<uint8_t>(e.role));
    TEST_ASSERT_EQUAL_STRING("call_abc", e.tool_call_id);
    TEST_ASSERT_EQUAL_STRING("read_doc", e.name);
    TEST_ASSERT_EQUAL_STRING("Ошибка: файла нет", e.content);
}

/// Круг для массива вызовов: текст сохраняется как JSON, который снова
/// разбирается — иначе после перезагрузки история стала бы неотправляемой.
void test_tool_calls_roundtrip()
{
    const char* calls =
        "[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"search_docs\",\"arguments\":\"{\\\"query\\\":"
        "\\\"фильтр\\\"}\"}}]";
    const size_t n = chatline::build(
        api::Role::Assistant, "", nullptr, chatline::Usage{}, g_buf, sizeof(g_buf), nullptr, nullptr, calls);
    TEST_ASSERT_GREATER_THAN(0, n);

    char calls_out[1024];
    chatline::Entry e;
    TEST_ASSERT_TRUE(chatline::parse(g_buf, n, e, g_content, sizeof(g_content), calls_out, sizeof(calls_out)));
    TEST_ASSERT_NOT_NULL(e.tool_calls);
    TEST_ASSERT_GREATER_THAN(0, e.tool_calls_len);
    TEST_ASSERT_EQUAL_STRING(calls, calls_out);
}

/// Массив вызовов не влез в буфер — честный отказ, а не молча потерянные
/// вызовы: без них история не отправится, и это надо заметить при разборе.
void test_tool_calls_small_buffer_returns_false()
{
    const char* calls =
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_doc\",\"arguments\":\"{}\"}}]";
    const size_t n = chatline::build(
        api::Role::Assistant, "", nullptr, chatline::Usage{}, g_buf, sizeof(g_buf), nullptr, nullptr, calls);
    TEST_ASSERT_GREATER_THAN(0, n);

    char small[16];
    chatline::Entry e;
    TEST_ASSERT_FALSE(chatline::parse(g_buf, n, e, g_content, sizeof(g_content), small, sizeof(small)));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_user_message);
    RUN_TEST(test_build_with_timestamp);
    RUN_TEST(test_build_assistant_with_usage);
    RUN_TEST(test_build_partial_answer);
    RUN_TEST(test_build_user_has_no_usage);
    RUN_TEST(test_build_escapes_content);
    RUN_TEST(test_build_small_buffer_returns_zero);
    RUN_TEST(test_build_null_content_is_empty);
    RUN_TEST(test_parse_user_message);
    RUN_TEST(test_parse_assistant_role);
    RUN_TEST(test_parse_usage);
    RUN_TEST(test_parse_partial_flag);
    RUN_TEST(test_parse_meta_line_skipped);
    RUN_TEST(test_parse_broken_line_returns_false);
    RUN_TEST(test_parse_line_without_role_returns_false);
    RUN_TEST(test_parse_unknown_role_falls_back_to_user);
    RUN_TEST(test_parse_missing_content_is_empty);
    RUN_TEST(test_parse_small_content_buffer_returns_false);
    RUN_TEST(test_build_parse_roundtrip);
    RUN_TEST(test_estimate_size_grows_with_content);

    RUN_TEST(test_build_tool_message);
    RUN_TEST(test_build_assistant_with_tool_calls);
    RUN_TEST(test_tool_message_roundtrip);
    RUN_TEST(test_tool_calls_roundtrip);
    RUN_TEST(test_tool_calls_small_buffer_returns_false);
    return UNITY_END();
}
