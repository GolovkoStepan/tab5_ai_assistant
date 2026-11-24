/*
 * Тесты строки списка тредов: разбор имени файла и заголовка.
 *
 * Имена тредов приходят из файловой системы, то есть могут быть какими угодно
 * (карту могли наполнить на компьютере). Ошибка в разборе — либо падение, либо
 * бессмысленная строка в списке, поэтому границы проверяются отдельно.
 */

#include <unity.h>

#include <string.h>

#include "util/chat_list.h"

void setUp(void) {}
void tearDown(void) {}

// --- Разбор имени треда ---

void test_slug_normal()
{
    chatlist::Stamp s;
    TEST_ASSERT_TRUE(chatlist::parse_slug("chat-20261124-120000", s));
    TEST_ASSERT_EQUAL_INT(2026, s.year);
    TEST_ASSERT_EQUAL_INT(11, s.month);
    TEST_ASSERT_EQUAL_INT(24, s.day);
    TEST_ASSERT_EQUAL_INT(12, s.hour);
    TEST_ASSERT_EQUAL_INT(0, s.minute);
    TEST_ASSERT_EQUAL_INT(0, s.second);
    TEST_ASSERT_TRUE(s.real);
}

void test_slug_with_suffix()
{
    // Два треда можно создать в одну секунду — имя получает суффикс.
    chatlist::Stamp s;
    TEST_ASSERT_TRUE(chatlist::parse_slug("chat-20261124-120000-1", s));
    TEST_ASSERT_EQUAL_INT(24, s.day);
    TEST_ASSERT_TRUE(s.real);
}

void test_slug_uptime_not_real()
{
    // Часы не были выставлены: дата нулевая, показать её как настоящую нельзя.
    chatlist::Stamp s;
    TEST_ASSERT_TRUE(chatlist::parse_slug("chat-00000000-005946", s));
    TEST_ASSERT_FALSE(s.real);
}

void test_slug_bad_prefix()
{
    chatlist::Stamp s;
    TEST_ASSERT_FALSE(chatlist::parse_slug("notes-20261124-120000", s));
    TEST_ASSERT_FALSE(chatlist::parse_slug("", s));
    TEST_ASSERT_FALSE(chatlist::parse_slug(nullptr, s));
}

void test_slug_short()
{
    chatlist::Stamp s;
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-2026", s));
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-2026112", s));
}

void test_slug_non_digits()
{
    chatlist::Stamp s;
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-2026x124-120000", s));
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-20261124_120000", s));
}

void test_slug_out_of_range()
{
    // Цифры на месте, но такой даты не существует: в списке она выглядела бы
    // как настоящая.
    chatlist::Stamp s;
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-20261324-120000", s));  // месяц 13
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-20261132-120000", s));  // день 32
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-20261124-250000", s));  // час 25
    TEST_ASSERT_FALSE(chatlist::parse_slug("chat-20261124-126000", s));  // минута 60
}

// --- Формат даты для списка ---

