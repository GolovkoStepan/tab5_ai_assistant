/*
 * Тесты времени сообщений.
 *
 * Время в треде нужно для двух вещей: чтобы человек понимал, когда это было, и
 * чтобы сортировка не зависела от часового пояса. Поэтому проверяется не
 * «строка не пустая», а что смещение записано верно, включая знак POSIX
 * (в "MSK-3" спрятан UTC+3, и перепутать знак — классическая ошибка).
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/iso_time.h"

void setUp(void) {}
void tearDown(void) {}

// --- Смещение таймзоны ---

void test_tz_msk()
{
    int minutes = 0;
    TEST_ASSERT_TRUE(isotime::parse_tz_offset("MSK-3", &minutes));
    TEST_ASSERT_EQUAL_INT(180, minutes);
}

/// Знак POSIX перевёрнут: "UTC+5" — это UTC−5. Спутать легко, а сообщения
/// после этого датируются на 10 часов неверно.
void test_tz_posix_sign_inverted()
{
    int minutes = 0;
    TEST_ASSERT_TRUE(isotime::parse_tz_offset("UTC+5", &minutes));
    TEST_ASSERT_EQUAL_INT(-300, minutes);
}

void test_tz_zero()
{
    int minutes = 99;
    TEST_ASSERT_TRUE(isotime::parse_tz_offset("UTC-0", &minutes));
    TEST_ASSERT_EQUAL_INT(0, minutes);

    TEST_ASSERT_TRUE(isotime::parse_tz_offset("GMT+0", &minutes));
    TEST_ASSERT_EQUAL_INT(0, minutes);
}

/// Полчаса тоже встречаются: Индия UTC+5:30, Ньюфаундленд UTC−3:30.
void test_tz_half_hour()
{
    int minutes = 0;
    TEST_ASSERT_TRUE(isotime::parse_tz_offset("IST-5:30", &minutes));
    TEST_ASSERT_EQUAL_INT(330, minutes);
}

void test_tz_no_offset_is_error()
{
    int minutes = 0;
    TEST_ASSERT_FALSE(isotime::parse_tz_offset("MSK", &minutes));
    TEST_ASSERT_FALSE(isotime::parse_tz_offset("", &minutes));
    TEST_ASSERT_FALSE(isotime::parse_tz_offset(nullptr, &minutes));
}

void test_tz_out_of_range_is_error()
{
    int minutes = 0;
    TEST_ASSERT_FALSE(isotime::parse_tz_offset("MSK-15", &minutes));
    TEST_ASSERT_FALSE(isotime::parse_tz_offset("MSK-99", &minutes));
}

void test_tz_garbage_after_number_is_error()
{
    int minutes = 0;
    TEST_ASSERT_FALSE(isotime::parse_tz_offset("MSK-3abc", &minutes));
}

// --- Форматирование ---

void test_format_moscow()
{
    char out[32];
    const size_t n = isotime::format_iso8601(out, sizeof(out), 2026, 11, 24, 12, 5, 9, 180);

    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("2026-11-24T12:05:09+03:00", out);
    TEST_ASSERT_EQUAL_UINT32(strlen("2026-11-24T12:05:09+03:00"), n);
}

void test_format_utc()
{
    char out[32];
    TEST_ASSERT_GREATER_THAN(0, isotime::format_iso8601(out, sizeof(out), 2026, 1, 2, 3, 4, 5, 0));
    TEST_ASSERT_EQUAL_STRING("2026-01-02T03:04:05+00:00", out);
}

/// Отрицательное смещение: знак минус и абсолютное значение часов, а не «-3:-30».
void test_format_negative_offset()
{
    char out[32];
    TEST_ASSERT_GREATER_THAN(0, isotime::format_iso8601(out, sizeof(out), 2026, 11, 24, 12, 0, 0, -210));
    TEST_ASSERT_EQUAL_STRING("2026-11-24T12:00:00-03:30", out);
}

/// Ведущие нули обязательны: без них строка не разберётся как ISO-8601 ни
/// одним парсером и не отсортируется лексикографически.
void test_format_pads_zeros()
{
    char out[32];
    TEST_ASSERT_GREATER_THAN(0, isotime::format_iso8601(out, sizeof(out), 2026, 1, 2, 3, 4, 5, 180));
    TEST_ASSERT_EQUAL_STRING("2026-01-02T03:04:05+03:00", out);
}

/// Лексикографическая сортировка ломается, если год выходит за четыре цифры.
void test_format_requires_four_digit_year()
{
    char out[32];
    TEST_ASSERT_GREATER_THAN(0, isotime::format_iso8601(out, sizeof(out), 2026, 11, 24, 12, 0, 0, 180));
    TEST_ASSERT_EQUAL_UINT32('2', static_cast<unsigned char>(out[0]));
}

void test_format_rejects_invalid_fields()
{
    char out[32];
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 2026, 13, 1, 0, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 2026, 0, 1, 0, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 2026, 1, 1, 24, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 2026, 1, 1, 0, 60, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 2026, 1, 1, 0, 0, 60, 0));
}

/// RTC без батарейки отдаёт мусор — лучше не писать время вообще, чем
/// записать «0000-00-00».
void test_format_rejects_sentinel_time()
{
    char out[32];
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), -1, -1, -1, -1, -1, -1, 180));
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 1970, 1, 1, 0, 0, 0, 0));
}

void test_format_small_buffer_returns_zero()
{
    char out[8];
    TEST_ASSERT_EQUAL_UINT32(0, isotime::format_iso8601(out, sizeof(out), 2026, 11, 24, 12, 0, 0, 180));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/// Круг «таймзона из настроек → время сообщения»: значения по умолчанию
/// проекта (MSK-3) должны давать UTC+3.
void test_tz_from_default_settings_roundtrip()
{
    int minutes = 0;
    TEST_ASSERT_TRUE(isotime::parse_tz_offset("MSK-3", &minutes));

    char out[32];
    TEST_ASSERT_GREATER_THAN(0, isotime::format_iso8601(out, sizeof(out), 2026, 11, 24, 12, 0, 0, minutes));
    TEST_ASSERT_EQUAL_STRING("2026-11-24T12:00:00+03:00", out);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_tz_msk);
    RUN_TEST(test_tz_posix_sign_inverted);
    RUN_TEST(test_tz_zero);
    RUN_TEST(test_tz_half_hour);
    RUN_TEST(test_tz_no_offset_is_error);
    RUN_TEST(test_tz_out_of_range_is_error);
    RUN_TEST(test_tz_garbage_after_number_is_error);
    RUN_TEST(test_format_moscow);
    RUN_TEST(test_format_utc);
    RUN_TEST(test_format_negative_offset);
    RUN_TEST(test_format_pads_zeros);
    RUN_TEST(test_format_requires_four_digit_year);
    RUN_TEST(test_format_rejects_invalid_fields);
    RUN_TEST(test_format_rejects_sentinel_time);
    RUN_TEST(test_format_small_buffer_returns_zero);
    RUN_TEST(test_tz_from_default_settings_roundtrip);
    return UNITY_END();
}
