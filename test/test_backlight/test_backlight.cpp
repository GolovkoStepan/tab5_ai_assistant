/*
 * Тесты правил таймаута подсветки.
 *
 * Проверяется то, что на плате видно только через минуты ожидания:
 *   - экран не гаснет, пока идёт ответ модели;
 *   - любая клавиша и любой тап возвращают подсветку;
 *   - гашение не отменяет яркость из настроек.
 */

#include <unity.h>

#include "util/backlight.h"

void setUp(void) {}
void tearDown(void) {}

void test_timeout_zero_disables()
{
    // timeout_min = 0 в config.json означает «не гасить вовсе», а не «гасить
    // сразу»: ноль в этой настройке читается как «выключено».
    TEST_ASSERT_EQUAL_UINT32(0, backlight::timeout_ms(0));
}

void test_timeout_minutes_to_ms()
{
    TEST_ASSERT_EQUAL_UINT32(60000, backlight::timeout_ms(1));
    TEST_ASSERT_EQUAL_UINT32(120000, backlight::timeout_ms(2));
    TEST_ASSERT_EQUAL_UINT32(3600000, backlight::timeout_ms(60));
    // Верхняя граница настройки: 255 минут не должны переполнить uint32.
    TEST_ASSERT_EQUAL_UINT32(15300000u, backlight::timeout_ms(255));
}

void test_decide_busy_keeps_light()
{
    // Идёт ответ модели: экран не гаснет, даже если таймаут истёк. Человек
    // смотрит именно на приходящий текст.
    backlight::Activity a;
    a.busy = true;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::None), static_cast<int>(backlight::decide(true, true, a)));
}

void test_decide_busy_wakes_if_late()
{
    // Работа началась, когда экран уже погас (например, вопрос пришёл по
    // Serial). Показать происходящее важнее, чем сберечь батарею.
    backlight::Activity a;
    a.busy = true;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::On), static_cast<int>(backlight::decide(false, true, a)));
}

void test_decide_key_wakes()
{
    backlight::Activity a;
    a.key = true;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::On), static_cast<int>(backlight::decide(false, true, a)));
    // На горящем экране нажатие ничего не меняет: мигать от каждого события
    // незачем.
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::None), static_cast<int>(backlight::decide(true, true, a)));
}

void test_decide_touch_wakes()
{
    backlight::Activity a;
    a.touch = true;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::On), static_cast<int>(backlight::decide(false, true, a)));
}

void test_decide_activity_beats_timeout()
{
    // Событие пришло в тот же кадр, что истёк таймаут: человек как раз тронул
    // устройство, гасить незачем. Порядок проверок здесь и есть правило.
    backlight::Activity a;
    a.key = true;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::None), static_cast<int>(backlight::decide(true, true, a)));
}

void test_decide_off_after_timeout()
{
    backlight::Activity a;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::Off), static_cast<int>(backlight::decide(true, true, a)));
    // Таймаут не истёк — не гасим.
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::None), static_cast<int>(backlight::decide(true, false, a)));
}

void test_decide_nothing_when_dark()
{
    // Экран уже погашен, бездействие продолжается: нечего делать.
    backlight::Activity a;
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::None), static_cast<int>(backlight::decide(false, true, a)));
    TEST_ASSERT_EQUAL(static_cast<int>(backlight::Action::None), static_cast<int>(backlight::decide(false, false, a)));
}

void test_brightness_off_is_zero()
{
    TEST_ASSERT_EQUAL_UINT8(0, backlight::brightness(128, false));
    TEST_ASSERT_EQUAL_UINT8(0, backlight::brightness(255, false));
}

void test_brightness_on_restores_configured()
{
    // Пробуждение обязано вернуть яркость из настроек, а не значение по
    // умолчанию: иначе после первого гашения экран стал бы тусклее.
    TEST_ASSERT_EQUAL_UINT8(128, backlight::brightness(128, true));
    TEST_ASSERT_EQUAL_UINT8(200, backlight::brightness(200, true));
}

void test_brightness_zero_becomes_visible()
{
    // Яркость 0 в config.json — это не «не гасить», а «всегда темно». Чтобы
    // устройство не оказалось невидимым из-за правки файла, включаем
    // минимальную видимую.
    TEST_ASSERT_EQUAL_UINT8(1, backlight::brightness(0, true));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_timeout_zero_disables);
    RUN_TEST(test_timeout_minutes_to_ms);

    RUN_TEST(test_decide_busy_keeps_light);
    RUN_TEST(test_decide_busy_wakes_if_late);
    RUN_TEST(test_decide_key_wakes);
    RUN_TEST(test_decide_touch_wakes);
    RUN_TEST(test_decide_activity_beats_timeout);
    RUN_TEST(test_decide_off_after_timeout);
    RUN_TEST(test_decide_nothing_when_dark);

    RUN_TEST(test_brightness_off_is_zero);
    RUN_TEST(test_brightness_on_restores_configured);
    RUN_TEST(test_brightness_zero_becomes_visible);
    return UNITY_END();
}
