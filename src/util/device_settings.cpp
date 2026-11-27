// Параметры устройства: шаги и ограничения — без Arduino.
//
// Подробности и обоснование — в include/util/device_settings.h.

#include "util/device_settings.h"

#include <string.h>

#include "util/settings.h"

namespace devsettings {
namespace {

/// Ступени таймаута подсветки. Между 1 и 2 минутами разницы для человека нет,
/// а между 2 и 10 — есть, поэтому шкала неравномерная.
constexpr uint8_t kTimeoutSteps[] = {0, 1, 2, 5, 10, 20, 30, 60};
constexpr size_t kTimeoutCount = sizeof(kTimeoutSteps) / sizeof(kTimeoutSteps[0]);

/// Готовые зоны. Порядок — от Москвы к востоку: так чаще всего смотрят в начало
/// списка, а Камчатка и Магадан нужны реже.
constexpr Zone kZones[] = {
    {"Москва (UTC+3)", "MSK-3"},
    {"Калининград (UTC+2)", "EET-2"},
    {"Самара (UTC+4)", "SAMT-4"},
    {"Екатеринбург (UTC+5)", "YEKT-5"},
    {"Омск (UTC+6)", "OMST-6"},
    {"Красноярск (UTC+7)", "KRAT-7"},
    {"Иркутск (UTC+8)", "IRKT-8"},
    {"Якутск (UTC+9)", "YAKT-9"},
    {"Владивосток (UTC+10)", "VLAT-10"},
    {"Магадан (UTC+11)", "MAGT-11"},
    {"Камчатка (UTC+12)", "PETT-12"},
    {"UTC", "UTC-0"},
};
constexpr size_t kZoneCount = sizeof(kZones) / sizeof(kZones[0]);

}  // namespace

size_t zone_count()
{
    return kZoneCount;
}

const Zone* zone_at(size_t index)
{
    return index < kZoneCount ? &kZones[index] : nullptr;
}

int zone_index(const char* posix)
{
    if (posix == nullptr) return -1;
    for (size_t i = 0; i < kZoneCount; ++i) {
        if (strcmp(kZones[i].posix, posix) == 0) return static_cast<int>(i);
    }
    return -1;
}

const Zone& zone_step(const char* posix, int delta)
{
    int index = zone_index(posix);
    if (index < 0) index = 0;

    const int count = static_cast<int>(kZoneCount);
    // Остаток по модулю, а не if на край: так шаг назад от первой зоны даёт
    // последнюю, а не отказ. Отрицательный остаток в C++ возможен, поэтому
    // прибавляем count.
    int next = (index + delta) % count;
    if (next < 0) next += count;
    return kZones[next];
}

uint8_t brightness_step(uint8_t current, int delta)
{
    // Считаем в int: при uint8_t шаг назад от 16 ушёл бы в 240.
    int value = static_cast<int>(current) + delta * static_cast<int>(kBrightnessStep);
    if (value < kBrightnessMin) value = kBrightnessMin;
    if (value > kBrightnessMax) value = kBrightnessMax;
    return static_cast<uint8_t>(value);
}

uint8_t timeout_step(uint8_t current, int delta)
{
    if (delta == 0) return current;

    // Если значение пришло из файла кривым (например, 7 — правили руками),
    // сначала встаём на ближайшую ступень В СТОРОНУ ШАГА: «7 → вперёд → 10»,
    // «7 → назад → 5». Это читается как округление, а не как прыжок через
    // ступень.
    const int step = delta > 0 ? 1 : -1;
    int remaining = delta;

    int index = -1;
    for (size_t i = 0; i < kTimeoutCount; ++i) {
        if (kTimeoutSteps[i] == current) {
            index = static_cast<int>(i);
            break;
        }
    }

    if (index < 0) {
        // Не ступень: подбираем ближайшую в сторону шага и тратим на это один
        // шаг из delta.
        int best = -1;
        for (size_t i = 0; i < kTimeoutCount; ++i) {
            const int v = static_cast<int>(kTimeoutSteps[i]);
            if (step > 0 ? v > static_cast<int>(current) : v < static_cast<int>(current)) {
                if (best < 0 ||
                    (step > 0 ? v < static_cast<int>(kTimeoutSteps[best]) : v > static_cast<int>(kTimeoutSteps[best])))
                    best = static_cast<int>(i);
            }
        }
        if (best < 0) {
            // Дальше некуда: при шаге вперёд это значит «уже максимум».
            return step > 0 ? kTimeoutSteps[kTimeoutCount - 1] : kTimeoutSteps[0];
        }
        index = best;
        remaining -= step;
    }

    // Дальше — по ступеням, а не по минутам.
    int next = index + remaining;
    if (next < 0) next = 0;
    if (next >= static_cast<int>(kTimeoutCount)) next = static_cast<int>(kTimeoutCount) - 1;
    return kTimeoutSteps[next];
}

uint8_t battery_value(const settings::Config& cfg, Battery which)
{
    switch (which) {
    case Battery::Warn: return cfg.battery_warn_percent;
    case Battery::Critical: return cfg.battery_critical_percent;
    case Battery::Shutdown: return cfg.battery_shutdown_percent;
    default: return 0;
    }
}

const char* battery_name(Battery which)
{
    switch (which) {
    case Battery::Warn: return "Предупредить о разряде";
    case Battery::Critical: return "Критический разряд";
    case Battery::Shutdown: return "Выключиться при";
    default: return "";
    }
}

bool battery_step(settings::Config& cfg, Battery which, int delta)
{
    if (delta == 0) return false;

    // Шаг может быть больше единицы (`dev warn+5`): повторяем ровно столько раз,
    // сколько просили. Иначе команда «+5» тихо сработала бы как «+1», и
    // приёмка записала бы неверное значение.
    const int steps = delta > 0 ? delta : -delta;
    const int step = delta > 0 ? 1 : -1;

    bool changed = false;
    for (int i = 0; i < steps; ++i) {
        const uint8_t current = battery_value(cfg, which);

        int value = static_cast<int>(current) + step;
        if (value < kMinBattery) value = kMinBattery;
        if (value > kMaxBattery) value = kMaxBattery;

        // Порядок обязателен: warn > critical > shutdown. Порог упирается в
        // соседний, а не перескакивает его — иначе предупреждение о разряде не
        // показалось бы никогда, а validate() отказал бы при сохранении уже
        // после того, как человек ушёл с экрана.
        switch (which) {
        case Battery::Warn:
            // Предупреждение должно остаться строго выше критического.
            if (value <= static_cast<int>(cfg.battery_critical_percent)) value = cfg.battery_critical_percent + 1;
            break;
        case Battery::Critical:
            // Между предупреждением и выключением.
            if (value >= static_cast<int>(cfg.battery_warn_percent)) value = cfg.battery_warn_percent - 1;
            if (value <= static_cast<int>(cfg.battery_shutdown_percent)) value = cfg.battery_shutdown_percent + 1;
            break;
        case Battery::Shutdown:
            if (value >= static_cast<int>(cfg.battery_critical_percent)) value = cfg.battery_critical_percent - 1;
            break;
        default: return false;
        }

        if (value < kMinBattery) value = kMinBattery;
        if (value > kMaxBattery) value = kMaxBattery;

        const uint8_t next = static_cast<uint8_t>(value);
        if (next == current) break;  // упёрлись — дальше шагать некуда

        switch (which) {
        case Battery::Warn: cfg.battery_warn_percent = next; break;
        case Battery::Critical: cfg.battery_critical_percent = next; break;
        case Battery::Shutdown: cfg.battery_shutdown_percent = next; break;
        default: return false;
        }
        changed = true;

        // После упора соседний порог мог сравняться с этим (например, critical
        // подтянулся к warn при уменьшении warn). Разводим их на единицу, чтобы
        // validate() не отказал.
        if (cfg.battery_critical_percent >= cfg.battery_warn_percent) {
            cfg.battery_critical_percent =
                cfg.battery_warn_percent > kMinBattery ? cfg.battery_warn_percent - 1 : kMinBattery;
        }
        if (cfg.battery_shutdown_percent >= cfg.battery_critical_percent) {
            cfg.battery_shutdown_percent =
                cfg.battery_critical_percent > kMinBattery ? cfg.battery_critical_percent - 1 : kMinBattery;
        }
    }

    return changed;
}

Step parse_step(const char* text, size_t len)
{
    Step step;
    if (text == nullptr || len == 0) return step;

    // Отделяем имя от шага: `bright+1`, `warn-1`, `layout`.
    size_t name_len = len;
    int delta = 1;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '+' || text[i] == '-') {
            name_len = i;
            delta = text[i] == '-' ? -1 : 1;
            // После знака — число: «+2» значит два шага. Знак без числа —
            // не команда: «bright+» выглядит как опечатка, и угадывать
            // направление за человека незачем.
            int times = 0;
            for (size_t k = i + 1; k < len; ++k) {
                if (text[k] < '0' || text[k] > '9') return step;
                times = times * 10 + (text[k] - '0');
                if (times > 100) return step;
            }
            if (i + 1 >= len) return step;
            delta *= times;
            break;
        }
    }

    const auto word_is = [&](const char* word) {
        const size_t n = strlen(word);
        return name_len == n && strncmp(text, word, n) == 0;
    };

    if (word_is("bright") || word_is("br")) {
        step.param = Param::Brightness;
    } else if (word_is("timeout") || word_is("to")) {
        step.param = Param::Timeout;
    } else if (word_is("layout") || word_is("lay")) {
        // У раскладки нет направления: значений два, и любой шаг меняет сторону.
        step.param = Param::Layout;
        step.delta = 0;
    } else if (word_is("warn") || word_is("w")) {
        step.param = Param::Warn;
    } else if (word_is("crit") || word_is("c")) {
        step.param = Param::Critical;
    } else if (word_is("shut") || word_is("s")) {
        step.param = Param::Shutdown;
    } else if (word_is("tz") || word_is("zone")) {
        step.param = Param::Timezone;
    } else if (word_is("save")) {
        // Ctrl+S скриптом не нажать, а без него проверка «параметры легли в
        // config.json» требует человека у клавиатуры.
        step.param = Param::Save;
        step.delta = 0;
        return step;
    } else {
        return step;
    }

    step.delta = delta;
    return step;
}

}  // namespace devsettings
