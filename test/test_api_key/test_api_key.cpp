/*
 * Тесты проверки ключа API.
 *
 * Ошибка здесь дорогая: неверный ключ молча уходит в заголовок, запрос не
 * отправляется, а на экране это выглядит как «сервер не отвечает». Поэтому
 * проверяется именно то, что ломает HTTP: не-ASCII, пробелы, длина.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/api_key.h"

void setUp(void) {}
void tearDown(void) {}

// --- Очистка ---

void test_clean_trims_spaces()
{
    char key[] = "  sk-abcdef1234567890  ";
    const size_t n = apikey::clean(key);

    TEST_ASSERT_EQUAL_STRING("sk-abcdef1234567890", key);
    TEST_ASSERT_EQUAL_UINT32(strlen(key), n);
}

void test_clean_trims_newline_and_quotes()
{
    char key[] = "\"sk-abcdef1234567890\"\n";
    apikey::clean(key);
    TEST_ASSERT_EQUAL_STRING("sk-abcdef1234567890", key);
}

/// Внутренние дефисы и точки — часть ключа, их трогать нельзя.
void test_clean_keeps_inner_chars()
{
    char key[] = " sk-proj-abc.def_123 ";
    apikey::clean(key);
    TEST_ASSERT_EQUAL_STRING("sk-proj-abc.def_123", key);
}

void test_clean_all_spaces_becomes_empty()
{
    char key[] = "   \n\t ";
    TEST_ASSERT_EQUAL_UINT32(0, apikey::clean(key));
    TEST_ASSERT_EQUAL_STRING("", key);
}

// --- Валидация ---

void test_valid_key()
{
    char err[128];
    TEST_ASSERT_TRUE(apikey::validate("sk-abcdef1234567890abcdef", 160, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", err);
}

/// Кириллица в ключе — самая частая причина «запрос не уходит»: ключ копируют
/// вместе с пояснением. Ошибка должна быть внятной.
void test_cyrillic_rejected()
{
    char err[128];
    TEST_ASSERT_FALSE(apikey::validate("sk-ключ-1234567890abc", 160, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "ASCII"));
}

void test_space_rejected()
{
    char err[128];
    TEST_ASSERT_FALSE(apikey::validate("sk-abc def1234567890ab", 160, err, sizeof(err)));
}

/// Перевод строки в заголовке — это инъекция заголовков, а не просто опечатка.
void test_newline_rejected()
{
    char err[128];
    TEST_ASSERT_FALSE(apikey::validate("sk-abc\ndef1234567890ab", 160, err, sizeof(err)));
}

void test_empty_rejected()
{
    char err[128];
    TEST_ASSERT_FALSE(apikey::validate("", 160, err, sizeof(err)));
    TEST_ASSERT_FALSE(apikey::validate(nullptr, 160, err, sizeof(err)));
}

void test_too_short_rejected()
{
    char err[128];
    TEST_ASSERT_FALSE(apikey::validate("sk-abc", 160, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "короткий"));
}

void test_too_long_rejected()
{
    char err[128];
    char key[200];
    memset(key, 'a', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    TEST_ASSERT_FALSE(apikey::validate(key, 160, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "длиннее"));
}

/// Ровно на границе длины — годится. Ошибка на единицу здесь означала бы, что
/// валидный ключ не принимается, а это худший вид ошибки: данные верны, а
/// работает не всё.
void test_length_boundary()
{
    char err[128];
    char exact[apikey::kMinLen + 1];
    memset(exact, 'k', apikey::kMinLen);
    exact[apikey::kMinLen] = '\0';

    TEST_ASSERT_TRUE(apikey::validate(exact, 160, err, sizeof(err)));

    exact[apikey::kMinLen - 1] = '\0';
    TEST_ASSERT_FALSE(apikey::validate(exact, 160, err, sizeof(err)));
}

/// Ключ после очистки становится годным: это и есть смысл clean().
void test_clean_then_validate()
{
    char key[] = "  \"sk-abcdef1234567890\"  \n";
    apikey::clean(key);

    char err[128];
    TEST_ASSERT_TRUE(apikey::validate(key, 160, err, sizeof(err)));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_clean_trims_spaces);
    RUN_TEST(test_clean_trims_newline_and_quotes);
    RUN_TEST(test_clean_keeps_inner_chars);
    RUN_TEST(test_clean_all_spaces_becomes_empty);
    RUN_TEST(test_valid_key);
    RUN_TEST(test_cyrillic_rejected);
    RUN_TEST(test_space_rejected);
    RUN_TEST(test_newline_rejected);
    RUN_TEST(test_empty_rejected);
    RUN_TEST(test_too_short_rejected);
    RUN_TEST(test_too_long_rejected);
    RUN_TEST(test_length_boundary);
    RUN_TEST(test_clean_then_validate);
    return UNITY_END();
}
