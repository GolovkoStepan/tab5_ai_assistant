/*
 * Тесты правила показа баннера офлайн-режима.
 *
 * На плате это видно только глазами и только в нужный момент, поэтому правило
 * проверяется на хосте:
 *   - при подключении баннера нет;
 *   - во время подключения его тоже нет («сеть появляется», а не «нет сети»);
 *   - сообщение в полосе важнее баннера.
 */

#include <unity.h>

#include <string.h>

#include "util/offline.h"

void setUp(void) {}
void tearDown(void) {}

void test_hidden_when_connected()
{
    TEST_ASSERT_FALSE(offline::banner_visible(true, false, false));
    // Даже если идёт подключение к другой сети — связь есть, баннер не нужен.
    TEST_ASSERT_FALSE(offline::banner_visible(true, true, false));
}

void test_hidden_while_connecting()
{
    // Автоподключение занимает ~9 с после загрузки. Показать «офлайн» в эти
    // секунды значит соврать там, где человек как раз ждёт подключения.
    TEST_ASSERT_FALSE(offline::banner_visible(false, true, false));
}

void test_shown_when_offline()
{
    TEST_ASSERT_TRUE(offline::banner_visible(false, false, false));
}

void test_message_wins()
{
    // Ошибка запроса или подсказка в полосе важнее: две строки в одном месте
    // спорили бы друг с другом.
    TEST_ASSERT_FALSE(offline::banner_visible(false, false, true));
}

void test_text_says_what_works()
{
    const char* text = offline::banner_text();
    // Первым делом — что ДОСТУПНО, а не чего нет: человеку нужно знать, что
    // делать дальше.
    TEST_ASSERT_NOT_NULL(strstr(text, "архив"));
    TEST_ASSERT_NOT_NULL(strstr(text, "Ctrl+O"));
    TEST_ASSERT_NOT_NULL(strstr(text, "Ctrl+T"));
    // И как вернуть сеть.
    TEST_ASSERT_NOT_NULL(strstr(text, "Ctrl+W"));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_hidden_when_connected);
    RUN_TEST(test_hidden_while_connecting);
    RUN_TEST(test_shown_when_offline);
    RUN_TEST(test_message_wins);
    RUN_TEST(test_text_says_what_works);
    return UNITY_END();
}
