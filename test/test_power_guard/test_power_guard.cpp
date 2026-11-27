/*
 * Тесты правил предупреждения о разряде.
 *
 * Здесь проверяется то, что на плате видно только глазами и легко ломается
 * незаметно:
 *   - предупреждение появляется ровно при переходе через порог и не мигает;
 *   - при заряде и при неизвестном проценте его нет вообще;
 *   - перепутанные пороги в config.json не отключают защиту.
 */

#include <unity.h>

#include <string.h>

#include "util/power_guard.h"

void setUp(void) {}
void tearDown(void) {}

// --- Уровень ---

void test_level_ok_above_warn()
{
    const power::Thresholds t;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(true, 80, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(true, 21, t)));
}

void test_level_warn_at_threshold()
{
    // Порог 20 означает «на 20 % уже пора»: при ровно 20 предупреждение
    // должно быть, иначе человек увидит его на шаг позже обещанного.
    const power::Thresholds t;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Warn), static_cast<int>(power::level(true, 20, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Warn), static_cast<int>(power::level(true, 15, t)));
}

void test_level_critical_and_shutdown()
{
    const power::Thresholds t;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Critical), static_cast<int>(power::level(true, 10, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Critical), static_cast<int>(power::level(true, 6, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown), static_cast<int>(power::level(true, 5, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown), static_cast<int>(power::level(true, 0, t)));
}

void test_level_unknown_percent_is_ok()
{
    // На пустом отсеке getBatteryLevel() отдаёт 100 %, а сразу после снятия
    // батареи — мусор. Предупреждать по неизвестному проценту значит врать.
    const power::Thresholds t;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(false, 3, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(false, -1, t)));
}

void test_level_out_of_range_is_ok()
{
    const power::Thresholds t;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(true, -1, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(true, 101, t)));
}

void test_level_survives_broken_thresholds()
{
    // Пороги в config.json перепутаны (warn ниже critical). settings::validate
    // такое не пропускает, но модуль чистый — он обязан быть устойчив сам,
    // иначе при правке файла руками защита от разряда молча исчезнет.
    power::Thresholds t;
    t.warn = 5;
    t.critical = 10;
    t.shutdown = 20;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown), static_cast<int>(power::level(true, 4, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Critical), static_cast<int>(power::level(true, 9, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Warn), static_cast<int>(power::level(true, 19, t)));
}

void test_level_custom_thresholds()
{
    power::Thresholds t;
    t.warn = 50;
    t.critical = 30;
    t.shutdown = 10;
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok), static_cast<int>(power::level(true, 60, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Warn), static_cast<int>(power::level(true, 45, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Critical), static_cast<int>(power::level(true, 25, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown), static_cast<int>(power::level(true, 9, t)));
}

// --- Что показывать ---

void test_next_shown_appears_once()
{
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Warn),
        static_cast<int>(power::next_shown(power::Level::Ok, power::Level::Warn)));
    // Второй раз с тем же уровнем — ничего не меняется: баннер не мигает и
    // экран не перерисовывается каждую секунду.
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Warn),
        static_cast<int>(power::next_shown(power::Level::Warn, power::Level::Warn)));
}

void test_next_shown_clears_on_ok()
{
    // Кабель воткнули: «садится» при зарядке — прямая ложь.
    TEST_ASSERT_EQUAL(
        static_cast<int>(power::Level::Ok), static_cast<int>(power::next_shown(power::Level::Warn, power::Level::Ok)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Ok),
        static_cast<int>(power::next_shown(power::Level::Critical, power::Level::Ok)));
}

void test_next_shown_never_downgrades()
{
    // Заряд подрос с 4 % до 15 %: показанное «выключится» не откатывается к
    // «почти разряжена». Откат выглядел бы как улучшение, которого не было:
    // устройство всё равно выключится.
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown),
        static_cast<int>(power::next_shown(power::Level::Shutdown, power::Level::Warn)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Critical),
        static_cast<int>(power::next_shown(power::Level::Critical, power::Level::Warn)));
}

void test_next_shown_escalates()
{
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Critical),
        static_cast<int>(power::next_shown(power::Level::Warn, power::Level::Critical)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown),
        static_cast<int>(power::next_shown(power::Level::Critical, power::Level::Shutdown)));
}

void test_next_shown_from_nothing()
{
    // Первый вызов: показывали «ничего».
    TEST_ASSERT_EQUAL(
        static_cast<int>(power::Level::Ok), static_cast<int>(power::next_shown(power::Level::Ok, power::Level::Ok)));
    TEST_ASSERT_EQUAL(static_cast<int>(power::Level::Shutdown),
        static_cast<int>(power::next_shown(power::Level::Ok, power::Level::Shutdown)));
}

// --- Текст ---

void test_message_ok_is_empty()
{
    TEST_ASSERT_EQUAL_STRING("", power::message(power::Level::Ok));
}

void test_message_mentions_action()
{
    // Текст говорит, что делать, а не «батарея 12 %»: процент и так в трее,
    // а на заряде он недостоверен.
    TEST_ASSERT_NOT_NULL(strstr(power::message(power::Level::Warn), "зарядк"));
    TEST_ASSERT_NOT_NULL(strstr(power::message(power::Level::Critical), "зарядк"));
    TEST_ASSERT_NOT_NULL(strstr(power::message(power::Level::Shutdown), "выключ"));
}

void test_message_has_no_percent_sign()
{
    // Подпись с числом пришлось бы собирать из процента, а он на заряде
    // врёт. Тексты статичны именно поэтому.
    TEST_ASSERT_NULL(strstr(power::message(power::Level::Warn), "%"));
    TEST_ASSERT_NULL(strstr(power::message(power::Level::Critical), "%"));
    TEST_ASSERT_NULL(strstr(power::message(power::Level::Shutdown), "%"));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_level_ok_above_warn);
    RUN_TEST(test_level_warn_at_threshold);
    RUN_TEST(test_level_critical_and_shutdown);
    RUN_TEST(test_level_unknown_percent_is_ok);
    RUN_TEST(test_level_out_of_range_is_ok);
    RUN_TEST(test_level_survives_broken_thresholds);
    RUN_TEST(test_level_custom_thresholds);

    RUN_TEST(test_next_shown_appears_once);
    RUN_TEST(test_next_shown_clears_on_ok);
    RUN_TEST(test_next_shown_never_downgrades);
    RUN_TEST(test_next_shown_escalates);
    RUN_TEST(test_next_shown_from_nothing);

    RUN_TEST(test_message_ok_is_empty);
    RUN_TEST(test_message_mentions_action);
    RUN_TEST(test_message_has_no_percent_sign);
    return UNITY_END();
}
