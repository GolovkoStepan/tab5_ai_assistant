/*
 * Тесты правил системного трея.
 *
 * Здесь проверяются ровно те решения, которые на плате видны только глазами и
 * которые легко испортить незаметно:
 *   - процент заряда при подключённом кабеле (зарядник поднимает напряжение до
 *     потолка формулы getBatteryLevel, и там оказывается 100 %);
 *   - «нет Wi-Fi» против «есть»;
 *   - ноль палочек при RSSI == 0, который драйвер отдаёт без связи.
 */

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "util/tray_state.h"

void setUp(void) {}
void tearDown(void) {}

// --- Wi-Fi: подпись ---

void test_wifi_label_empty_when_connected()
{
    // Значок с заполненными палочками сам отвечает, что связь есть. Адрес
    // здесь дублировал бы экран Wi-Fi и отнимал место у заголовка.
    char out[8] = "x";
    tray::wifi_label(true, false, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_wifi_label_connecting()
{
    char out[8];
    tray::wifi_label(false, true, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("…", out);
}

void test_wifi_label_disconnected()
{
    char out[8];
    tray::wifi_label(false, false, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("нет", out);
}

void test_wifi_label_connecting_wins()
{
    // Подключаемся — значок ещё не заполнен, поэтому подпись нужна.
    char out[8];
    tray::wifi_label(false, true, out, sizeof(out));
    TEST_ASSERT_TRUE(out[0] != '\0');
}

// --- Wi-Fi: уровень ---

void test_wifi_level_zero_when_disconnected()
{
    // RSSI 0 у драйвера значит «нет связи», а по таблице порогов это был бы
    // самый сильный сигнал.
    TEST_ASSERT_EQUAL_UINT8(0, tray::wifi_level(false, 0));
    TEST_ASSERT_EQUAL_UINT8(0, tray::wifi_level(false, -40));
}

void test_wifi_level_matches_thresholds_when_connected()
{
    TEST_ASSERT_EQUAL_UINT8(4, tray::wifi_level(true, -40));
    TEST_ASSERT_EQUAL_UINT8(3, tray::wifi_level(true, -60));
    TEST_ASSERT_EQUAL_UINT8(2, tray::wifi_level(true, -70));
    TEST_ASSERT_EQUAL_UINT8(1, tray::wifi_level(true, -85));
    TEST_ASSERT_EQUAL_UINT8(0, tray::wifi_level(true, -95));
}

// --- Батарея: подпись ---

void test_battery_label_charging_says_zaryad()
{
    // Главный дефект, найденный на живой плате: при подключении кабеля
    // getBatteryLevel() сразу отдаёт 100 %, потому что считает заряд по
    // напряжению на ячейку, а зарядник держит его на верхней границе формулы.
    // Показывать это число — врать, а направление тока известно достоверно.
    char out[20];
    tray::battery_label(tray::Charge::Charging, false, 100, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("заряд", out);
    TEST_ASSERT_NULL(strstr(out, "100"));
}

void test_battery_label_charging_ignores_known_percent()
{
    // Даже если процент формально «достоверен», при заряде он неверен.
    char out[20];
    tray::battery_label(tray::Charge::Charging, true, 87, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("заряд", out);
}

void test_battery_label_percent_when_discharging()
{
    char out[20];
    tray::battery_label(tray::Charge::Discharging, true, 87, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("87 %", out);
}

void test_battery_label_dash_when_nothing_known()
{
    // Напряжение не выводим намеренно: при пустом отсеке INA226 видит холостой
    // ход зарядного узла, а тот на живой плате скачет между 8.37 и 4.35 В —
    // подпись мигала бы. Прочерк честнее числа, которое ничего не сообщает.
    char out[20];
    tray::battery_label(tray::Charge::Unknown, false, -1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("—", out);

    // И от процента, которому нельзя верить, он тоже не зависит.
    tray::battery_label(tray::Charge::Unknown, false, 100, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("—", out);
}

void test_battery_label_no_fake_hundred_without_battery()
{
    // На пустом отсеке та же формула тоже даёт 100 %: процентов быть не должно.
    char out[20];
    tray::battery_label(tray::Charge::Unknown, false, 100, out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "100"));
}

void test_battery_label_clamps_out_of_range()
{
    char out[20];
    tray::battery_label(tray::Charge::Discharging, true, 140, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("100 %", out);

    tray::battery_label(tray::Charge::Discharging, true, -5, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0 %", out);
}

void test_battery_label_charged_after_charging()
{
    // «Заряжено» — вывод из истории, а не измерение: процент при полном заряде
    // врёт так же, как и во время него.
    char out[20];
    tray::battery_label(tray::Charge::Charged, false, 100, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("заряжено", out);
    TEST_ASSERT_NULL(strstr(out, "100"));
}

// --- Батарея: вывод состояния заряда из истории ---

void test_next_charge_reports_charging()
{
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Charging),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Unknown, true, false, false)));
}

void test_next_charge_reports_discharging()
{
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Discharging),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Charging, false, true, false)));
}

void test_next_charge_becomes_charged_after_charging()
{
    // Главная причина этой логики: на Tab5 «тока нет» означает и «заряжен», и
    // «аккумулятора нет». Различить можно только памятью о том, что заряд шёл.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Charged),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Charging, false, false, false)));
}

void test_next_charge_stays_unknown_without_history()
{
    // Заряда не видели — «заряжено» объявить нельзя: аккумулятора может не быть.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Unknown),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Unknown, false, false, false)));
}

void test_next_charge_charged_is_sticky()
{
    // Уже заряжено и по-прежнему нет тока — состояние не откатывается в
    // «неизвестно», иначе значок мигал бы между состояниями.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Charged),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Charged, false, false, false)));
}

void test_next_charge_discharging_from_charged()
{
    // Выдернули кабель — работает от аккумулятора, проценты снова достоверны.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Discharging),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Charged, false, true, false)));
}

void test_next_charge_absent_battery_is_unknown_not_charged()
{
    // Дефект с платы: аккумулятор вынули при подключённом кабеле. Заряд перед
    // этим шёл, ток упал до нуля — и история объявляла «заряжено» про батарею,
    // которой уже нет. Подпись «заряжено» рядом с жёлтым значком (0 %) и была
    // тем, что увидел владелец.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Unknown),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Charging, false, false, true)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(tray::Charge::Unknown),
        static_cast<uint8_t>(tray::next_charge(tray::Charge::Charged, false, false, true)));
}

// --- Наличие аккумулятора ---

void test_presence_current_proves_battery()
{
    // Ток течёт только через аккумулятор — это прямое доказательство.
    tray::Presence p;
    p.absent = true;  // даже если раньше решили, что его нет
    TEST_ASSERT_FALSE(tray::next_presence(p, true, 8000).absent);
}

void test_presence_low_voltage_proves_no_battery()
{
    // 4.34 В — холостой ход зарядного узла IP2326 в пустом отсеке (замер с
    // платы). Живая 2S-сборка так низко не опускается.
    tray::Presence p;
    const tray::Presence next = tray::next_presence(p, false, 4341);
    TEST_ASSERT_TRUE(next.absent);
}

void test_presence_plausible_voltage_proves_nothing()
{
    // 8.37 В — правдоподобное напряжение заряженной сборки. Но в пустом отсеке
    // зарядный узел «дышит» и доходит до него, поэтому это не доказательство
    // наличия — только отсутствие доказательства отсутствия.
    tray::Presence p;
    TEST_ASSERT_FALSE(tray::next_presence(p, false, 8371).absent);
}

void test_presence_absence_is_latched()
{
    // Главное в этом решении: подозрение защёлкивается. Без защёлки значок
    // мигал бы между «заряжено» и «нет данных» каждые 5–15 секунд вместе с
    // «дыханием» зарядного узла.
    tray::Presence p;
    p = tray::next_presence(p, false, 4341);
    TEST_ASSERT_TRUE(p.absent);

    // Высокая фаза того же «дыхания» не должна снимать защёлку.
    p = tray::next_presence(p, false, 8371);
    TEST_ASSERT_TRUE(p.absent);
}

void test_presence_zero_reading_proves_nothing()
{
    // Неудачное чтение INA226 — не доказательство отсутствия.
    tray::Presence p;
    TEST_ASSERT_FALSE(tray::next_presence(p, false, 0).absent);
    TEST_ASSERT_FALSE(tray::next_presence(p, false, -1).absent);
}

// --- Батарея: порог и заполнение ---

void test_battery_low_below_threshold()
{
    TEST_ASSERT_TRUE(tray::battery_low(true, 10));
    TEST_ASSERT_TRUE(tray::battery_low(true, 19));
}

void test_battery_not_low_at_threshold()
{
    // Порог — «ниже», а не «ниже или равно»: на 20 % тревоги ещё нет.
    TEST_ASSERT_FALSE(tray::battery_low(true, 20));
    TEST_ASSERT_FALSE(tray::battery_low(true, 100));
}

void test_battery_unknown_is_not_low()
{
    // Неизвестный заряд не повод пугать: неизвестно — не значит «садится».
    TEST_ASSERT_FALSE(tray::battery_low(false, 5));
    TEST_ASSERT_FALSE(tray::battery_low(true, -1));
}

void test_battery_fill_unknown_is_minus_one()
{
    TEST_ASSERT_EQUAL_INT(-1, tray::battery_fill(tray::Charge::Unknown, false, 80));
    TEST_ASSERT_EQUAL_INT(-1, tray::battery_fill(tray::Charge::Unknown, false, 0));
}

void test_battery_fill_clamps()
{
    TEST_ASSERT_EQUAL_INT(0, tray::battery_fill(tray::Charge::Discharging, true, -3));
    TEST_ASSERT_EQUAL_INT(100, tray::battery_fill(tray::Charge::Discharging, true, 130));
    TEST_ASSERT_EQUAL_INT(87, tray::battery_fill(tray::Charge::Discharging, true, 87));
}

void test_battery_fill_charged_is_full()
{
    // «Заряжено» рядом с пустым корпусом читалось бы как «ноль»: уровень в
    // этом состоянии известен по определению, даже если процента нет.
    TEST_ASSERT_EQUAL_INT(100, tray::battery_fill(tray::Charge::Charged, false, -1));
    TEST_ASSERT_EQUAL_INT(100, tray::battery_fill(tray::Charge::Charged, false, 100));
}

// --- Сравнение снимков ---

void test_info_equality_ignores_nothing()
{
    tray::Info a;
    a.wifi_connected = true;
    snprintf(a.wifi_text, sizeof(a.wifi_text), "%s", "нет");
    a.battery_known = true;
    a.battery_percent = 55;

    tray::Info b = a;
    TEST_ASSERT_TRUE(a == b);

    b.battery_percent = 54;
    TEST_ASSERT_TRUE(a != b);

    b = a;
    snprintf(b.wifi_text, sizeof(b.wifi_text), "%s", "…");
    TEST_ASSERT_TRUE(a != b);
}

void test_info_equality_zero_initialised()
{
    // Снимок собирается как `tray::Info{}` перед заполнением, поэтому
    // сравнение пустых снимков должно быть истинным: иначе экран
    // перерисовывался бы каждый кадр.
    const tray::Info a;
    const tray::Info b;
    TEST_ASSERT_TRUE(a == b);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_wifi_label_empty_when_connected);
    RUN_TEST(test_wifi_label_connecting);
    RUN_TEST(test_wifi_label_disconnected);
    RUN_TEST(test_wifi_label_connecting_wins);

    RUN_TEST(test_wifi_level_zero_when_disconnected);
    RUN_TEST(test_wifi_level_matches_thresholds_when_connected);

    RUN_TEST(test_battery_label_charging_says_zaryad);
    RUN_TEST(test_battery_label_charging_ignores_known_percent);
    RUN_TEST(test_battery_label_percent_when_discharging);
    RUN_TEST(test_battery_label_dash_when_nothing_known);
    RUN_TEST(test_battery_label_no_fake_hundred_without_battery);
    RUN_TEST(test_battery_label_clamps_out_of_range);
    RUN_TEST(test_battery_label_charged_after_charging);

    RUN_TEST(test_next_charge_reports_charging);
    RUN_TEST(test_next_charge_reports_discharging);
    RUN_TEST(test_next_charge_becomes_charged_after_charging);
    RUN_TEST(test_next_charge_stays_unknown_without_history);
    RUN_TEST(test_next_charge_charged_is_sticky);
    RUN_TEST(test_next_charge_discharging_from_charged);
    RUN_TEST(test_next_charge_absent_battery_is_unknown_not_charged);

    RUN_TEST(test_presence_current_proves_battery);
    RUN_TEST(test_presence_low_voltage_proves_no_battery);
    RUN_TEST(test_presence_plausible_voltage_proves_nothing);
    RUN_TEST(test_presence_absence_is_latched);
    RUN_TEST(test_presence_zero_reading_proves_nothing);

    RUN_TEST(test_battery_low_below_threshold);
    RUN_TEST(test_battery_not_low_at_threshold);
    RUN_TEST(test_battery_unknown_is_not_low);

    RUN_TEST(test_battery_fill_unknown_is_minus_one);
    RUN_TEST(test_battery_fill_clamps);
    RUN_TEST(test_battery_fill_charged_is_full);

    RUN_TEST(test_info_equality_ignores_nothing);
    RUN_TEST(test_info_equality_zero_initialised);
    return UNITY_END();
}
