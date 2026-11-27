/*
 * Тесты истории введённых вопросов.
 *
 * На плате это проверяется нажатием Alt+↑, а вот кольцо на 20 записей и
 * сохранение черновика глазами не проверишь: чтобы увидеть переполнение, надо
 * задать двадцать один вопрос подряд. Поэтому логика проверяется на хосте.
 */

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "util/input_history.h"

namespace {

/// Арена как в прошивке, но в статической памяти: тесты не должны зависеть от
/// кучи, иначе падение теста не отличить от нехватки памяти.
char arena[inputhist::kArenaBytes];

inputhist::History fresh()
{
    inputhist::History h;
    inputhist::init(h, arena, sizeof(arena));
    inputhist::clear(h);
    return h;
}

inputhist::History with(const char* const* texts, size_t n)
{
    inputhist::History h = fresh();
    for (size_t i = 0; i < n; ++i)
        inputhist::add(h, texts[i], strlen(texts[i]));
    return h;
}

/// Пролистать вверх n раз и вернуть последний полученный текст.
const char* up_n(inputhist::History& h, size_t n, size_t* len)
{
    const char* out = nullptr;
    for (size_t i = 0; i < n; ++i)
        out = inputhist::up(h, nullptr, 0, len);
    return out;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_init_rejects_small_arena()
{
    inputhist::History h;
    TEST_ASSERT_FALSE(inputhist::init(h, arena, inputhist::kArenaBytes - 1));
    TEST_ASSERT_FALSE(inputhist::init(h, nullptr, sizeof(arena)));
    // Отказ не должен оставлять «рабочую» историю: иначе add() писал бы в
    // nullptr, и плата падала бы при первом же вопросе.
    TEST_ASSERT_NULL(h.arena);
}

void test_empty_history_gives_nothing()
{
    inputhist::History h = fresh();
    size_t len = 123;
    TEST_ASSERT_NULL(inputhist::up(h, nullptr, 0, &len));
    TEST_ASSERT_EQUAL_UINT(0, len);
    TEST_ASSERT_NULL(inputhist::down(h, &len));
    TEST_ASSERT_TRUE(inputhist::at_draft(h));
    TEST_ASSERT_EQUAL_UINT(0, inputhist::size(h));
}

void test_add_and_up_returns_newest_first()
{
    static const char* const texts[] = {"первый", "второй", "третий"};
    inputhist::History h = with(texts, 3);
    TEST_ASSERT_EQUAL_UINT(3, inputhist::size(h));

    size_t len = 0;
    const char* text = inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_EQUAL_STRING("третий", text);
    TEST_ASSERT_EQUAL_UINT(strlen("третий"), len);

    text = inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_EQUAL_STRING("второй", text);
    text = inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_EQUAL_STRING("первый", text);

    // Дальше вверх некуда: самое старое уже на экране, поле менять не надо.
    TEST_ASSERT_NULL(inputhist::up(h, nullptr, 0, &len));
}

void test_down_walks_back_to_draft()
{
    static const char* const texts[] = {"старый", "новый"};
    inputhist::History h = with(texts, 2);

    size_t len = 0;
    TEST_ASSERT_EQUAL_STRING("новый", inputhist::up(h, nullptr, 0, &len));
    TEST_ASSERT_EQUAL_STRING("старый", inputhist::up(h, nullptr, 0, &len));

    // Вниз — обратно к более новым, и последний шаг возвращает черновик.
    TEST_ASSERT_EQUAL_STRING("новый", inputhist::down(h, &len));
    const char* text = inputhist::down(h, &len);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_UINT(0, len);
    TEST_ASSERT_TRUE(inputhist::at_draft(h));

    // И дальше вниз уже некуда.
    TEST_ASSERT_NULL(inputhist::down(h, &len));
}

void test_draft_is_preserved()
{
    static const char* const texts[] = {"вчерашний вопрос"};
    inputhist::History h = with(texts, 1);

    // Человек начал набирать новый вопрос и полез в историю.
    const char* draft = "как сбро";
    size_t len = 0;
    const char* text = inputhist::up(h, draft, strlen(draft), &len);
    TEST_ASSERT_EQUAL_STRING("вчерашний вопрос", text);

    // Alt+↓ возвращает ровно то, что было набрано, а не пустоту.
    text = inputhist::down(h, &len);
    TEST_ASSERT_EQUAL_STRING("как сбро", text);
    TEST_ASSERT_EQUAL_UINT(strlen(draft), len);
    TEST_ASSERT_TRUE(inputhist::at_draft(h));
}

void test_draft_saved_only_once()
{
    static const char* const texts[] = {"а", "б"};
    inputhist::History h = with(texts, 2);

    size_t len = 0;
    inputhist::up(h, "черновик", strlen("черновик"), &len);
    // Второй шаг вверх передаёт уже вытащенный вопрос, но черновик не должен
    // перезаписаться им: иначе Alt+↓ вернул бы «б» вместо набранного.
    inputhist::up(h, "б", strlen("б"), &len);

    // Вниз сначала проходит записи и только потом доходит до черновика.
    TEST_ASSERT_EQUAL_STRING("б", inputhist::down(h, &len));
    TEST_ASSERT_EQUAL_STRING("черновик", inputhist::down(h, &len));
    TEST_ASSERT_TRUE(inputhist::at_draft(h));
}

void test_ring_overflows_keeping_newest()
{
    // 25 вопросов при 20 слотах: старые пять должны уйти, новые остаться.
    char buf[32];
    inputhist::History h = fresh();
    for (int i = 0; i < 25; ++i) {
        snprintf(buf, sizeof(buf), "вопрос-%d", i);
        TEST_ASSERT_TRUE(inputhist::add(h, buf, strlen(buf)));
    }
    TEST_ASSERT_EQUAL_UINT(inputhist::kMaxEntries, inputhist::size(h));

    size_t len = 0;
    const char* text = inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_EQUAL_STRING("вопрос-24", text);

    // Самое старое из оставшихся — вопрос-5, а не вопрос-0.
    for (int i = 0; i < 19; ++i)
        text = inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_EQUAL_STRING("вопрос-5", text);
    TEST_ASSERT_NULL(inputhist::up(h, nullptr, 0, &len));
}

void test_consecutive_duplicates_collapse()
{
    inputhist::History h = fresh();
    inputhist::add(h, "привет", 6);
    inputhist::add(h, "привет", 6);
    inputhist::add(h, "привет", 6);
    TEST_ASSERT_EQUAL_UINT(1, inputhist::size(h));

    // А разные вопросы не склеиваются, даже если один — подстрока другого.
    inputhist::add(h, "привет", 6);
    inputhist::add(h, "привет!", 7);
    inputhist::add(h, "привет", 6);
    TEST_ASSERT_EQUAL_UINT(3, inputhist::size(h));
}

void test_add_rejects_empty_and_too_long()
{
    inputhist::History h = fresh();
    TEST_ASSERT_FALSE(inputhist::add(h, "", 0));
    TEST_ASSERT_FALSE(inputhist::add(h, nullptr, 5));

    // Ровно в слот — влезает (нужен ещё байт под завершающий ноль).
    char big[inputhist::kEntryCap];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_FALSE(inputhist::add(h, big, inputhist::kEntryCap));

    // А на байт меньше — уже да.
    TEST_ASSERT_TRUE(inputhist::add(h, big, inputhist::kEntryCap - 1));
    TEST_ASSERT_EQUAL_UINT(1, inputhist::size(h));
}

void test_add_resets_listing()
{
    static const char* const texts[] = {"а", "б"};
    inputhist::History h = with(texts, 2);

    size_t len = 0;
    inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_FALSE(inputhist::at_draft(h));

    // Отправили новый вопрос — поле очищено, листание начинается заново.
    inputhist::add(h, "в", strlen("в"));
    TEST_ASSERT_TRUE(inputhist::at_draft(h));
    TEST_ASSERT_EQUAL_STRING("в", inputhist::up(h, nullptr, 0, &len));
}

void test_clear_forgets_everything()
{
    static const char* const texts[] = {"а", "б"};
    inputhist::History h = with(texts, 2);
    size_t len = 0;
    inputhist::up(h, "черновик", 8, &len);

    inputhist::clear(h);
    TEST_ASSERT_EQUAL_UINT(0, inputhist::size(h));
    TEST_ASSERT_TRUE(inputhist::at_draft(h));
    TEST_ASSERT_NULL(inputhist::up(h, nullptr, 0, &len));
    // Черновик тоже забыт: он относился к прежнему треду.
    TEST_ASSERT_NULL(inputhist::down(h, &len));
}

void test_to_draft_restarts_listing()
{
    static const char* const texts[] = {"а", "б", "в"};
    inputhist::History h = with(texts, 3);

    size_t len = 0;
    TEST_ASSERT_EQUAL_STRING("в", inputhist::up(h, nullptr, 0, &len));
    TEST_ASSERT_EQUAL_STRING("б", inputhist::up(h, nullptr, 0, &len));
    TEST_ASSERT_FALSE(inputhist::at_draft(h));

    // Поле очистили вручную (Esc или Del) — листание должно начаться заново
    // с самой свежей записи, а не уводить ещё глубже.
    inputhist::to_draft(h);
    TEST_ASSERT_TRUE(inputhist::at_draft(h));
    TEST_ASSERT_EQUAL_STRING("в", inputhist::up(h, nullptr, 0, &len));

    // Записи при этом никуда не делись.
    TEST_ASSERT_EQUAL_UINT(3, inputhist::size(h));
}

void test_cyrillic_length_is_bytes()
{
    // Длина в байтах, а не в символах: «ёж» — 4 байта. Ошибка здесь обрезала
    // бы вопрос посреди буквы, и в поле появился бы мусор.
    inputhist::History h = fresh();
    TEST_ASSERT_TRUE(inputhist::add(h, "ёж", strlen("ёж")));

    size_t len = 0;
    const char* text = inputhist::up(h, nullptr, 0, &len);
    TEST_ASSERT_EQUAL_UINT(4, len);
    TEST_ASSERT_EQUAL_STRING("ёж", text);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_rejects_small_arena);
    RUN_TEST(test_empty_history_gives_nothing);
    RUN_TEST(test_add_and_up_returns_newest_first);
    RUN_TEST(test_down_walks_back_to_draft);
    RUN_TEST(test_draft_is_preserved);
    RUN_TEST(test_draft_saved_only_once);
    RUN_TEST(test_ring_overflows_keeping_newest);
    RUN_TEST(test_consecutive_duplicates_collapse);
    RUN_TEST(test_add_rejects_empty_and_too_long);
    RUN_TEST(test_add_resets_listing);
    RUN_TEST(test_clear_forgets_everything);
    RUN_TEST(test_to_draft_restarts_listing);
    RUN_TEST(test_cyrillic_length_is_bytes);
    return UNITY_END();
}
