/*
 * Стык трёх разборщиков на настоящем ответе RouterAI.
 *
 * util/chunked, util/sse и util/api_json проверены по отдельности, но живой
 * ответ проходит через все три подряд, и дефект стыка не поймает ни один
 * модульный тест. Здесь записанный реальный поток (проверка 2026-11-24)
 * подаётся в ChunkedDecoder байт за байтом, тот кормит sse::Parser, а события
 * уходят в api::parse_chunk — ровно как в hw/api_client.
 *
 * Почему по одному байту: граница чанка TCP регулярно попадает в середину
 * строки `data:` и даже в середину UTF-8 символа. Если разборщик где-то
 * опирается на «кусок пришёл целиком», байтовое дробление это вскроет.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/api_json.h"
#include "util/chunked.h"
#include "util/sse.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

/// Настоящий ответ RouterAI (стриминг, reasoning_effort=none), уже без
/// чанкового обрамления: то, что отдаёт сокет после декодирования.
constexpr const char* kStream =
    ": PROCESSING\n"
    "\n"
    ": PROCESSING\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"role\":\"assistant\"},\"index\":0}],\"created\":1795503091,"
    "\"id\":\"rai-1795503090-QvlWO8fpCxDBRGhaYqx0\",\"model\":\"deepseek/deepseek-v4.1-flash-1017\","
    "\"service_tier\":\"default\",\"object\":\"chat.completion.chunk\",\"usage\":null,\"provider\":\"BytePlus\"}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"К\",\"role\":\"assistant\"},\"index\":0}],\"created\":1795503091,"
    "\"id\":\"rai-1795503090-QvlWO8fpCxDBRGhaYqx0\",\"model\":\"deepseek/deepseek-v4.1-flash-1017\","
    "\"service_tier\":\"default\",\"object\":\"chat.completion.chunk\",\"usage\":null,\"provider\":\"BytePlus\"}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"расный, з\",\"role\":\"assistant\"},\"index\":0}],"
    "\"created\":1795503091,\"id\":\"rai-1795503090-QvlWO8fpCxDBRGhaYqx0\","
    "\"model\":\"deepseek/deepseek-v4.1-flash-1017\",\"service_tier\":\"default\","
    "\"object\":\"chat.completion.chunk\",\"usage\":null,\"provider\":\"BytePlus\"}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"елёный, синий\",\"role\":\"assistant\"},\"index\":0}],"
    "\"created\":1795503091,\"id\":\"rai-1795503090-QvlWO8fpCxDBRGhaYqx0\","
    "\"model\":\"deepseek/deepseek-v4.1-flash-1017\",\"service_tier\":\"default\","
    "\"object\":\"chat.completion.chunk\",\"usage\":null,\"provider\":\"BytePlus\"}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"role\":\"assistant\"},\"finish_reason\":\"stop\",\"index\":0}],"
    "\"created\":1795503091,\"id\":\"rai-1795503090-QvlWO8fpCxDBRGhaYqx0\","
    "\"model\":\"deepseek/deepseek-v4.1-flash-1017\",\"service_tier\":\"default\","
    "\"object\":\"chat.completion.chunk\",\"usage\":null,\"provider\":\"BytePlus\"}\n"
    "\n"
    "data: {\"choices\":[],\"created\":1795503091,\"id\":\"rai-1795503090-QvlWO8fpCxDBRGhaYqx0\","
    "\"model\":\"deepseek/deepseek-v4.1-flash-1017\",\"service_tier\":\"default\","
    "\"object\":\"chat.completion.chunk\",\"usage\":{\"total_tokens\":29,\"prompt_tokens\":18,"
    "\"prompt_tokens_details\":{\"cached_tokens\":0},\"completion_tokens\":11,"
    "\"completion_tokens_details\":{\"reasoning_tokens\":0},\"cost\":0.0021849937824},\"provider\":\"BytePlus\"}\n"
    "\n"
    "data: [DONE]\n"
    "\n";

/// Что собрали из потока — то, что увидел бы экран чата.
struct Collected {
    char text[512]{};
    size_t text_len{0};
    long input_tokens{0};
    long output_tokens{0};
    char finish_reason[24]{};
    bool finished{false};
    unsigned events{0};
};

void on_text(const char* text, size_t len, void* ctx)
{
    auto* c = static_cast<Collected*>(ctx);
    if (c->text_len + len + 1 > sizeof(c->text)) return;
    memcpy(c->text + c->text_len, text, len);
    c->text_len += len;
    c->text[c->text_len] = '\0';
}

void on_sse_event(const char* data, size_t len, void* ctx)
{
    auto* c = static_cast<Collected*>(ctx);
    ++c->events;

    api::Chunk chunk;
    char text[256];
    char reasoning[256];
    if (!api::parse_chunk(data, len, chunk, text, sizeof(text), reasoning, sizeof(reasoning))) return;

    if (chunk.has_text) on_text(text, chunk.text_len, ctx);
    if (chunk.has_usage) {
        c->input_tokens = chunk.input_tokens;
        c->output_tokens = chunk.output_tokens;
    }
    if (chunk.has_finish) {
        snprintf(c->finish_reason, sizeof(c->finish_reason), "%s", chunk.finish_reason);
        c->finished = true;
    }
}

void on_chunk_data(const char* data, size_t len, void* ctx)
{
    static_cast<sse::Parser*>(ctx)->feed(data, len);
}

/// Оборачивает payload в чанковое обрамление кусками по chunk_size байт.
void feed_as_chunked(const char* payload, size_t len, size_t chunk_size, http::ChunkedDecoder& decoder)
{
    char framed[16];
    size_t pos = 0;
    while (pos < len) {
        size_t n = (len - pos < chunk_size) ? len - pos : chunk_size;
        const int head = snprintf(framed, sizeof(framed), "%zx\r\n", n);
        decoder.feed(framed, static_cast<size_t>(head));
        decoder.feed(payload + pos, n);
        decoder.feed("\r\n", 2);
        pos += n;
    }
    decoder.feed("0\r\n\r\n", 5);
}

/// Гоняет весь конвейер с заданным дроблением.
void run_pipeline(size_t chunk_size, Collected& out)
{
    sse::Parser parser(on_sse_event, &out);
    http::ChunkedDecoder decoder(on_chunk_data, &parser);

    feed_as_chunked(kStream, strlen(kStream), chunk_size, decoder);
}

}  // namespace

/// Заголовок чанка разбирается, события доходят до JSON, текст собирается.
void test_pipeline_reassembles_text()
{
    Collected c;
    run_pipeline(64, c);

    TEST_ASSERT_EQUAL_STRING("Красный, зелёный, синий", c.text);
    TEST_ASSERT_EQUAL_INT(18, c.input_tokens);
    TEST_ASSERT_EQUAL_INT(11, c.output_tokens);
    TEST_ASSERT_EQUAL_STRING("stop", c.finish_reason);
    TEST_ASSERT_TRUE(c.finished);
}

/// Побайтовая подача: границы чанков рвут строки и UTF-8 пополам.
void test_pipeline_byte_by_byte()
{
    Collected c;
    run_pipeline(1, c);

    TEST_ASSERT_EQUAL_STRING("Красный, зелёный, синий", c.text);
    TEST_ASSERT_EQUAL_UINT32(18, static_cast<unsigned>(c.input_tokens));
    TEST_ASSERT_TRUE(c.finished);
}

/// Кусок в 7 байт — самый неудобный: не делит ни строки, ни глифы нацело.
void test_pipeline_odd_chunks()
{
    Collected c;
    run_pipeline(7, c);

    TEST_ASSERT_EQUAL_STRING("Красный, зелёный, синий", c.text);
    TEST_ASSERT_EQUAL_STRING("stop", c.finish_reason);
}

/// Комментарии `: PROCESSING` — heartbeat сервера, а не событие. Если считать
/// их событиями, счётчик покажет лишнее, а разбор JSON будет впустую падать.
void test_pipeline_ignores_heartbeats()
{
    Collected c;
    run_pipeline(64, c);

    // Шесть содержательных чанков data: (пустая роль, три куска текста,
    // finish_reason, usage). Два `: PROCESSING` и `[DONE]` в счёт не входят.
    TEST_ASSERT_EQUAL_UINT32(6, c.events);
}

/// Поток завершён: и terminator чанка, и [DONE] должны быть видны.
void test_pipeline_reports_completion()
{
    Collected c;
    sse::Parser parser(on_sse_event, &c);
    http::ChunkedDecoder decoder(on_chunk_data, &parser);

    feed_as_chunked(kStream, strlen(kStream), 100, decoder);

    TEST_ASSERT_TRUE(parser.done());
    TEST_ASSERT_TRUE(decoder.done());
    TEST_ASSERT_FALSE(decoder.broken());
    TEST_ASSERT_EQUAL_UINT32(0, parser.dropped());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_pipeline_reassembles_text);
    RUN_TEST(test_pipeline_byte_by_byte);
    RUN_TEST(test_pipeline_odd_chunks);
    RUN_TEST(test_pipeline_ignores_heartbeats);
    RUN_TEST(test_pipeline_reports_completion);
    return UNITY_END();
}
