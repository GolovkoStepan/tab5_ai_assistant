/*
 * Тесты чистых функций форматирования строк.
 *
 * Эти функции нужны и в прошивке (статус-бар, лог агента), и на хосте —
 * поэтому вынесены в отдельный модуль без зависимостей от Arduino.
 */

#include <string.h>
#include <unity.h>

#include "util/format.h"

void setUp(void) {}
void tearDown(void) {}

// --- Форматирование объёма памяти ---

void test_format_bytes_kilobytes()
{
    char buf[16];
    format_bytes(1536, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.5 КБ", buf);
}

void test_format_bytes_megabytes()
{
    char buf[16];
    format_bytes(3 * 1024 * 1024, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3.0 МБ", buf);
}

void test_format_bytes_gigabytes()
{
    char buf[16];
    format_bytes(4ULL * 1024 * 1024 * 1024, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("4.0 ГБ", buf);
}

void test_format_bytes_small()
{
    char buf[16];
    format_bytes(512, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("512 Б", buf);
}

// --- Форматирование счётчика токенов ---

void test_format_tokens_plain()
{
    char buf[16];
    format_tokens(480, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("480", buf);
}

void test_format_tokens_thousands()
{
    char buf[16];
    format_tokens(12345, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("12.3k", buf);
}

void test_format_tokens_zero()
{
    char buf[16];
    format_tokens(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

// --- Обрезка строки под ширину ---

void test_truncate_no_change()
{
    char buf[32];
    truncate_text("короткий", 30, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("короткий", buf);
}

void test_truncate_adds_ellipsis()
{
    char buf[32];
    truncate_text("очень длинная строка", 10, buf, sizeof(buf));
    // 9 символов + многоточие
    TEST_ASSERT_EQUAL_STRING_LEN("очень дли", buf, 9);
    TEST_ASSERT_EQUAL_STRING("…", buf + strlen(buf) - 3);
}

void test_truncate_zero_width()
{
    char buf[8];
    truncate_text("текст", 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

// --- Перенос строк по ширине ---

void test_wrap_simple()
{
    // Функция возвращает число строк и заполняет массив смещений
    const char* text = "один два три";
    size_t offsets[8];
    size_t count = wrap_text(text, 9, offsets, 8);
    TEST_ASSERT_GREATER_THAN(1, count);
}

void test_wrap_single_line()
{
    const char* text = "коротко";
    size_t offsets[8];
    size_t count = wrap_text(text, 100, offsets, 8);
    TEST_ASSERT_EQUAL_UINT(1, count);
    TEST_ASSERT_EQUAL_UINT(0, offsets[0]);
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_format_bytes_kilobytes);
    RUN_TEST(test_format_bytes_megabytes);
    RUN_TEST(test_format_bytes_gigabytes);
    RUN_TEST(test_format_bytes_small);

    RUN_TEST(test_format_tokens_plain);
    RUN_TEST(test_format_tokens_thousands);
    RUN_TEST(test_format_tokens_zero);

    RUN_TEST(test_truncate_no_change);
    RUN_TEST(test_truncate_adds_ellipsis);
    RUN_TEST(test_truncate_zero_width);

    RUN_TEST(test_wrap_simple);
    RUN_TEST(test_wrap_single_line);

    return UNITY_END();
}
