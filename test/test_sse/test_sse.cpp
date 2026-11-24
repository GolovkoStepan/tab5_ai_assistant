/*
 * Тесты SSE-парсера.
 *
 * Главное, что здесь проверяется: границы чанков. TCP приносит произвольные
 * куски, и классический дефект стриминга — потеря события, когда `data:` и
 * его JSON оказались в разных read(). Поэтому почти все тесты кормят парсер
 * байт по байту и кусками неудобной длины.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/sse.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

/// Собирает события в один буфер через перевод строки — так проще сравнивать.
struct Collector {
    char buf[4096]{};
    size_t len{0};
    int events{0};
    int done{0};
};

void collect(const char* data, size_t len, void* ctx)
{
    auto* c = static_cast<Collector*>(ctx);
    if (c->len > 0) c->buf[c->len++] = '|';
    if (c->len + len < sizeof(c->buf)) {
        memcpy(c->buf + c->len, data, len);
        c->len += len;
        c->buf[c->len] = '\0';
    }
    ++c->events;
}

}  // namespace

void test_single_event()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: {\"a\":1}\n\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));
    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", c.buf);
    TEST_ASSERT_FALSE(p.done());
}

/// Событие не выдаётся, пока не пришла пустая строка: иначе многострочное
/// `data:` развалилось бы на отдельные куски.
void test_event_only_after_blank_line()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* partial = "data: {\"a\":1}\n";
    TEST_ASSERT_TRUE(p.feed(partial, strlen(partial)));
    TEST_ASSERT_EQUAL_INT(0, c.events);

    const char* blank = "\n";
    TEST_ASSERT_TRUE(p.feed(blank, strlen(blank)));
    TEST_ASSERT_EQUAL_INT(1, c.events);
}

/// Самый частый реальный дефект: чанк разрезан посреди строки.
void test_byte_by_byte()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: {\"x\":\"привет\"}\n\ndata: {\"y\":2}\n\n";
    for (size_t i = 0; i < strlen(stream); ++i) {
        TEST_ASSERT_TRUE(p.feed(stream + i, 1));
    }

    TEST_ASSERT_EQUAL_INT(2, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"x\":\"привет\"}|{\"y\":2}", c.buf);
}

/// Разрез ровно в неудобных местах: после `data:`, на двоеточии, на `\r`.
void test_awkward_split_points()
{
    const char* const parts[] = {"da", "ta", ":", " ", "{\"a\":", "1}", "\r", "\n", "\r", "\n"};
    Collector c;
    sse::Parser p(collect, &c);

    for (const char* part : parts) {
        TEST_ASSERT_TRUE(p.feed(part, strlen(part)));
    }

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", c.buf);
}

/// CRLF — законный разделитель строк в HTTP. Без его обработки к JSON
/// прилипал бы \r, и разбор падал бы на ровном месте.
void test_crlf()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: {\"a\":1}\r\n\r\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", c.buf);
}

void test_done_marker_stops()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: [DONE]\n\n";
    TEST_ASSERT_FALSE(p.feed(stream, strlen(stream)));
    TEST_ASSERT_TRUE(p.done());
    // Маркер — не событие для обработчика: это транспортный сигнал.
    TEST_ASSERT_EQUAL_INT(0, c.events);
}

/// После [DONE] сервер может дописать мусор (или прийти ещё один TCP-сегмент).
/// Парсер обязан остаться в состоянии «готово» и не выдавать событий.
void test_data_after_done_is_ignored()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: [DONE]\n\ndata: {\"late\":true}\n\n";
    TEST_ASSERT_FALSE(p.feed(stream, strlen(stream)));
    TEST_ASSERT_EQUAL_INT(0, c.events);
}

/// Комментарии-«heartbeat» (`: ping`) не должны попадать в события и не должны
/// сбивать разделитель.
void test_comment_lines_ignored()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = ": ping\n\ndata: {\"a\":1}\n\n: keep-alive\n\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", c.buf);
}

/// Служебные поля `event:` и `id:` не входят в полезную нагрузку.
void test_other_fields_ignored()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "event: message\nid: 42\ndata: {\"a\":1}\n\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", c.buf);
}

/// Многострочное data: строки склеиваются через \n, как требует формат.
void test_multiline_data_joined()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: {\"a\":\ndata: 1}\n\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":\n1}", c.buf);
}

/// Пробел после двоеточия в значение не входит, а второй пробел — уже данные.
void test_space_after_colon_only_once()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data:  x\n\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING(" x", c.buf);
}

/// `data:` без пробела и без значения (пустое событие) — просто разделитель.
void test_empty_data_line()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data:\n\n";
    TEST_ASSERT_TRUE(p.feed(stream, strlen(stream)));
    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("", c.buf);
}

/// Событие длиннее буфера выбрасывается целиком, а не приходит обрезанным:
/// половина JSON хуже, чем ничего, — разборщик выдал бы ошибку вместо
/// нормального пропуска.
void test_oversized_event_dropped()
{
    Collector c;
    sse::Parser p(collect, &c);

    static char big[sse::kMaxEventBytes + 128];
    const size_t prefix = strlen("data: ");
    memcpy(big, "data: ", prefix);
    memset(big + prefix, 'x', sizeof(big) - prefix - 2);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\n';

    TEST_ASSERT_TRUE(p.feed(big, sizeof(big)));

    TEST_ASSERT_EQUAL_INT(0, c.events);
    TEST_ASSERT_EQUAL_UINT(1, p.dropped());
}

/// После выброшенного большого события поток продолжает разбираться: одна
/// плохая строка не должна убивать весь ответ.
void test_stream_survives_oversized_event()
{
    Collector c;
    sse::Parser p(collect, &c);

    static char big[sse::kMaxEventBytes + 128];
    const size_t prefix = strlen("data: ");
    memcpy(big, "data: ", prefix);
    memset(big + prefix, 'x', sizeof(big) - prefix - 2);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\n';

    TEST_ASSERT_TRUE(p.feed(big, sizeof(big)));

    const char* ok = "data: {\"a\":1}\n\n";
    TEST_ASSERT_TRUE(p.feed(ok, strlen(ok)));

    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", c.buf);
}

/// reset() нужен при повторном запросе в том же соединении: остатки прошлого
/// ответа не должны попасть в новый.
void test_reset_clears_state()
{
    Collector c;
    sse::Parser p(collect, &c);

    const char* stream = "data: [DONE]\n\n";
    TEST_ASSERT_FALSE(p.feed(stream, strlen(stream)));
    TEST_ASSERT_TRUE(p.done());

    p.reset();
    TEST_ASSERT_FALSE(p.done());
    TEST_ASSERT_EQUAL_UINT(0, p.dropped());

    const char* next = "data: {\"a\":2}\n\n";
    TEST_ASSERT_TRUE(p.feed(next, strlen(next)));
    TEST_ASSERT_EQUAL_INT(1, c.events);
    TEST_ASSERT_EQUAL_STRING("{\"a\":2}", c.buf);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_single_event);
    RUN_TEST(test_event_only_after_blank_line);
    RUN_TEST(test_byte_by_byte);
    RUN_TEST(test_awkward_split_points);
    RUN_TEST(test_crlf);
    RUN_TEST(test_done_marker_stops);
    RUN_TEST(test_data_after_done_is_ignored);
    RUN_TEST(test_comment_lines_ignored);
    RUN_TEST(test_other_fields_ignored);
    RUN_TEST(test_multiline_data_joined);
    RUN_TEST(test_space_after_colon_only_once);
    RUN_TEST(test_empty_data_line);
    RUN_TEST(test_oversized_event_dropped);
    RUN_TEST(test_stream_survives_oversized_event);
    RUN_TEST(test_reset_clears_state);
    return UNITY_END();
}