void test_format_stamp_real()
{
    chatlist::Stamp s;
    TEST_ASSERT_TRUE(chatlist::parse_slug("chat-20261124-120530", s));

    char buf[24];
    TEST_ASSERT_EQUAL_UINT32(11, chatlist::format_stamp(s, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("24.11 12:05", buf);
}

void test_format_stamp_not_real()
{
    chatlist::Stamp s;
    TEST_ASSERT_TRUE(chatlist::parse_slug("chat-00000000-005946", s));

    char buf[24];
    TEST_ASSERT_GREATER_THAN_UINT32(0, chatlist::format_stamp(s, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("—", buf);
}

void test_format_stamp_small_buffer()
{
    chatlist::Stamp s;
    TEST_ASSERT_TRUE(chatlist::parse_slug("chat-20261124-120530", s));

    char buf[5];
    TEST_ASSERT_EQUAL_UINT32(0, chatlist::format_stamp(s, buf, sizeof(buf)));
}

// --- Заголовок из первой строки ---

void test_title_user_message()
{
    const char* line =
        R"({"role":"user","content":"как сбросить ошибку очистителя?","ts":"2026-11-24T12:00:00+03:00"})";
    char buf[96];
    TEST_ASSERT_TRUE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("как сбросить ошибку очистителя?", buf);
}

void test_title_first_line_only()
{
    // В списке многострочный заголовок не читается: берём первую строку.
    const char* line = R"({"role":"user","content":"первая строка\nвторая строка"})";
    char buf[96];
    TEST_ASSERT_TRUE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("первая строка", buf);
}

void test_title_assistant_rejected()
{
    // Заголовок треда — вопрос пользователя. Ответ модели в списке бесполезен:
    // он начинается с «Конечно!» у половины тредов.
    const char* line = R"({"role":"assistant","content":"конечно, вот шаги"})";
    char buf[96];
    TEST_ASSERT_FALSE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
}

void test_title_meta_rejected()
{
    const char* line = R"({"type":"meta","title":"заголовок","created":"2026-11-24"})";
    char buf[96];
    TEST_ASSERT_FALSE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
}

void test_title_empty_content()
{
    const char* line = R"({"role":"user","content":""})";
    char buf[96];
    TEST_ASSERT_FALSE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
}

void test_title_broken_json()
{
    const char* line = R"({"role":"user","content":"обрыв без закрывающей скобки)";
    char buf[96];
    TEST_ASSERT_FALSE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
}

void test_title_truncated_with_ellipsis()
{
    // Длинный заголовок обрезается, и обрезка не должна разрезать букву.
    const char* line = R"({"role":"user","content":"очень длинный вопрос про стиральную машину и её фильтр"})";
    char buf[24];
    TEST_ASSERT_TRUE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));

    // Проверяем, что получился корректный UTF-8: битый хвост (начало буквы без
    // продолжения) M5GFX нарисовал бы рамкой.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(buf);
    size_t i = 0;
    while (p[i] != 0) {
        int extra = 0;
        if ((p[i] & 0x80) == 0x00)
            extra = 0;
        else if ((p[i] & 0xE0) == 0xC0)
            extra = 1;
        else if ((p[i] & 0xF0) == 0xE0)
            extra = 2;
        else if ((p[i] & 0xF8) == 0xF0)
            extra = 3;
        else
            TEST_FAIL_MESSAGE("недопустимый ведущий байт UTF-8");

        ++i;
        for (int k = 0; k < extra; ++k) {
            TEST_ASSERT_NOT_EQUAL(0, p[i]);
            TEST_ASSERT_EQUAL(0x80, p[i] & 0xC0);
            ++i;
        }
    }
}

void test_title_exact_fit_no_ellipsis()
{
    // Текст уместился целиком — многоточие не нужно.
    const char* line = R"({"role":"user","content":"коротко"})";
    char buf[64];
    TEST_ASSERT_TRUE(chatlist::title_from_line(line, strlen(line), buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("коротко", buf);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_slug_normal);
    RUN_TEST(test_slug_with_suffix);
    RUN_TEST(test_slug_uptime_not_real);
    RUN_TEST(test_slug_bad_prefix);
    RUN_TEST(test_slug_short);
    RUN_TEST(test_slug_non_digits);
    RUN_TEST(test_slug_out_of_range);
    RUN_TEST(test_format_stamp_real);
    RUN_TEST(test_format_stamp_not_real);
    RUN_TEST(test_format_stamp_small_buffer);
    RUN_TEST(test_title_user_message);
    RUN_TEST(test_title_first_line_only);
    RUN_TEST(test_title_assistant_rejected);
    RUN_TEST(test_title_meta_rejected);
    RUN_TEST(test_title_empty_content);
    RUN_TEST(test_title_broken_json);
    RUN_TEST(test_title_truncated_with_ellipsis);
    RUN_TEST(test_title_exact_fit_no_ellipsis);
    UNITY_END();
    return 0;
}
