/*
 * Тесты параметров устройства: шаги яркости, ступени таймаута, порядок порогов
 * разряда и список таймзон.
 *
 * Почему это тесты, а не «посмотрим на плате». Ошибка здесь тихая: стрелка,
 * которая молча переставляет пороги, не ломает экран — она ломает будущее
 * предупреждение о разряде. А validate() откажет при сохранении уже после
 * того, как человек ушёл с экрана, и сохранения не будет вовсе.
 */

#include <unity.h>

#include <string.h>

#include "util/device_settings.h"
#include "util/settings.h"

void setUp(void) {}
void tearDown(void) {}

// --- Яркость ----------------------------------------------------------------

void test_brightness_steps_up()
{
    TEST_ASSERT_EQUAL_UINT8(144, devsettings::brightness_step(128, 1));
    TEST_ASSERT_EQUAL_UINT8(160, devsettings::brightness_step(128, 2));
}

void test_brightness_clamped_at_bottom()
{
    // Ниже 16 экран почти чёрный, а кнопки возврата яркости у платы нет.
    TEST_ASSERT_EQUAL_UINT8(16, devsettings::brightness_step(16, -1));
    TEST_ASSERT_EQUAL_UINT8(16, devsettings::brightness_step(128, -100));
}

void test_brightness_clamped_at_top()
{
    TEST_ASSERT_EQUAL_UINT8(255, devsettings::brightness_step(250, 5));
    TEST_ASSERT_EQUAL_UINT8(255, devsettings::brightness_step(128, 100));
}

void test_brightness_no_underflow_on_uint8()
{
    // Считай мы в uint8_t, шаг назад от 16 дал бы 240 — «убавил яркость» стало
    // бы «прибавил».
    const uint8_t v = devsettings::brightness_step(16, -1);
    TEST_ASSERT_TRUE(v <= 16);
}

// --- Таймаут ----------------------------------------------------------------

void test_timeout_walks_steps()
{
    TEST_ASSERT_EQUAL_UINT8(1, devsettings::timeout_step(0, 1));
    TEST_ASSERT_EQUAL_UINT8(2, devsettings::timeout_step(1, 1));
    TEST_ASSERT_EQUAL_UINT8(5, devsettings::timeout_step(2, 1));
    TEST_ASSERT_EQUAL_UINT8(10, devsettings::timeout_step(5, 1));
    TEST_ASSERT_EQUAL_UINT8(60, devsettings::timeout_step(30, 1));
}

void test_timeout_walks_back()
{
    TEST_ASSERT_EQUAL_UINT8(30, devsettings::timeout_step(60, -1));
    TEST_ASSERT_EQUAL_UINT8(0, devsettings::timeout_step(1, -1));
    // У края — не уходим в отрицательное.
    TEST_ASSERT_EQUAL_UINT8(0, devsettings::timeout_step(0, -1));
}

void test_timeout_from_file_value_rounds_down()
{
    // В config.json могло остаться 7 (правили руками). Шаг вперёд должен вести
    // на 10, а не перескочить через ступень.
    TEST_ASSERT_EQUAL_UINT8(10, devsettings::timeout_step(7, 1));
    // Шаг назад — на 5: 7 не ступень, и «назад» означает «ближайшая меньше».
    TEST_ASSERT_EQUAL_UINT8(5, devsettings::timeout_step(7, -1));
}

void test_timeout_above_list_goes_to_max()
{
    // 60 — последняя ступень; из 60 дальше некуда.
    TEST_ASSERT_EQUAL_UINT8(60, devsettings::timeout_step(60, 1));
}

// --- Пороги разряда ---------------------------------------------------------

void test_battery_steps_change_value()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    TEST_ASSERT_EQUAL_UINT8(20, cfg.battery_warn_percent);

    TEST_ASSERT_TRUE(devsettings::battery_step(cfg, devsettings::Battery::Warn, 1));
    TEST_ASSERT_EQUAL_UINT8(21, cfg.battery_warn_percent);
    TEST_ASSERT_TRUE(devsettings::battery_step(cfg, devsettings::Battery::Warn, -1));
    TEST_ASSERT_EQUAL_UINT8(20, cfg.battery_warn_percent);
}

