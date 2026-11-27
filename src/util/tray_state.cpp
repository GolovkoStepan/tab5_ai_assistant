// Что показывает системный трей: правила без железа.
//
// Подробности — в include/util/tray_state.h. Коротко: процент заряда верен
// только на разряде (зарядник поднимает напряжение до потолка формулы), а у
// Wi-Fi «нет связи» должно отличаться от «связь есть».

#include "util/tray_state.h"

#include <stdio.h>
#include <string.h>

#include "util/led_state.h"
#include "util/wifi_list.h"

namespace tray {

bool Info::operator==(const Info& o) const
{
    return wifi_connected == o.wifi_connected && wifi_connecting == o.wifi_connecting && wifi_level == o.wifi_level &&
           charge == o.charge && battery_known == o.battery_known && battery_percent == o.battery_percent &&
           strcmp(wifi_text, o.wifi_text) == 0 && strcmp(battery_text, o.battery_text) == 0 &&
           strcmp(clock_text, o.clock_text) == 0;
}

void wifi_label(bool connected, bool connecting, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;

    if (connected) {
        // Пусто: значок с заполненными палочками уже ответил, что связь есть.
        // Адрес здесь дублировал бы экран Wi-Fi и отнимал сотню пикселей у
        // заголовка.
        out[0] = '\0';
        return;
    }

    snprintf(out, out_len, "%s", connecting ? "…" : "нет");
}

void battery_label(Charge charge, bool percent_known, int percent, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;

    // Заряд идёт — процент показывать нельзя. `getBatteryLevel()` на Tab5
    // считает его по напряжению на ячейку, а зарядник держит 4.2 В на ячейку,
    // то есть верхнюю границу формулы: сразу после подключения кабеля там
    // оказывается 100 % при любом реальном заряде. Направление тока при этом
    // известно достоверно, и слово честнее выдуманного числа.
    if (charge == Charge::Charging) {
        snprintf(out, out_len, "%s", "заряд");
        return;
    }

    if (charge == Charge::Charged) {
        // Заряд в этой сессии шёл и ток упал до нуля. Процента по-прежнему нет:
        // при полном заряде формула врёт так же, как и во время него.
        snprintf(out, out_len, "%s", "заряжено");
        return;
    }

    if (percent_known) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        snprintf(out, out_len, "%d %%", percent);
        return;
    }

    // Ни заряда, ни разряда: аккумулятора нет либо он заряжен, и различить это
    // нечем (Р-016). Напряжение здесь не показываем намеренно, хотя оно и
    // известно. При пустом отсеке INA226 видит не аккумулятор, а холостой ход
    // зарядного узла IP2326, и тот **периодически поднимается и гаснет**:
    // на живой плате показание скачет между 8.37 и 4.35 В каждые несколько
    // секунд. В трее это было бы мигание подписи, а число, которое одинаково и
    // при пустом отсеке, и при заряженном, не сообщает ничего.
    //
    // Прочерк, а не пустое место: пустой корпус без подписи читается как
    // «ноль процентов», то есть как утверждение вместо отсутствия данных.
    // Подробности и напряжение остались на экране диагностики.
    snprintf(out, out_len, "%s", "—");
}

Charge next_charge(Charge previous, bool charging, bool discharging, bool absent)
{
    if (charging) return Charge::Charging;
    if (discharging) return Charge::Discharging;

    // Тока нет. Если аккумулятора заведомо нет, то и заряжаться нечему:
    // «заряжено» здесь было бы утверждением о несуществующей батарее.
    // Этот случай и увидел владелец, сняв аккумулятор при подключённом кабеле:
    // раньше история помнила, что заряд шёл, и объявляла его завершённым.
    if (absent) return Charge::Unknown;

    // Заряд в этой сессии уже шёл — значит он закончился. Если не шёл, честно
    // сказать нечего: аккумулятора может не быть вовсе.
    if (previous == Charge::Charging || previous == Charge::Charged) return Charge::Charged;
    return Charge::Unknown;
}

Presence next_presence(Presence previous, bool current_flows, int millivolts)
{
    // Ток — прямое доказательство: он течёт только через аккумулятор.
    // Снимаем защёлку: батарею могли вставить обратно.
    if (current_flows) return Presence{false};

    // Уже знаем, что её нет, а доказательств обратного не появилось.
    if (previous.absent) return previous;

    // Напряжение ниже физически возможного для 2S-сборки: это холостой ход
    // зарядного узла, а не батарея. Защёлкиваем.
    if (millivolts > 0 && millivolts < kPackMinPlausibleMv) return Presence{true};

    return previous;
}

uint8_t wifi_level(bool connected, int rssi)
{
    // «Не подключено» — это ноль палочек, каким бы ни был последний RSSI.
    // Драйвер отдаёт 0 при отсутствии связи, а таблица порогов считает это
    // отличным сигналом: без этой проверки значок врал бы сильнее всего
    // именно тогда, когда сеть пропала.
    if (!connected) return 0;
    return wifi::signal_level(rssi);
}

bool clock_label(int weekday, int day, int month, int hour, int minute, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return false;
    out[0] = '\0';

    // Порядок как в struct tm: 0 — воскресенье. В русском «Пн» — первый день,
    // но переставлять здесь нечего: индекс в таблицу берётся прямой.
    static const char* const kWeekdays[7] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
    // Сокращения в родительном падеже — так они читаются рядом с числом
    // («27 нояб.»). Короткие месяцы не режем: «мая» и «июня» уже короче
    // любого сокращения.
    static const char* const kMonths[12] = {
        "янв.", "февр.", "мар.", "апр.", "мая", "июня", "июля", "авг.", "сент.", "окт.", "нояб.", "дек."};

    if (weekday < 0 || weekday > 6) return false;
    if (day < 1 || day > 31) return false;
    if (month < 1 || month > 12) return false;
    if (hour < 0 || hour > 23) return false;
    if (minute < 0 || minute > 59) return false;

    const int n =
        snprintf(out, out_len, "%s, %d %s %02d:%02d", kWeekdays[weekday], day, kMonths[month - 1], hour, minute);
    if (n < 0 || static_cast<size_t>(n) >= out_len) {
        // Не влезло — пустая строка, а не обрезанная посреди слова дата:
        // «Пт, 27 ноя» читается как другой месяц.
        out[0] = '\0';
        return false;
    }
    return true;
}

bool battery_low(bool known, int percent)
{
    return known && percent >= 0 && percent < led::kLowBatteryPercent;
}

int battery_fill(Charge charge, bool known, int percent)
{
    // «Заряжено» — единственный случай, когда уровень известен без процента:
    // это верхняя граница по определению. Пустой корпус рядом со словом
    // «заряжено» читался бы как «ноль» и спорил с подписью.
    if (charge == Charge::Charged) return 100;

    // «Неизвестно» — это только флаг known (процент недостоверен: идёт заряд
    // или аккумулятора нет). Отрицательный процент при known — просто мусор во
    // входных данных, и его надо прижать к нулю, а не выдавать за
    // «неизвестно»: иначе два признака одного состояния спорят друг с другом.
    if (!known) return -1;
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

}  // namespace tray
