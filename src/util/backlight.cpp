// Таймаут подсветки — правила без железа.
//
// Подробности и зачем это отдельный модуль — в include/util/backlight.h.

#include "util/backlight.h"

namespace backlight {

uint32_t timeout_ms(uint8_t timeout_min)
{
    if (timeout_min == 0) return 0;
    // Переполнение здесь не грозит: 255 минут — это 15.3 млн мс, меньше
    // 2^32. Ограничение по настройке (60 мин) стоит ещё и в settings::validate.
    return static_cast<uint32_t>(timeout_min) * 60u * 1000u;
}

Action decide(bool was_on, bool timed_out, const Activity& a)
{
    // Работа идёт — ждать её важнее, чем беречь экран. Ответ модели приходит
    // по частям, и человек смотрит именно на него; погасить экран посреди
    // стриминга значило бы спрятать то, что он ждал.
    if (a.busy) return was_on ? Action::None : Action::On;

    // Любое действие человека возвращает подсветку. Проверяется до гашения:
    // если событие пришло в тот же кадр, что истёк таймаут, человек как раз
    // тронул устройство, и гасить незачем.
    if (a.key || a.touch) return was_on ? Action::None : Action::On;

    if (was_on && timed_out) return Action::Off;

    // Подсветка погашена, бездействие продолжается — ничего не делаем.
    return Action::None;
}

uint8_t brightness(uint8_t normal, bool on)
{
    if (!on) return 0;
    // Яркость 0 в настройках означала бы «всегда темно»: настройка допускает
    // 0..255, и 0 — это не «не гасить», а именно погашенная подсветка. Чтобы
    // устройство не оказалось невидимым по ошибке в config.json, включаем
    // минимальную видимую.
    return normal == 0 ? 1 : normal;
}

}  // namespace backlight
