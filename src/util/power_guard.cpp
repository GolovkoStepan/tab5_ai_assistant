// Правила предупреждения о разряде — без железа.
//
// Подробности и зачем это отдельный модуль — в include/util/power_guard.h.

#include "util/power_guard.h"

#include <stddef.h>

namespace power {
namespace {

/// Пороги в правильном порядке. Если в config.json они перепутаны (warn ниже
/// critical), проверка уровня сравнивала бы с меньшим первым и никогда не
/// дошла бы до следующей ступени. settings::validate такое не пропускает, но
/// полагаться на это нельзя: модуль чистый и может быть вызван с чем угодно.
bool ordered(const Thresholds& t, uint8_t* first, uint8_t* second, uint8_t* third)
{
    *first = t.warn;
    *second = t.critical;
    *third = t.shutdown;
    return t.warn > t.critical && t.critical > t.shutdown;
}

}  // namespace

Level level(bool known, int percent, const Thresholds& t)
{
    if (!known) return Level::Ok;
    if (percent < 0 || percent > 100) return Level::Ok;

    uint8_t warn = 0;
    uint8_t critical = 0;
    uint8_t shutdown = 0;
    if (!ordered(t, &warn, &critical, &shutdown)) {
        // Пороги перепутаны — работаем по значениям по умолчанию, а не по
        // мусору из настроек. Это лучше, чем не предупредить о разряде вовсе.
        const Thresholds safe;
        warn = safe.warn;
        critical = safe.critical;
        shutdown = safe.shutdown;
    }

    // Сравнение «ниже или равно»: порог 20 означает «на 20 % уже пора».
    // Иначе при заряде ровно 20 % предупреждение не покажется, и человек
    // увидит его только на 19 — то есть на шаг позже обещанного.
    if (percent <= static_cast<int>(shutdown)) return Level::Shutdown;
    if (percent <= static_cast<int>(critical)) return Level::Critical;
    if (percent <= static_cast<int>(warn)) return Level::Warn;
    return Level::Ok;
}

Level next_shown(Level shown, Level now)
{
    // Показываем самый серьёзный из двух: если уже горит «критично», а заряд
    // подрос до 15 %, не надо откатываться к «предупреждению» — это выглядело
    // бы как улучшение, которого не было. Снимаем баннер только на Ok.
    if (now == Level::Ok) return Level::Ok;
    if (static_cast<uint8_t>(now) > static_cast<uint8_t>(shown)) return now;
    return shown;
}

const char* message(Level level)
{
    switch (level) {
    case Level::Warn: return "Батарея садится — подключите зарядку";
    case Level::Critical: return "Батарея почти разряжена — сохраните работу и подключите зарядку";
    case Level::Shutdown: return "Батарея разряжена — устройство выключится";
    case Level::Ok: break;
    }
    return "";
}

}  // namespace power
