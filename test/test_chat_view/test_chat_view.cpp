/*
 * Тесты видимого окна ленты чата.
 *
 * Ошибка здесь не «некрасиво», а «на экране пустота»: неверный подсчёт строк
 * сдвигает окно за пределы ленты, и пользователь видит чёрный экран вместо
 * ответа. Поэтому проверяется и арифметика, и краевые случаи.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/chat_view.h"

void setUp(void) {}
void tearDown(void) {}

// --- Строк на странице ---

void test_lines_per_page_exact()
{
    TEST_ASSERT_EQUAL_UINT32(10, chatview::lines_per_page(200, 20));
}

void test_lines_per_page_rounds_down()
{
    TEST_ASSERT_EQUAL_UINT32(9, chatview::lines_per_page(199, 20));
}

void test_lines_per_page_zero_height()
{
    TEST_ASSERT_EQUAL_UINT32(0, chatview::lines_per_page(0, 20));
    TEST_ASSERT_EQUAL_UINT32(0, chatview::lines_per_page(199, 0));
    TEST_ASSERT_EQUAL_UINT32(0, chatview::lines_per_page(-50, 20));
}

// --- Окно ---

void test_empty_thread()
{
    const chatview::Window w = chatview::compute(nullptr, 0, 10, 0);

    TEST_ASSERT_EQUAL_INT(-1, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(0, w.visible_count);
    TEST_ASSERT_FALSE(w.clipped_top);
}

void test_single_short_message()
{
    const uint16_t lines[] = {2};
    const chatview::Window w = chatview::compute(lines, 1, 10, 0);

    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(0, w.first_line);
    TEST_ASSERT_EQUAL_UINT32(1, w.visible_count);
    TEST_ASSERT_EQUAL_UINT32(2, w.lines_used);
    TEST_ASSERT_FALSE(w.clipped_top);
    TEST_ASSERT_FALSE(w.clipped_bottom);
}

/// Свежие сообщения важнее старых: окно начинается с конца ленты.
void test_shows_tail_of_conversation()
{
    const uint16_t lines[] = {3, 3, 3, 3};
    const chatview::Window w = chatview::compute(lines, 4, 6, 0);

    TEST_ASSERT_EQUAL_INT(2, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(2, w.visible_count);
    TEST_ASSERT_EQUAL_UINT32(6, w.lines_used);
    TEST_ASSERT_TRUE(w.clipped_top);
}

/// Ответ длиннее экрана: показываем его хвост, чтобы был виден последний
/// напечатанный токен.
void test_long_message_shows_tail()
{
    const uint16_t lines[] = {40};
    const chatview::Window w = chatview::compute(lines, 1, 10, 0);

    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(30, w.first_line);
    TEST_ASSERT_EQUAL_UINT32(10, w.lines_used);
    TEST_ASSERT_TRUE(w.clipped_top);
}

/// Прокрутка вверх: снизу остаётся скрытая часть, и окно сдвигается.
void test_scroll_up_skips_from_bottom()
{
    const uint16_t lines[] = {2, 2, 2};
    const chatview::Window w = chatview::compute(lines, 3, 4, 2);

    // Нижнее сообщение (2 строки) уехало за край, освободившееся место заняли
    // предыдущие два — иначе на экране была бы пустота.
    TEST_ASSERT_TRUE(w.clipped_bottom);
    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(2, w.visible_count);
    TEST_ASSERT_EQUAL_UINT32(4, w.lines_used);
}

/// Прокрутка внутри длинного сообщения: показываем его середину, а не начало.
void test_scroll_inside_long_message()
{
    const uint16_t lines[] = {30};
    const chatview::Window w = chatview::compute(lines, 1, 10, 10);

    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(10, w.first_line);
    TEST_ASSERT_TRUE(w.clipped_bottom);
    TEST_ASSERT_TRUE(w.clipped_top);
}

/// Прокрутка дальше начала ленты не должна давать отрицательный индекс.
void test_scroll_beyond_top_is_clamped()
{
    const uint16_t lines[] = {2, 2};
    const chatview::Window w = chatview::compute(lines, 2, 10, 500);

    // Всё уехало вверх: показывать нечего, но и не падаем.
    TEST_ASSERT_TRUE(w.first_msg == -1 || w.first_msg >= 0);
    TEST_ASSERT_TRUE(w.visible_count <= 2);
}

/// Пустые сообщения не занимают строк — но и не ломают подсчёт: у пустого
/// сообщения height 0, а пустая строка всё равно занимает место в ленте.
void test_zero_line_message_counted_as_one()
{
    const uint16_t lines[] = {0};
    const chatview::Window w = chatview::compute(lines, 1, 10, 0);

    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(1, w.lines_used);
}

/// Ровно влезающая лента не должна помечаться как обрезанная сверху: иначе
/// на экране появится ложный индикатор «выше есть ещё».
void test_exactly_fits_not_clipped()
{
    const uint16_t lines[] = {2, 2, 2};
    const chatview::Window w = chatview::compute(lines, 3, 6, 0);

    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(3, w.visible_count);
    TEST_ASSERT_FALSE(w.clipped_top);
}

/// Верхнее сообщение не влезает целиком, а нижнее — влезает. Свежее сообщение
/// (то, ради чего экран открыли) должно быть нарисовано: обнуление счётчика
/// здесь оставляло бы внизу пустые строки, а ответ модели — невидимым.
void test_partial_top_keeps_newest()
{
    const uint16_t lines[] = {6, 3};
    const chatview::Window w = chatview::compute(lines, 2, 6, 0);

    TEST_ASSERT_EQUAL_INT(0, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(2, w.visible_count);
    TEST_ASSERT_EQUAL_UINT32(6, w.lines_used);
    TEST_ASSERT_TRUE(w.clipped_top);
    TEST_ASSERT_FALSE(w.clipped_bottom);
}

void test_zero_max_lines()
{
    const uint16_t lines[] = {2};
    const chatview::Window w = chatview::compute(lines, 1, 0, 0);

    TEST_ASSERT_EQUAL_INT(-1, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(0, w.visible_count);
}

/// Много сообщений по одной строке: влезает ровно столько, сколько строк.
void test_many_single_line_messages()
{
    const uint16_t lines[] = {1, 1, 1, 1, 1};
    const chatview::Window w = chatview::compute(lines, 5, 3, 0);

    TEST_ASSERT_EQUAL_INT(2, w.first_msg);
    TEST_ASSERT_EQUAL_UINT32(3, w.visible_count);
    TEST_ASSERT_TRUE(w.clipped_top);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_lines_per_page_exact);
    RUN_TEST(test_lines_per_page_rounds_down);
    RUN_TEST(test_lines_per_page_zero_height);
    RUN_TEST(test_empty_thread);
    RUN_TEST(test_single_short_message);
    RUN_TEST(test_shows_tail_of_conversation);
    RUN_TEST(test_long_message_shows_tail);
    RUN_TEST(test_scroll_up_skips_from_bottom);
    RUN_TEST(test_scroll_inside_long_message);
    RUN_TEST(test_scroll_beyond_top_is_clamped);
    RUN_TEST(test_zero_line_message_counted_as_one);
    RUN_TEST(test_exactly_fits_not_clipped);
    RUN_TEST(test_partial_top_keeps_newest);
    RUN_TEST(test_zero_max_lines);
    RUN_TEST(test_many_single_line_messages);
    return UNITY_END();
}
