// Цвета светодиодов клавиатуры: чистые правила, проверяемые на хосте.

#include "util/led_state.h"

namespace led {

bool Status::operator==(const Status& o) const
{
    return busy == o.busy && streaming == o.streaming && retrying == o.retrying && error == o.error &&
           charging == o.charging && discharging == o.discharging && battery_level == o.battery_level;
}

Colors compute(const Status& s)
{
    Colors c;

    // --- Левый: состояние модели ---
    //
    // Порядок важен. Во время паузы перед повтором ошибка уже записана в поле
    // сообщения, но показывать её красным значит сказать «всё плохо», хотя
    // клиент в этот момент как раз работает. Поэтому повтор проверяется раньше.
    //
    // Уровни яркости: простой — «тихо», ответ и запрос — «работа», повтор и
    // ошибка — «тревога». Почему именно так, а не просто «цвета поярче»: при
    // одной яркости в регистре зелёный казался в разы ярче синего, и левый
    // светодиод выглядел погасшим рядом с правым.
    if (s.retrying) {
        c.model = kAlarmOrange;
    } else if (s.error) {
        c.model = kAlarmRed;
    } else if (s.streaming) {
        c.model = kWorkGreen;
    } else if (s.busy) {
        // Запрос ушёл, первого текста ещё нет. Тот же синий, что и в простое,
        // но на уровень ярче: разница видна, а тревоги не добавляет — это
        // штатное состояние, а не проблема.
        c.model = kWorkBlue;
    } else {
        c.model = kQuietBlue;
    }

    // --- Правый: питание ---
    //
    // Проценты берём только тогда, когда по току видно, что аккумулятор
    // реально работает: на пустом отсеке getBatteryLevel() врёт 100 %.
    if (s.charging) {
        // Заряд — это «работа»: идёт процесс, но тревоги нет.
        c.power = kWorkBlue;
    } else if (s.discharging) {
        // Нормальный разряд — «тихо»: то же самое, что левый в простое.
        // Раньше здесь стоял зелёный полной яркости, и правый светодиод
        // светился заметно ярче левого, хотя оба сообщали «всё в порядке».
        const bool low = s.battery_level >= 0 && s.battery_level < kLowBatteryPercent;
        c.power = low ? kAlarmOrange : kQuietGreen;
    } else {
        // idle: «заряжен» и «аккумулятора нет» неразличимы. Гасим — это
        // честнее, чем зелёный на пустом отсеке.
        c.power = kOff;
    }

    return c;
}

}  // namespace led
