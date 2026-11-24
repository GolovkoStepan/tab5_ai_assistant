// Системные часы: таймзона и синхронизация по NTP.
//
// Три источника времени и порядок их использования описаны в hw/clock.h.
// Здесь только работа с ними: TZ, SNTP, RTC.

#include "hw/clock.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <string.h>

namespace hw {
namespace clock_sync {
namespace {

constexpr const char* kTag = "clock";

/// Серверы времени. Первый обычно отвечает быстрее, остальные — запас.
constexpr const char* kNtp1 = "pool.ntp.org";
constexpr const char* kNtp2 = "time.google.com";

/// Через сколько перезапускать SNTP, если время так и не пришло.
constexpr uint32_t kRetryMs = 20000;

/// Сколько попыток делаем, прежде чем смириться с часами без NTP.
constexpr int kMaxAttempts = 10;

/// Скачок времени вперёд больше этого значения считаем сверкой по сети:
/// системные часы сами так не ходят, а RTC мы перечитываем только при старте.
constexpr long kSyncJumpSeconds = 5;

bool g_started = false;
bool g_synced = false;
unsigned long g_attempt_ms = 0;
int g_attempts = 0;

/// Записать текущее системное время в RTC.
///
/// В регистрах RTC лежит UTC, а не местное время: M5Unified при чтении
/// пересчитывает её через mktime с принудительным GMT0 (см.
/// RTC_Class::setSystemTimeFromRtc). Если записать туда местное, после
/// перезагрузки часы уедут на величину таймзоны.
bool write_rtc_from_system()
{
    if (!M5.Rtc.isEnabled()) return false;

    const time_t now = time(nullptr);
    if (now < kMinValidEpoch) return false;

    struct tm utc{};
    gmtime_r(&now, &utc);

    m5::rtc_datetime_t dt;
    dt.date = m5::rtc_date_t(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_wday);
    dt.time.hours = static_cast<int8_t>(utc.tm_hour);
    dt.time.minutes = static_cast<int8_t>(utc.tm_min);
    dt.time.seconds = static_cast<int8_t>(utc.tm_sec);

    // День недели берём из gmtime_r (0..6), а не -1: драйвер RX8130 проверяет
    // поля через isValid(), где weekDay == -1 считается ошибкой. «Пусть RTC
    // посчитает сам» здесь не работает.

    if (!M5.Rtc.setDateTime(&dt)) {
        ESP_LOGW(kTag, "не удалось записать время в RTC");
        return false;
    }
    return true;
}

}  // namespace

void begin(const settings::Config& cfg)
{
    // Таймзона — переменной TZ, чтобы localtime_r() сразу давал местное время.
    // Пустая строка не годится: тогда часы считались бы по UTC, а метки в
    // треде разошлись бы с тем, что видит пользователь.
    const char* tz = cfg.timezone[0] != '\0' ? cfg.timezone : settings::defaults::kTimezone;
    setenv("TZ", tz, 1);
    tzset();

    // M5Unified подхватил время из RTC в M5.begin(), но только если RTC отдал
    // осмысленную дату. На Tab5 с недозаряженным суперконденсатором он отдаёт
    // «2000-01--1», и системные часы остаются на нуле. Тогда единственный
    // источник — NTP, и до него метки времени писать нечем.
    g_started = true;
    g_synced = false;
    g_attempts = 0;
    g_attempt_ms = 0;

    ESP_LOGI(kTag, "таймзона %s, время %s", tz, valid() ? "есть (из RTC)" : "не выставлено (ждём NTP)");
}

void tick(bool wifi_connected)
{
    if (!g_started) return;

    // Сверка уже была — дальше часы идут сами.
    if (g_synced) return;

    // Штатный признак: SNTP выставляет COMPLETED после ответа сервера.
    // Читать его нужно ровно один раз — при чтении lwIP сбрасывает статус в
    // RESET, и второй вызов уже ничего не скажет. Поэтому результат сразу
    // запоминаем в g_synced.
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && time(nullptr) >= kMinValidEpoch) {
        g_synced = true;
        if (write_rtc_from_system()) {
            ESP_LOGI(kTag, "время сверено по NTP и записано в RTC");
        } else {
            ESP_LOGI(kTag, "время сверено по NTP, RTC недоступен");
        }
        return;
    }

    if (!wifi_connected) return;

    const unsigned long ms = millis();
    if (g_attempts == 0 || ms - g_attempt_ms > kRetryMs) {
        if (g_attempts >= kMaxAttempts) return;

        ++g_attempts;
        g_attempt_ms = ms;
        // configTzTime и задаёт TZ, и запускает SNTP. Повторный вызов
        // перезапускает запрос — это дешевле, чем ждать, пока lwIP сам
        // сообразит, что первый ушёл в никуда.
        configTzTime(getenv("TZ"), kNtp1, kNtp2);
        ESP_LOGI(kTag, "запрос времени по NTP (попытка %d)", g_attempts);
    }
}

bool valid()
{
    // time() до синхронизации возвращает секунды с загрузки, а не дату.
    // Сравнение с 2020 годом отсекает и это, и мусор из незаряженного RTC.
    return time(nullptr) >= kMinValidEpoch;
}

bool synced()
{
    return g_synced;
}

bool store_to_rtc()
{
    return write_rtc_from_system();
}

}  // namespace clock_sync
}  // namespace hw
