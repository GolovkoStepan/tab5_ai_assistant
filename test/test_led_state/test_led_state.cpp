/*
 * Тесты цветов светодиодов клавиатуры A164.
 *
 * Правила («красный — ошибка», «оранжевый — батарея садится») — это логика,
 * которую на плате видно только глазами, а проверить надо все комбинации.
 * Поэтому цвета вынесены в чистую функцию, а здесь перебираются состояния.
 *
 * Отдельный блок — уровни воспринимаемой яркости. Это не косметика: владелец
 * спросил «они точно в пол силы оба горят?», потому что правый светодиод
 * светился заметно ярче левого при одинаковом регистре яркости (он один на
 * оба). Цвета подобраны под равную воспринимаемую яркость, и здесь это
 * закреплено числами.
 */

#include <unity.h>

#include "util/led_state.h"

void setUp(void) {}
void tearDown(void) {}

using led::Rgb;

// --- Левый светодиод: состояние модели ---

void test_idle_is_quiet_blue()
{
    led::Status s;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kQuietBlue);
}

void test_busy_is_brighter_than_idle()
{
    led::Status s;
    s.busy = true;
    // Тревоги не добавляем, но «запрос ушёл» должно отличаться от простоя.
    TEST_ASSERT_TRUE(led::compute(s).model != led::kQuietBlue);
    TEST_ASSERT_TRUE(led::compute(s).model == led::kWorkBlue);
}

void test_busy_is_perceptually_brighter_not_just_different()
{
    // Раньше здесь сравнивались только байты, и «ярче» означало «другое
    // число». Теперь важно, что уровень действительно выше: иначе плата
    // показывала бы «запрос ушёл» темнее, чем простой, и это выглядело бы
    // как ошибка отрисовки.
    TEST_ASSERT_TRUE(led::perceived(led::kWorkBlue) > led::perceived(led::kQuietBlue));
}

void test_streaming_is_work_green()
{
    led::Status s;
    s.busy = true;
    s.streaming = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kWorkGreen);
}

void test_error_is_red()
{
    led::Status s;
    s.error = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kAlarmRed);
}

void test_retry_wins_over_error()
{
    // Во время паузы перед повтором сообщение об ошибке уже записано, но
    // клиент как раз работает. Красный здесь сказал бы «всё плохо» неверно.
    led::Status s;
    s.error = true;
    s.retrying = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kAlarmOrange);
}

void test_retry_wins_over_streaming()
{
    // Оборвавшийся поток и пауза перед повтором: показываем повтор.
    led::Status s;
    s.busy = true;
    s.streaming = true;
    s.retrying = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kAlarmOrange);
}

void test_error_wins_over_streaming()
{
    led::Status s;
    s.busy = true;
    s.streaming = true;
    s.error = true;
    TEST_ASSERT_TRUE(led::compute(s).model == led::kAlarmRed);
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

void test_charging_is_work_blue()
{
    led::Status s;
    s.charging = true;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kWorkBlue);
}

void test_discharging_is_quiet_green()
{
    // Нормальный разряд — уровень «тихо», а не полная яркость: иначе правый
    // светодиод светится втрое ярче левого, хотя оба сообщают «всё в порядке».
    led::Status s;
    s.discharging = true;
    s.battery_level = 80;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kQuietGreen);
}

void test_discharging_and_idle_are_equally_bright()
{
    // Главный ответ на вопрос владельца: в покое оба светодиода светятся
    // одинаково. Левый — «жду вопроса», правый — «работаю от аккумулятора».
    led::Status idle;
    led::Status discharging;
    discharging.discharging = true;
    discharging.battery_level = 80;

    TEST_ASSERT_EQUAL_UINT32(led::perceived(led::compute(idle).model), led::perceived(led::compute(discharging).power));
}

void test_low_battery_is_orange()
{
    led::Status s;
    s.discharging = true;
    s.battery_level = led::kLowBatteryPercent - 1;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kAlarmOrange);
}

void test_battery_threshold_is_not_low()
{
    // Ровно на пороге — ещё нормально, оранжевый начинается ниже.
    led::Status s;
    s.discharging = true;
    s.battery_level = led::kLowBatteryPercent;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kQuietGreen);
}