void test_battery_multi_step_applies_every_step()
{
    // `dev warn+5` — это пять шагов, а не один. На живой плате команда тихо
    // сработала как «+1»: значение 21 вместо 25, и приёмка записала бы неверное.
    settings::Config cfg;
    settings::make_default_config(cfg);

    TEST_ASSERT_TRUE(devsettings::battery_step(cfg, devsettings::Battery::Warn, 5));
    TEST_ASSERT_EQUAL_UINT8(25, cfg.battery_warn_percent);

    TEST_ASSERT_TRUE(devsettings::battery_step(cfg, devsettings::Battery::Shutdown, 2));
    TEST_ASSERT_EQUAL_UINT8(7, cfg.battery_shutdown_percent);

    // Критический упирается в выключение (7): ниже него он не пойдёт, иначе
    // «критично» наступило бы позже выключения.
    TEST_ASSERT_TRUE(devsettings::battery_step(cfg, devsettings::Battery::Critical, -3));
    TEST_ASSERT_EQUAL_UINT8(8, cfg.battery_critical_percent);
    TEST_ASSERT_TRUE(cfg.battery_critical_percent > cfg.battery_shutdown_percent);
}

void test_battery_warn_cannot_cross_critical()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    // warn 20, critical 10: убавляем warn, пока он не упрётся в critical + 1.
    for (int i = 0; i < 20; ++i)
        devsettings::battery_step(cfg, devsettings::Battery::Warn, -1);

    TEST_ASSERT_EQUAL_UINT8(11, cfg.battery_warn_percent);
    TEST_ASSERT_TRUE(cfg.battery_warn_percent > cfg.battery_critical_percent);
    TEST_ASSERT_TRUE(cfg.battery_critical_percent > cfg.battery_shutdown_percent);
}

void test_battery_shutdown_cannot_cross_critical()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    for (int i = 0; i < 20; ++i)
        devsettings::battery_step(cfg, devsettings::Battery::Shutdown, 1);

    TEST_ASSERT_EQUAL_UINT8(9, cfg.battery_shutdown_percent);
    TEST_ASSERT_TRUE(cfg.battery_critical_percent > cfg.battery_shutdown_percent);
}

void test_battery_order_survives_any_sequence()
{
    // Бессистемные шаги по всем трём порогам не должны ломать порядок: именно
    // это проверяет settings::validate при сохранении.
    settings::Config cfg;
    settings::make_default_config(cfg);

    const int deltas[] = {1, 1, 1, -1, -1, 3, -5, 2, -1, 1, -2, 4};
    const devsettings::Battery which[] = {
        devsettings::Battery::Warn, devsettings::Battery::Critical, devsettings::Battery::Shutdown};
    int k = 0;
    for (int round = 0; round < 40; ++round) {
        for (size_t w = 0; w < 3; ++w) {
            devsettings::battery_step(cfg, which[w], deltas[k % 12]);
            ++k;
        }
        TEST_ASSERT_TRUE(cfg.battery_warn_percent > cfg.battery_critical_percent);
        TEST_ASSERT_TRUE(cfg.battery_critical_percent > cfg.battery_shutdown_percent);

        char err[128];
        TEST_ASSERT_TRUE_MESSAGE(settings::validate(cfg, err, sizeof(err)), err);
    }
}

void test_battery_names_exist()
{
    TEST_ASSERT_NOT_NULL(devsettings::battery_name(devsettings::Battery::Warn));
    TEST_ASSERT_NOT_NULL(devsettings::battery_name(devsettings::Battery::Critical));
    TEST_ASSERT_NOT_NULL(devsettings::battery_name(devsettings::Battery::Shutdown));
    TEST_ASSERT_TRUE(strlen(devsettings::battery_name(devsettings::Battery::Warn)) > 0);
}

// --- Таймзоны ---------------------------------------------------------------

void test_zone_list_has_moscow_and_utc()
{
    const int moscow = devsettings::zone_index("MSK-3");
    TEST_ASSERT_TRUE(moscow >= 0);
    TEST_ASSERT_EQUAL_STRING("MSK-3", devsettings::zone_at(static_cast<size_t>(moscow))->posix);

    const int utc = devsettings::zone_index("UTC-0");
    TEST_ASSERT_TRUE(utc >= 0);
}

void test_zone_unknown_is_minus_one()
{
    // IANA-имя newlib на ESP32 не понимает: TZ станет пустой, и метки уедут в
    // UTC молча. Поэтому такой строки в списке быть не должно.
    TEST_ASSERT_EQUAL_INT(-1, devsettings::zone_index("Asia/Yekaterinburg"));
    TEST_ASSERT_EQUAL_INT(-1, devsettings::zone_index(""));
    TEST_ASSERT_EQUAL_INT(-1, devsettings::zone_index(nullptr));
}

