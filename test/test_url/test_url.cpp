/*
 * Тесты разбора адреса API.
 *
 * Адрес хранится одной строкой в config.json и правится руками, поэтому
 * проверяются все варианты, которые человек реально может написать —
 * с портом, без схемы, с завершающим слэшем. Ошибка разбора на плате
 * выглядит как «не удалось подключиться» без причины, и искать её потом
 * дорого.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/url.h"

void setUp(void) {}
void tearDown(void) {}

// --- Успешный разбор ---

void test_https_simple()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("https://routerai.ru/api/v1", p, err, sizeof(err)));
    TEST_ASSERT_TRUE(p.tls);
    TEST_ASSERT_EQUAL_STRING("routerai.ru", p.host);
    TEST_ASSERT_EQUAL_UINT16(443, p.port);
    TEST_ASSERT_EQUAL_STRING("/api/v1", p.path);
}

void test_http_simple()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("http://example.com/x", p, err, sizeof(err)));
    TEST_ASSERT_FALSE(p.tls);
    TEST_ASSERT_EQUAL_UINT16(80, p.port);
    TEST_ASSERT_EQUAL_STRING("example.com", p.host);
}

/// Порт в адресе: иначе локальный прокси не проверить.
void test_explicit_port()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("http://192.168.1.5:8080/v1", p, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("192.168.1.5", p.host);
    TEST_ASSERT_EQUAL_UINT16(8080, p.port);
    TEST_ASSERT_EQUAL_STRING("/v1", p.path);
}

/// Завершающий слэш не должен давать двойной при склейке с эндпоинтом:
/// «//chat/completions» — это 404 от сервера.
void test_trailing_slash_removed()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("https://routerai.ru/api/v1/", p, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("/api/v1", p.path);

    TEST_ASSERT_TRUE(url::append_path(p, "/chat/completions"));
    TEST_ASSERT_EQUAL_STRING("/api/v1/chat/completions", p.path);
    TEST_ASSERT_NULL(strstr(p.path, "//"));
}

/// Адрес без пути: типичный случай «https://routerai.ru».
void test_no_path()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("https://routerai.ru", p, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("routerai.ru", p.host);
    TEST_ASSERT_EQUAL_STRING("", p.path);

    TEST_ASSERT_TRUE(url::append_path(p, "chat/completions"));
    TEST_ASSERT_EQUAL_STRING("/chat/completions", p.path);
}

/// Корень «/» — тот же пустой путь.
void test_only_slash_path()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("https://routerai.ru/", p, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", p.path);
}

/// Несколько завершающих слэшей — тоже схлопываются.
void test_multiple_trailing_slashes()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("https://h/path///", p, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("/path", p.path);
}

/// Сегмент уже со слэшем: двойного быть не должно.
void test_append_segment_with_leading_slash()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_TRUE(url::parse("https://h/a/b", p, err, sizeof(err)));
    TEST_ASSERT_TRUE(url::append_path(p, "/c"));
    TEST_ASSERT_EQUAL_STRING("/a/b/c", p.path);
}

// --- Ошибки ---

void test_missing_scheme()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("routerai.ru/api", p, err, sizeof(err)));
    TEST_ASSERT_NOT_EQUAL('\0', err[0]);
}

void test_empty_address()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("", p, err, sizeof(err)));
    TEST_ASSERT_NOT_EQUAL('\0', err[0]);
}

void test_null_address()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse(nullptr, p, err, sizeof(err)));
}

void test_no_host()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("https:///path", p, err, sizeof(err)));
    TEST_ASSERT_NOT_EQUAL('\0', err[0]);
}

/// Порт 70000 — частая опечатка. Молча привести к 65535 нельзя: подключение
/// уйдёт не туда, и причина будет неочевидна.
void test_port_out_of_range()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("http://h:70000/", p, err, sizeof(err)));
    TEST_ASSERT_FALSE(url::parse("http://h:0/", p, err, sizeof(err)));
}

void test_port_not_a_number()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("http://h:abc/", p, err, sizeof(err)));
}

void test_too_long_host()
{
    url::Parts p;
    char err[128];

    char addr[512] = "https://";
    const size_t prefix = strlen(addr);
    memset(addr + prefix, 'a', 400);
    addr[prefix + 400] = '\0';

    TEST_ASSERT_FALSE(url::parse(addr, p, err, sizeof(err)));
    TEST_ASSERT_NOT_EQUAL('\0', err[0]);
}

void test_too_long_path()
{
    url::Parts p;
    char err[128];

    char addr[512] = "https://h/";
    const size_t prefix = strlen(addr);
    memset(addr + prefix, 'p', 400);
    addr[prefix + 400] = '\0';

    TEST_ASSERT_FALSE(url::parse(addr, p, err, sizeof(err)));
}

/// Запрос в адресе — признак того, что в base_url скопировали готовый URL
/// запроса, а не базовый адрес. Лучше сказать об этом сразу.
void test_query_string_rejected()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("https://h?x=1", p, err, sizeof(err)));
}

/// Схема в верхнем регистре — не ошибка ввода, но и не то, что нам нужно:
/// HTTPClient ждёт конкретно http/https.
void test_uppercase_scheme_rejected()
{
    url::Parts p;
    char err[128];

    TEST_ASSERT_FALSE(url::parse("HTTPS://h/p", p, err, sizeof(err)));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_https_simple);
    RUN_TEST(test_http_simple);
    RUN_TEST(test_explicit_port);
    RUN_TEST(test_trailing_slash_removed);
    RUN_TEST(test_no_path);
    RUN_TEST(test_only_slash_path);
    RUN_TEST(test_multiple_trailing_slashes);
    RUN_TEST(test_append_segment_with_leading_slash);
    RUN_TEST(test_missing_scheme);
    RUN_TEST(test_empty_address);
    RUN_TEST(test_null_address);
    RUN_TEST(test_no_host);
    RUN_TEST(test_port_out_of_range);
    RUN_TEST(test_port_not_a_number);
    RUN_TEST(test_too_long_host);
    RUN_TEST(test_too_long_path);
    RUN_TEST(test_query_string_rejected);
    RUN_TEST(test_uppercase_scheme_rejected);
    return UNITY_END();
}