void test_unknown_battery_level_is_not_low()
{
    // Процентов нет — не выдумываем разряд.
    led::Status s;
    s.discharging = true;
    s.battery_level = -1;
    TEST_ASSERT_TRUE(led::compute(s).power == led::kQuietGreen);
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
    TEST_ASSERT_TRUE(led::compute(s).power == led::kWorkBlue);
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

// --- Воспринимаемая яркость: уровни ---
//
// Уровни нужны, чтобы смысл индикации читался без сравнения байтов: тускло в
// покое, ярче во время работы, ярче всего при тревоге.

void test_quiet_levels_are_equal()
{
    // Внутри уровня цвета разные, а яркость одна — иначе один светодиод
    // выглядит погасшим рядом с другим.
    TEST_ASSERT_EQUAL_UINT32(led::perceived(led::kQuietBlue), led::perceived(led::kQuietGreen));
}

void test_work_levels_are_equal()
{
    TEST_ASSERT_EQUAL_UINT32(led::perceived(led::kWorkBlue), led::perceived(led::kWorkGreen));
}

void test_levels_are_ordered()
{
    TEST_ASSERT_TRUE(led::perceived(led::kQuietBlue) < led::perceived(led::kWorkBlue));
    TEST_ASSERT_TRUE(led::perceived(led::kWorkBlue) < led::perceived(led::kAlarmOrange));
}

void test_quiet_level_is_far_below_work_level()
{
    // Именно эта разница была дефектом: зелёный полной яркости против
    // тускло-синего. Сейчас «тихо» — это примерно треть «работы», и разница
    // остаётся заметной, но не выглядит как «один горит, другой нет».
    const uint32_t quiet = led::perceived(led::kQuietBlue);
    const uint32_t work = led::perceived(led::kWorkBlue);
    TEST_ASSERT_TRUE(work >= quiet * 3);
    TEST_ASSERT_TRUE(work <= quiet * 4);
}

void test_red_is_dim_by_metric_but_stays_red()
{
    // Метрика яркости не должна заставить перекрасить ошибку в оранжевый:
    // чистый красный физически не даёт больше ~54, и подмешивать зелёный ради
    // числа значит потерять сам сигнал «ошибка».
    TEST_ASSERT_EQUAL_UINT8(255, led::kAlarmRed.r);
    TEST_ASSERT_EQUAL_UINT8(0, led::kAlarmRed.g);
    TEST_ASSERT_EQUAL_UINT8(0, led::kAlarmRed.b);
}

void test_perceived_monotonic_with_channel()
{
    // Базовая проверка метрики: больше канал — не меньше яркость.
    TEST_ASSERT_TRUE(led::perceived(Rgb{0, 10, 0}) < led::perceived(Rgb{0, 20, 0}));
    TEST_ASSERT_TRUE(led::perceived(Rgb{0, 0, 0}) == 0);
}

void test_green_weighs_more_than_blue()
{
    // Причина исходного дефекта: при одинаковом значении канала зелёный
    // воспринимается намного ярче синего. Если это соотношение изменится,
    // подбор цветов надо перепроверить.
    TEST_ASSERT_TRUE(led::perceived(Rgb{0, 100, 0}) > led::perceived(Rgb{0, 0, 100}));
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
    RUN_TEST(test_idle_is_quiet_blue);
    RUN_TEST(test_busy_is_brighter_than_idle);
    RUN_TEST(test_busy_is_perceptually_brighter_not_just_different);
    RUN_TEST(test_streaming_is_work_green);
    RUN_TEST(test_error_is_red);
    RUN_TEST(test_retry_wins_over_error);
    RUN_TEST(test_retry_wins_over_streaming);
    RUN_TEST(test_error_wins_over_streaming);
    RUN_TEST(test_model_ignores_power_fields);

    RUN_TEST(test_charging_is_work_blue);
    RUN_TEST(test_discharging_is_quiet_green);
    RUN_TEST(test_discharging_and_idle_are_equally_bright);
    RUN_TEST(test_low_battery_is_orange);
    RUN_TEST(test_battery_threshold_is_not_low);
    RUN_TEST(test_unknown_battery_level_is_not_low);
    RUN_TEST(test_idle_and_no_battery_are_off);
    RUN_TEST(test_charging_wins_over_discharging);
    RUN_TEST(test_power_ignores_model_fields);

    RUN_TEST(test_quiet_levels_are_equal);
    RUN_TEST(test_work_levels_are_equal);
    RUN_TEST(test_levels_are_ordered);
    RUN_TEST(test_quiet_level_is_far_below_work_level);
    RUN_TEST(test_red_is_dim_by_metric_but_stays_red);
    RUN_TEST(test_perceived_monotonic_with_channel);
    RUN_TEST(test_green_weighs_more_than_blue);

    RUN_TEST(test_rgb_comparison);
    RUN_TEST(test_status_comparison);
    return UNITY_END();
}
