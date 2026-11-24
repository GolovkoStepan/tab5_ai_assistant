/*
 * Тесты выбора сообщений для пересчёта строк ленты.
 *
 * Баг, ради которого эти тесты написаны, был найден на живой плате. Ответ
 * модели на 5 КБ отображался двумя строками, а стрелки не двигали ленту:
 * измерение пропустило растущее последнее сообщение, total_lines оказался
 * меньше высоты экрана.
 *
 * Причина была не в арифметике переноса, а в условии «когда перемерять»:
 * пересчёт последнего сообщения был привязан к режиму экрана, а не к тому,
 * изменились ли данные. Ответ, пришедший целиком за один tick(), оставался
 * неизмеренным. Тест закрывает именно это условие.
 */

#include <unity.h>

#include "util/chat_measure.h"

void setUp(void) {}
void tearDown(void) {}

// --- Тред не менялся ---

void test_nothing_to_measure()
{
    TEST_ASSERT_EQUAL_UINT32(5, chatmeasure::first_to_measure(5, 5, 100, 100));
}

void test_empty_thread()
{
    TEST_ASSERT_EQUAL_UINT32(0, chatmeasure::first_to_measure(0, 0, 0, 0));
}

// --- Появились новые сообщения ---

void test_new_messages_measured()
{
    // Было измерено 2, в треде 4: считаем с третьего.
    TEST_ASSERT_EQUAL_UINT32(2, chatmeasure::first_to_measure(2, 4, 50, 10));
}

void test_thread_shorter_resets()
{
    // Новый тред или загрузка: прежний кэш неверен целиком.
    TEST_ASSERT_EQUAL_UINT32(0, chatmeasure::first_to_measure(5, 2, 50, 10));
}

// --- Растущий ответ: главный случай ---

void test_growing_answer_remeasured()
{
    // Все сообщения измерены, но последнее выросло: перемеряем только его.
    // Это и есть случай, который ломался: ответ пришёл целиком за один
    // tick(), режим экрана уже Idle, а длина последнего сообщения изменилась.
    TEST_ASSERT_EQUAL_UINT32(4, chatmeasure::first_to_measure(5, 5, 5000, 0));
}

void test_growing_answer_from_empty()
{
    // Ответ только начался: было 0 байт, стало 60.
    TEST_ASSERT_EQUAL_UINT32(1, chatmeasure::first_to_measure(2, 2, 60, 0));
}

void test_measured_len_zero_counts_as_change()
{
    // Именно так выглядел баг на плате: пустой ответ запомнен как 0, потом
    // пришло 5327 байт. Разница обязана заставить перемерить.
    TEST_ASSERT_EQUAL_UINT32(1, chatmeasure::first_to_measure(2, 2, 5327, 0));
}

void test_same_length_no_remeasure()
{
    // Длина не изменилась — пересчёт не нужен: стриминг закончился, и кадры
    // идут по другим причинам (сеть, индикатор ожидания).
    TEST_ASSERT_EQUAL_UINT32(3, chatmeasure::first_to_measure(3, 3, 42, 42));
}

void test_finished_message_same_len_no_remeasure()
{
    // finish_last() правит время и токены, но не текст: перемерять незачем.
    TEST_ASSERT_EQUAL_UINT32(7, chatmeasure::first_to_measure(7, 7, 1234, 1234));
}

// --- Одно сообщение ---

void test_single_message_grown()
{
    TEST_ASSERT_EQUAL_UINT32(0, chatmeasure::first_to_measure(1, 1, 100, 0));
}

void test_single_message_measured()
{
    TEST_ASSERT_EQUAL_UINT32(1, chatmeasure::first_to_measure(1, 1, 100, 100));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_nothing_to_measure);
    RUN_TEST(test_empty_thread);
    RUN_TEST(test_new_messages_measured);
    RUN_TEST(test_thread_shorter_resets);
    RUN_TEST(test_growing_answer_remeasured);
    RUN_TEST(test_growing_answer_from_empty);
    RUN_TEST(test_measured_len_zero_counts_as_change);
    RUN_TEST(test_same_length_no_remeasure);
    RUN_TEST(test_finished_message_same_len_no_remeasure);
    RUN_TEST(test_single_message_grown);
    RUN_TEST(test_single_message_measured);

    return UNITY_END();
}
