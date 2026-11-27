/*
 * Тесты арифметики страниц списков.
 *
 * На плате это видно только числами в заголовке, и ошибка выглядит безобидно:
 * «60 из 137» вместо «60 из 60». Но если `has_more` соврёт, остаток архива
 * прячется целиком, и человек делает вывод «моей заметки тут нет». Поэтому
 * границы проверяются на хосте.
 */

#include <unity.h>

#include <string.h>

#include "util/list_page.h"

void setUp(void) {}
void tearDown(void) {}

// --- Сколько показано -------------------------------------------------------

void test_shown_full_page()
{
    TEST_ASSERT_EQUAL_UINT(60, listpage::shown(137, 0, 60));
}

void test_shown_last_partial_page()
{
    // Последняя страница короче предела: показано ровно то, что прочиталось.
    TEST_ASSERT_EQUAL_UINT(17, listpage::shown(137, 120, 60));
}

void test_shown_never_exceeds_total()
{
    // Файлы удалили между чтениями — «показано» не должно обгонять «всего».
    TEST_ASSERT_EQUAL_UINT(5, listpage::shown(125, 120, 60));
    // Смещение за концом — ничего.
    TEST_ASSERT_EQUAL_UINT(0, listpage::shown(137, 137, 60));
    TEST_ASSERT_EQUAL_UINT(0, listpage::shown(0, 0, 60));
}

// --- Есть ли продолжение ----------------------------------------------------

void test_has_more_after_full_page()
{
    TEST_ASSERT_TRUE(listpage::has_more(137, 0, 60));
}

void test_no_more_when_everything_fits()
{
    TEST_ASSERT_FALSE(listpage::has_more(60, 0, 60));
    TEST_ASSERT_FALSE(listpage::has_more(12, 0, 60));
    TEST_ASSERT_FALSE(listpage::has_more(0, 0, 0));
}

void test_no_more_on_exactly_last_page()
{
    // 120 при странице 60: после второй страницы продолжения нет. Ошибка на
    // единицу здесь показала бы «ещё 0» и лишний раз сходила на карту.
    TEST_ASSERT_TRUE(listpage::has_more(120, 0, 60));
    TEST_ASSERT_FALSE(listpage::has_more(120, 60, 60));
}

void test_has_more_clamped_by_total()
{
    // Прочитали больше, чем осталось: продолжения всё равно нет.
    TEST_ASSERT_FALSE(listpage::has_more(137, 120, 60));
}

// --- Следующее смещение -----------------------------------------------------

void test_next_offset_advances_by_loaded()
{
    TEST_ASSERT_EQUAL_UINT(60, listpage::next_offset(137, 0, 60));
    TEST_ASSERT_EQUAL_UINT(120, listpage::next_offset(137, 60, 60));
    // Последняя страница: смещение упирается в конец, а не уходит за него.
    TEST_ASSERT_EQUAL_UINT(137, listpage::next_offset(137, 120, 60));
}

void test_next_offset_stays_when_nothing_loaded()
{
    // Ничего не прочиталось (карта отвалилась) — смещение не двигаем, иначе
    // следующая попытка пропустила бы страницу.
    TEST_ASSERT_EQUAL_UINT(0, listpage::next_offset(137, 0, 0));
}

// --- Подпись в заголовке ----------------------------------------------------

void test_label_shows_fraction_when_truncated()
{
    char out[64];
    TEST_ASSERT_TRUE(listpage::label("Документы", 137, 60, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("Документы: 60 из 137", out);
}

void test_label_shows_one_number_when_all_fit()
{
    char out[64];
    TEST_ASSERT_TRUE(listpage::label("Треды", 12, 12, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("Треды: 12", out);

    // «12 из 12» не сообщает ничего нового — числа не повторяем.
    TEST_ASSERT_NULL(strstr(out, "из"));
}

void test_label_empty_list()
{
    char out[64];
    TEST_ASSERT_TRUE(listpage::label("Документы", 0, 0, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("Документы: 0", out);
}

void test_label_respects_small_buffer()
{
    char small[8];
    TEST_ASSERT_EQUAL_UINT(0, listpage::label("Документы", 137, 60, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
}

// --- Подсказка о продолжении ------------------------------------------------

void test_more_hint_counts_remaining()
{
    char out[64];
    TEST_ASSERT_TRUE(listpage::more_hint(77, out, sizeof(out)) > 0);
    // Стрелка та же, которой листают: страницы ходят «← →», и «↓» обещала бы
    // движение вниз, которого нет.
    TEST_ASSERT_EQUAL_STRING("→ — ещё 77", out);
}

void test_more_hint_empty_when_nothing_left()
{
    char out[64];
    TEST_ASSERT_EQUAL_UINT(0, listpage::more_hint(0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_shown_full_page);
    RUN_TEST(test_shown_last_partial_page);
    RUN_TEST(test_shown_never_exceeds_total);

    RUN_TEST(test_has_more_after_full_page);
    RUN_TEST(test_no_more_when_everything_fits);
    RUN_TEST(test_no_more_on_exactly_last_page);
    RUN_TEST(test_has_more_clamped_by_total);

    RUN_TEST(test_next_offset_advances_by_loaded);
    RUN_TEST(test_next_offset_stays_when_nothing_loaded);

    RUN_TEST(test_label_shows_fraction_when_truncated);
    RUN_TEST(test_label_shows_one_number_when_all_fit);
    RUN_TEST(test_label_empty_list);
    RUN_TEST(test_label_respects_small_buffer);

    RUN_TEST(test_more_hint_counts_remaining);
    RUN_TEST(test_more_hint_empty_when_nothing_left);

    return UNITY_END();
}
