/*
 * Тесты форматирования времени работы для экрана «О программе».
 *
 * На плате строка видна только глазами и меняется медленно, поэтому правило
 * проверяется на хосте:
 *   - свежая загрузка даёт «только что», а не «0 мин»;
 *   - минуты, часы и сутки появляются ровно тогда, когда они есть;
 *   - малый буфер не приводит к мусору.
 */

#include <unity.h>

#include <string.h>

#include "util/about_info.h"

void setUp(void) {}
void tearDown(void) {}

void test_uptime_fresh_boot()
{
    char buf[32];
    about::uptime_label(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("только что", buf);

    // До первой минуты — всё ещё «только что», а не «0 мин»: ноль читается
    // как счётчик, который не работает.
    about::uptime_label(59999, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("только что", buf);
}

void test_uptime_minutes()
{
    char buf[32];
    about::uptime_label(60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1 мин", buf);

    about::uptime_label(7 * 60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("7 мин", buf);

    // «0 ч 59 мин» лишнее слово: часов ещё нет, показываем минуты.
    about::uptime_label(59 * 60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("59 мин", buf);
}

void test_uptime_hours()
{
    char buf[32];
    about::uptime_label(60 * 60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1 ч 0 мин", buf);

    about::uptime_label(193 * 60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3 ч 13 мин", buf);

    // Сутки ещё не наступили — часы идут до 23.
    about::uptime_label((24 * 60 - 1) * 60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("23 ч 59 мин", buf);
}

void test_uptime_days()
{
    char buf[32];
    about::uptime_label(24 * 60 * 60000u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1 сут 0 ч", buf);

    about::uptime_label((2 * 24 + 4) * 60 * 60000u, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2 сут 4 ч", buf);
}

void test_uptime_large_value()
{
    // Верхняя граница uint32 — около 49 суток. Счёт не должен переполняться
    // или показывать отрицательное число.
    char buf[32];
    about::uptime_label(0xFFFFFFFFu, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("49 сут 17 ч", buf);
}

void test_uptime_small_buffer()
{
    // Буфер, в который влезает только часть строки, не должен давать мусор:
    // snprintf обрезает по границе и закрывает строку. «3 ч 13 мин» в шесть
    // байт не влезает — «3 ч » (пять символов) и нуль.
    char buf[6];
    about::uptime_label(193 * 60000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("3 ч ", buf);
}

void test_uptime_null_safety()
{
    // Вызывающий мог передать nullptr — падать в этом месте незачем.
    about::uptime_label(60000, nullptr, 32);

    char buf[32];
    about::uptime_label(60000, buf, 1);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_uptime_fresh_boot);
    RUN_TEST(test_uptime_minutes);
    RUN_TEST(test_uptime_hours);
    RUN_TEST(test_uptime_days);
    RUN_TEST(test_uptime_large_value);
    RUN_TEST(test_uptime_small_buffer);
    RUN_TEST(test_uptime_null_safety);
    return UNITY_END();
}
