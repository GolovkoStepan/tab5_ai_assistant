/*
 * Тесты декодера Transfer-Encoding: chunked.
 *
 * Проверяется главная ловушка: SSE-ответ RouterAI приходит чанками, а
 * HTTPClient::getStreamPtr() их НЕ декодирует. Без этого декодера SSE-парсер
 * увидел бы «1f\r\ndata: {...}» и не разобрал бы ни одного события.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/chunked.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

/// Приёмник собирает всё, что выдал декодер, в один буфер.
struct Collector {
    char buf[8192]{};
    size_t len{0};
};

void sink(const char* data, size_t len, void* ctx)
{
    auto* c = static_cast<Collector*>(ctx);
    if (c->len + len < sizeof(c->buf)) {
        memcpy(c->buf + c->len, data, len);
        c->len += len;
        c->buf[c->len] = '\0';
    }
}

}  // namespace

/// Простейший случай: один чанк и терминальный.
void test_single_chunk()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "5\r\nhello\r\n0\r\n\r\n";
    TEST_ASSERT_FALSE(d.feed(raw, strlen(raw)));

    TEST_ASSERT_TRUE(d.done());
    TEST_ASSERT_FALSE(d.broken());
    TEST_ASSERT_EQUAL_STRING("hello", c.buf);
    TEST_ASSERT_EQUAL_UINT32(5, d.payload_bytes());
    TEST_ASSERT_EQUAL_UINT32(1, d.chunks());
}

/// Несколько чанков склеиваются в один непрерывный поток — именно этого ждёт
/// SSE-парсер.
void test_multiple_chunks_concatenated()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "4\r\n0123\r\n4\r\n4567\r\n0\r\n\r\n";
    d.feed(raw, strlen(raw));

    TEST_ASSERT_EQUAL_STRING("01234567", c.buf);
    TEST_ASSERT_EQUAL_UINT32(2, d.chunks());
}

/// Реальная картина: заголовок чанка разрезан между двумя read().
void test_boundary_inside_chunk_header()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    // "1f\r\n" разрезан после «1».
    TEST_ASSERT_TRUE(d.feed("1", 1));
    TEST_ASSERT_TRUE(d.feed("f\r\n", 3));

    TEST_ASSERT_EQUAL_UINT32(0, d.payload_bytes());

    char data[32];
    memset(data, 'x', sizeof(data));
    TEST_ASSERT_TRUE(d.feed(data, 5));
    TEST_ASSERT_EQUAL_UINT32(5, d.payload_bytes());
}

/// Байт за байтом — худший случай для автомата.
void test_byte_by_byte()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    for (size_t i = 0; i < strlen(raw); ++i) {
        d.feed(raw + i, 1);
    }

    TEST_ASSERT_TRUE(d.done());
    TEST_ASSERT_EQUAL_STRING("abcde", c.buf);
    TEST_ASSERT_EQUAL_UINT32(2, d.chunks());
}

/// Расширение чанка (`;name=value`) не должно попадать в данные.
void test_chunk_extension_skipped()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "5;foo=bar\r\nhello\r\n0\r\n\r\n";
    d.feed(raw, strlen(raw));

    TEST_ASSERT_TRUE(d.done());
    TEST_ASSERT_EQUAL_STRING("hello", c.buf);
}

/// Заголовочные и строчные hex-цифры одинаково допустимы.
void test_uppercase_hex_size()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    char raw[64];
    snprintf(raw, sizeof(raw), "A\r\n0123456789\r\n0\r\n\r\n");
    d.feed(raw, strlen(raw));

    TEST_ASSERT_EQUAL_STRING("0123456789", c.buf);
}

/// Трейлеры после терминального чанка не должны ломать разбор.
void test_trailers_ignored()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "2\r\nok\r\n0\r\nx-checksum: abc\r\n\r\n";
    d.feed(raw, strlen(raw));

    TEST_ASSERT_TRUE(d.done());
    TEST_ASSERT_FALSE(d.broken());
    TEST_ASSERT_EQUAL_STRING("ok", c.buf);
}

/// Терминальный чанк без трейлеров, но с одним \r\n — законный вариант.
void test_zero_chunk_without_trailer()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "2\r\nok\r\n0\r\n\r\n";
    d.feed(raw, strlen(raw));

    TEST_ASSERT_TRUE(d.done());
    TEST_ASSERT_EQUAL_STRING("ok", c.buf);
}

/// Мусор вместо длины: останавливаемся и помечаем поток битым, а не читаем
/// вечно в надежде на чудо.
void test_garbage_size_is_error()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "zz\r\n";
    TEST_ASSERT_FALSE(d.feed(raw, strlen(raw)));
    TEST_ASSERT_TRUE(d.broken());
    TEST_ASSERT_FALSE(d.done());
}

/// Пустая строка вместо длины — тоже ошибка (не терминальный чанк: тот
/// записывается как «0»).
void test_empty_size_is_error()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "\r\n";
    TEST_ASSERT_FALSE(d.feed(raw, strlen(raw)));
    TEST_ASSERT_TRUE(d.broken());
}

/// Отсутствие \n после данных — ошибка, а не «съеденный» байт.
void test_missing_data_crlf_is_error()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    // Данные правильные, после них «X» вместо \r.
    const char* raw = "2\r\nokX";
    d.feed(raw, strlen(raw));

    TEST_ASSERT_TRUE(d.broken());
}

/// После завершения потока дальнейшие байты игнорируются: сервер может
/// прислать ещё один TCP-сегмент, и он не должен попасть в данные.
void test_bytes_after_done_ignored()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    const char* raw = "2\r\nok\r\n0\r\n\r\n";
    d.feed(raw, strlen(raw));
    TEST_ASSERT_TRUE(d.done());

    d.feed("мусор", strlen("мусор"));
    TEST_ASSERT_EQUAL_STRING("ok", c.buf);
}

/// Реалистичный кусок SSE-потока RouterAI: два события, разбитые на чанки.
void test_realistic_sse_stream()
{
    Collector c;
    http::ChunkedDecoder d(sink, &c);

    // Длины чанков считаны по факту: 52 байта = 0x34 (кириллица — по 2 байта).
    const char* raw =
        "34\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"при\"}}]}\n\n"
        "\r\n"
        "34\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"вет\"}}]}\n\n"
        "\r\n"
        "0\r\n\r\n";

    d.feed(raw, strlen(raw));

    TEST_ASSERT_TRUE(d.done());
    TEST_ASSERT_NOT_NULL(strstr(c.buf, "data: {\"choices\":[{\"delta\":{\"content\":\"при\"}}]}"));
    TEST_ASSERT_NOT_NULL(strstr(c.buf, "\"вет\""));
    // В данных не должно остаться заголовков чанков.
    TEST_ASSERT_NULL(strstr(c.buf, "34\r\n"));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_chunk);
    RUN_TEST(test_multiple_chunks_concatenated);
    RUN_TEST(test_boundary_inside_chunk_header);
    RUN_TEST(test_byte_by_byte);
    RUN_TEST(test_chunk_extension_skipped);
    RUN_TEST(test_uppercase_hex_size);
    RUN_TEST(test_trailers_ignored);
    RUN_TEST(test_zero_chunk_without_trailer);
    RUN_TEST(test_garbage_size_is_error);
    RUN_TEST(test_empty_size_is_error);
    RUN_TEST(test_missing_data_crlf_is_error);
    RUN_TEST(test_bytes_after_done_ignored);
    RUN_TEST(test_realistic_sse_stream);
    return UNITY_END();
}
