#pragma once

// Настройки устройства: секреты в NVS, остальное — в /sdcard/config.json.
//
// Разделение намеренное (см. docs/ARCHITECTURE.md, 4.5): SD-карту легко вынуть
// и прочитать на компьютере, поэтому Wi-Fi-пароль и ключ API живут только в NVS.
// Остальные настройки — в JSON, чтобы их можно было править руками и бэкапить.
//
// Чистая логика (валидация полей, разбор JSON) отделена от железа — её
// проверяют юнит-тесты на хосте, без платы.

#include <stddef.h>
#include <stdint.h>

namespace settings {

/// Максимальная длина строковых полей. SSID и пароль Wi-Fi по стандарту — до
/// 32 и 63 байт; base_url и model с запасом.
constexpr size_t kMaxSsid = 33;
constexpr size_t kMaxPassword = 65;
constexpr size_t kMaxUrl = 192;
constexpr size_t kMaxApiKey = 160;
constexpr size_t kMaxModel = 64;
constexpr size_t kMaxTimezone = 32;

/// Секреты. Хранятся только в NVS, в файл не попадают никогда.
struct Secrets {
    char wifi_ssid[kMaxSsid]{};
    char wifi_password[kMaxPassword]{};
    char api_key[kMaxApiKey]{};
};

/// Несекретные настройки. Хранятся в /sdcard/config.json.
struct Config {
    char api_base_url[kMaxUrl]{};
    char api_model[kMaxModel]{};
    /// Температура генерации, 0..20 (то есть 0.0..2.0 с шагом 0.1).
    /// Целое, а не float: без плавающей точки в NVS и без ошибок сравнения.
    uint8_t temperature_tenths{3};
    /// Уровень «размышлений» модели: 0 = нет, 1 = низкий, 2 = средний, 3 = высокий.
    uint8_t reasoning_effort{1};
    /// Веб-поиск включён.
    bool search_enabled{true};
    /// Сколько результатов поиска вставлять в контекст.
    uint8_t search_max_results{3};
    /// Яркость экрана, 0..255.
    uint8_t brightness{128};
    /// Раскладка: false = латиница (ENG), true = ЙЦУКЕН (РУС).
    bool layout_russian{false};
    /// Через сколько минут бездействия гаснет экран; 0 — не гасить.
    uint8_t screen_timeout_min{2};
    /// Пороги предупреждения о разряде, проценты.
    uint8_t battery_warn_percent{20};
    uint8_t battery_critical_percent{10};
    uint8_t battery_shutdown_percent{5};
    /// Таймзона в формате POSIX: "MSK-3".
    char timezone[kMaxTimezone]{"MSK-3"};
};

namespace defaults {

/// Значения по умолчанию. Отдельно от Config, чтобы было видно, что меняется
/// при сбросе настроек, и чтобы сравнить «заполнено ли поле».
constexpr const char* kApiBaseUrl = "https://routerai.ru/api/v1";
constexpr const char* kApiModel = "~deepseek/deepseek-v4-flash-latest";
constexpr const char* kTimezone = "MSK-3";

}  // namespace defaults

/// Заполняет Config значениями по умолчанию.
void make_default_config(Config& out);

/// Проверяет поля перед сохранением. Возвращает false и пишет причину в err.
/// Проверяется то, что реально ломает работу: пустой адрес, адрес без схемы,
/// температура вне диапазона, слишком длинная таймзона.
bool validate(const Config& cfg, char* err, size_t err_len);

/// Годится ли строка как таймзона POSIX.
///
/// Проверяется не «красиво ли написано», а «поймёт ли это setenv("TZ")»:
/// нужна непустая строка с именем и смещением («YEKT-5», «MSK-3», «UTC-0»).
/// Строка без смещения («Europe/Moscow») не годится: newlib на ESP32 не умеет
/// базу зон IANA, и TZ станет пустой — тогда все метки уедут в UTC молча.
///
/// @param tz строка из настроек или из команды `tz`
bool valid_timezone(const char* tz);

/// Разбирает JSON из /sdcard/config.json. Отсутствующие поля остаются как есть,
/// поэтому перед вызовом нужно заполнить Config значениями по умолчанию.
/// Возвращает false, если JSON синтаксически неверен или не объект.
/// Поля неизвестного типа пропускаются, а не считаются ошибкой: так старый
/// файл конфигурации не ломает новую прошивку.
bool parse_config_json(const char* json, Config& out, char* err, size_t err_len);

/// Сериализует Config в JSON. Секретов здесь нет по построению.
/// @return число записанных байт без завершающего нуля, 0 если буфер мал
size_t write_config_json(const Config& cfg, char* out, size_t out_len);

/// Экранирует строку для вставки в JSON-литерал, без обрамляющих кавычек.
/// @return число записанных байт без завершающего нуля, 0 если буфер мал
size_t json_escape(const char* src, char* out, size_t out_len);

}  // namespace settings
