/*
 * Тесты цветов светодиодов клавиатуры A164.
 *
 * Правила («красный — ошибка», «оранжевый — батарея садится») — это логика,
 * которую на плате видно только глазами, а проверить надо все комбинации.
 * Поэтому цвета вынесены в чистую функцию, а здесь перебираются состояния.
 */

#include <unity.h>

#include "util/led_state.h"

void setUp(void) {}
void tearDown(void) {}

using led::Rgb;

// --- Левый светодиод: состояние модели ---

void test_idle_is_dim_blue()
{
    led::Status s;
    TEST_ASSERT_EQUAL_UINT8(led::kDimBlue.r, led::compute(s).model.r);
    TEST_ASSERT_EQUAL_UINT8(led::kDimBlue.g, led::compute(s).model.g);
    TEST_ASSERT_EQUAL_UINT8(led::kDimBlue.b, led::compute(s).model.b);
}

void test_busy_is_brighter_than_idle()
{
    led::Status s;
    s.busy = true;
    // Тревоги не добавляем, но «запрос ушёл» должно отличаться от простоя.
    TEST_ASSERT_TRUE(led::compute(s).model != led::kDimBlue);
    TEST_ASSERT_EQUAL_UINT8(led::kBlue.b, led::compute(s).model.b);
}

void test_streaming_is_green()
{
    led::Status s;
    s.busy = true;
    s.streaming = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kGreen);
}

void test_error_is_red()
{
    led::Status s;
    s.error = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kRed);
}

void test_retry_wins_over_error()
{
    // Во время паузы перед повтором сообщение об ошибке уже записано, но
    // клиент как раз работает. Красный здесь сказал бы «всё плохо» неверно.
    led::Status s;
    s.error = true;
    s.retrying = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kOrange);
}

void test_retry_wins_over_streaming()
{
    // Оборвавшийся поток и пауза перед повтором: показываем повтор.
    led::Status s;
    s.busy = true;
    s.streaming = true;
    s.retrying = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kOrange);
}

void test_error_wins_over_streaming()
{
    led::Status s;
    s.busy = true;
    s.streaming = true;
    s.error = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kRed);
}

void test_model_ignores_power_fields()
{
    // Правая часть не должна влиять на левый светодиод: иначе разряд батареи
    // перекрашивал бы индикатор ответа модели.
    led::Status a;
    led::Status b;
    b.charging = true;
    b.battery_level = 5;
    TEST_ASSERT_TRUE(led::compute(a).model == led::compute(b).model);
}

// --- Правый светодиод: питание ---

void test_charging_is_blue()
{
    led::Status s;
    s.charging = true;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kBlue);
}

void test_discharging_is_green()
{
    led::Status s;
    s.discharging = true;
    s.battery_level = 80;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kGreen);
}

void test_low_battery_is_orange()
{
    led::Status s;
    s.discharging = true;
    s.battery_level = led::kLowBatteryPercent - 1;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kOrange);
}

void test_battery_threshold_is_not_low()
{
    // Ровно на пороге — ещё нормально, оранжевый начинается ниже.
    led::Status s;
    s.discharging = true;
    s.battery_level = led::kLowBatteryPercent;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kGreen);
}

void test_unknown_battery_level_is_not_low()
{
    // Процентов нет — не выдумываем разряд.
    led::Status s;
    s.discharging = true;
    s.battery_level = -1;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kGreen);
}

void test_idle_and_no_battery_are_off()
{
    // На Tab5 «заряжен» и «аккумулятора нет» неразличимы (Р-016). Гасим:
    // честное «ничего не могу сказать» лучше зелёного на пустом отсеке.
    led::Status s;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kOff);

    s.battery_level = 100;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kOff);
}

void test_charging_wins_over_discharging()
{
    // Одновременно оба не бывает, но если данные противоречат — верим заряду:
    // ошибиться в сторону «кабель воткнут» безопаснее.
    led::Status s;
    s.charging = true;
    s.discharging = true;
    s.battery_level = 5;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kBlue);
}

void test_power_ignores_model_fields()
{
    led::Status a;
    led::Status b;
    b.busy = true;
    b.error = true;
    b.retrying = true;
    TEST_ASSERT_TRUE(led::compute(a).power == led::compute(b).power);
}

// --- Вспомогательное ---

void test_rgb_comparison()
{
    const Rgb a{1, 2, 3};
    const Rgb b{1, 2, 3};
    const Rgb c{3, 2, 1};
    TEST_ASSERT_TRUE(a == b);
    TEST_ASSERT_FALSE(a == c);
    TEST_ASSERT_TRUE(a != c);
}

void test_status_comparison()
{
    // По этому сравнению решается, писать ли цвета в клавиатуру: без него
    // I2C дёргался бы каждый кадр.
    led::Status a;
    led::Status b;
    TEST_ASSERT_TRUE(a == b);

    b.retrying = true;
    TEST_ASSERT_TRUE(a != b);

    b.retrying = false;
    b.battery_level = 50;
    TEST_ASSERT_TRUE(a != b);

    b.battery_level = -1;
    TEST_ASSERT_TRUE(a == b);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_is_dim_blue);
    RUN_TEST(test_busy_is_brighter_than_idle);
    RUN_TEST(test_streaming_is_green);
    RUN_TEST(test_error_is_red);
    RUN_TEST(test_retry_wins_over_error);
    RUN_TEST(test_retry_wins_over_streaming);
    RUN_TEST(test_error_wins_over_streaming);
    RUN_TEST(test_model_ignores_power_fields);

    RUN_TEST(test_charging_is_blue);
    RUN_TEST(test_discharging_is_green);
    RUN_TEST(test_low_battery_is_orange);
    RUN_TEST(test_battery_threshold_is_not_low);
    RUN_TEST(test_unknown_battery_level_is_not_low);
    RUN_TEST(test_idle_and_no_battery_are_off);
    RUN_TEST(test_charging_wins_over_discharging);
    RUN_TEST(test_power_ignores_model_fields);

    RUN_TEST(test_rgb_comparison);
    RUN_TEST(test_status_comparison);
    return UNITY_END();
}