void test_zone_step_wraps_around()
{
    const devsettings::Zone& first = *devsettings::zone_at(0);
    const devsettings::Zone& last = *devsettings::zone_at(devsettings::zone_count() - 1);

    // Шаг назад от первой зоны — последняя, а не отказ.
    TEST_ASSERT_EQUAL_STRING(last.posix, devsettings::zone_step(first.posix, -1).posix);
    // Шаг вперёд от последней — первая.
    TEST_ASSERT_EQUAL_STRING(first.posix, devsettings::zone_step(last.posix, 1).posix);
}

void test_zone_step_from_unknown_starts_at_first()
{
    // В config.json могло лежать что-то не из списка: экран не должен пустеть.
    TEST_ASSERT_EQUAL_STRING(devsettings::zone_at(0)->posix, devsettings::zone_step("Europe/Moscow", 0).posix);
    TEST_ASSERT_EQUAL_STRING(devsettings::zone_at(1)->posix, devsettings::zone_step("Europe/Moscow", 1).posix);
}

// --- Разбор шага для Serial (команда `dev`) ---------------------------------

void test_parse_step_all_params()
{
    using P = devsettings::Param;
    const auto parse = [](const char* s) { return devsettings::parse_step(s, strlen(s)); };

    TEST_ASSERT_EQUAL(static_cast<int>(P::Brightness), static_cast<int>(parse("bright+1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Timeout), static_cast<int>(parse("timeout-1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Layout), static_cast<int>(parse("layout").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Warn), static_cast<int>(parse("warn+1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Critical), static_cast<int>(parse("crit-1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Shutdown), static_cast<int>(parse("shut+1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Timezone), static_cast<int>(parse("tz+1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(P::Save), static_cast<int>(parse("save").param));
}

void test_parse_step_direction()
{
    const auto parse = [](const char* s) { return devsettings::parse_step(s, strlen(s)); };

    TEST_ASSERT_EQUAL_INT(1, parse("bright+1").delta);
    TEST_ASSERT_EQUAL_INT(-1, parse("bright-1").delta);
    // Без знака — шаг вперёд.
    TEST_ASSERT_EQUAL_INT(1, parse("bright").delta);
    // Число после знака — сколько шагов.
    TEST_ASSERT_EQUAL_INT(3, parse("timeout+3").delta);
    TEST_ASSERT_EQUAL_INT(-2, parse("warn-2").delta);
}

void test_parse_step_rejects_garbage()
{
    const auto parse = [](const char* s) { return devsettings::parse_step(s, strlen(s)); };

    TEST_ASSERT_EQUAL(static_cast<int>(devsettings::Param::None), static_cast<int>(parse("").param));
    TEST_ASSERT_EQUAL(static_cast<int>(devsettings::Param::None), static_cast<int>(parse("x+1").param));
    TEST_ASSERT_EQUAL(static_cast<int>(devsettings::Param::None), static_cast<int>(parse("bright+").param));
    TEST_ASSERT_EQUAL(static_cast<int>(devsettings::Param::None), static_cast<int>(parse("bright+a").param));
    TEST_ASSERT_EQUAL(static_cast<int>(devsettings::Param::None), static_cast<int>(parse("brightness").param));
    TEST_ASSERT_EQUAL(
        static_cast<int>(devsettings::Param::None), static_cast<int>(devsettings::parse_step(nullptr, 4).param));
    TEST_ASSERT_EQUAL(
        static_cast<int>(devsettings::Param::None), static_cast<int>(devsettings::parse_step("bright", 0).param));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_brightness_steps_up);
    RUN_TEST(test_brightness_clamped_at_bottom);
    RUN_TEST(test_brightness_clamped_at_top);
    RUN_TEST(test_brightness_no_underflow_on_uint8);

    RUN_TEST(test_timeout_walks_steps);
    RUN_TEST(test_timeout_walks_back);
    RUN_TEST(test_timeout_from_file_value_rounds_down);
    RUN_TEST(test_timeout_above_list_goes_to_max);

    RUN_TEST(test_battery_steps_change_value);
    RUN_TEST(test_battery_multi_step_applies_every_step);
    RUN_TEST(test_battery_warn_cannot_cross_critical);
    RUN_TEST(test_battery_shutdown_cannot_cross_critical);
    RUN_TEST(test_battery_order_survives_any_sequence);
    RUN_TEST(test_battery_names_exist);

    RUN_TEST(test_zone_list_has_moscow_and_utc);
    RUN_TEST(test_zone_unknown_is_minus_one);
    RUN_TEST(test_zone_step_wraps_around);
    RUN_TEST(test_zone_step_from_unknown_starts_at_first);

    RUN_TEST(test_parse_step_all_params);
    RUN_TEST(test_parse_step_direction);
    RUN_TEST(test_parse_step_rejects_garbage);

    return UNITY_END();
}
