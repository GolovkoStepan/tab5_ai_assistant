/*
 * Тесты настроек: валидация, разбор и запись JSON.
 *
 * Проверяем то, что нельзя проверить на плате глазами: корректность разбора
 * конфига, устойчивость к мусору в файле и то, что секреты не попадают в JSON.
 */

#include <string.h>
#include <unity.h>

#include "app_config.h"
#include "util/settings.h"

void setUp(void) {}
void tearDown(void) {}

// Копирование строки с гарантированным нулём.
// Своя функция, потому что strlcpy есть в macOS и BSD, но нет в glibc:
// локально тесты собирались, а на Ubuntu в CI линковка падала бы.
void copy_str(char* dst, size_t dst_len, const char* src)
{
    const size_t n = strlen(src);
    const size_t take = n < dst_len - 1 ? n : dst_len - 1;
    memcpy(dst, src, take);
    dst[take] = '\0';
}

// --- Пути на SD ---

/// Пути в cfg::k*Dir/kConfigPath указаны относительно точки монтирования,
/// без префикса kMountPoint.
///
/// Баг с этим был пойман на живой плате: SD_MMC.begin("/sdcard") монтирует
/// ФС, а SD_MMC.open() работает уже внутри неё. Полный путь "/sdcard/x"
/// превращался в "/sdcard/sdcard/x", fopen падал, и config.json молча не
/// создавался — на экране всё выглядело нормально, в логе только предупреждение.
/// Тест на хосте дешевле, чем поиск такого на устройстве.
void test_sd_paths_are_relative_to_mount_point()
{
    const char* mnt = cfg::kMountPoint;
    const size_t mnt_len = strlen(mnt);

    const char* paths[] = {cfg::kNotesDir, cfg::kChatsDir, cfg::kConfigPath};
    for (const char* p : paths) {
        TEST_ASSERT_NOT_NULL(p);
        // Абсолютный путь внутри ФС — обязателен.
        TEST_ASSERT_EQUAL_CHAR('/', p[0]);
        // Но без префикса точки монтирования.
        const bool prefixed = strncmp(p, mnt, mnt_len) == 0;
        TEST_ASSERT_FALSE_MESSAGE(prefixed, p);
    }
}

// --- Значения по умолчанию ---

void test_defaults_are_filled()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    TEST_ASSERT_EQUAL_STRING("https://routerai.ru/api/v1", cfg.api_base_url);
    TEST_ASSERT_EQUAL_STRING("~deepseek/deepseek-v4-flash-latest", cfg.api_model);
    TEST_ASSERT_EQUAL_STRING("MSK-3", cfg.timezone);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.temperature_tenths);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.reasoning_effort);
    TEST_ASSERT_TRUE(cfg.search_enabled);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.search_max_results);
    TEST_ASSERT_EQUAL_UINT8(128, cfg.brightness);
    TEST_ASSERT_FALSE(cfg.layout_russian);
}

void test_defaults_are_valid()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", err);
}

// --- Валидация ---

void test_validate_rejects_empty_url()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    cfg.api_base_url[0] = '\0';

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "пустой") != nullptr);
}

void test_validate_rejects_url_without_scheme()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    copy_str(cfg.api_base_url, sizeof(cfg.api_base_url), "routerai.ru/api/v1");

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "http") != nullptr);
}

void test_validate_rejects_url_without_host()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    copy_str(cfg.api_base_url, sizeof(cfg.api_base_url), "https://");

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "хоста") != nullptr);
}

void test_validate_rejects_empty_model()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    cfg.api_model[0] = '\0';

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
}

void test_validate_rejects_bad_temperature()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    cfg.temperature_tenths = 21;  // 2.1 — вне диапазона

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "температура") != nullptr);
}

void test_validate_accepts_http()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    copy_str(cfg.api_base_url, sizeof(cfg.api_base_url), "http://localhost:8080/v1");

    char err[128];
    TEST_ASSERT_TRUE(settings::validate(cfg, err, sizeof(err)));
}

// --- Разбор JSON ---

void test_parse_full_config()
{
    const char* json = R"({
        "api": {"base_url": "https://example.com/v1", "model": "some-model",
                "temperature": 0.7, "reasoning_effort": 2},
        "search": {"enabled": false, "max_results": 5, "engine": "native"},
        "ui": {"brightness": 200, "layout": "ru", "timeout_min": 7},
        "power": {"warn": 25, "critical": 12, "shutdown": 6},
        "time": {"tz": "UTC-0"}
    })";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));

    TEST_ASSERT_EQUAL_STRING("https://example.com/v1", cfg.api_base_url);
    TEST_ASSERT_EQUAL_STRING("some-model", cfg.api_model);
    TEST_ASSERT_EQUAL_UINT8(7, cfg.temperature_tenths);
    TEST_ASSERT_EQUAL_UINT8(2, cfg.reasoning_effort);
    TEST_ASSERT_FALSE(cfg.search_enabled);
    TEST_ASSERT_EQUAL_UINT8(5, cfg.search_max_results);
    TEST_ASSERT_EQUAL_UINT8(200, cfg.brightness);
    TEST_ASSERT_TRUE(cfg.layout_russian);
    TEST_ASSERT_EQUAL_UINT8(7, cfg.screen_timeout_min);
    TEST_ASSERT_EQUAL_UINT8(25, cfg.battery_warn_percent);
    TEST_ASSERT_EQUAL_UINT8(12, cfg.battery_critical_percent);
    TEST_ASSERT_EQUAL_UINT8(6, cfg.battery_shutdown_percent);
    TEST_ASSERT_EQUAL_STRING("UTC-0", cfg.timezone);
}

void test_parse_partial_keeps_defaults()
{
    // Файл только с одним полем: остальное должно остаться по умолчанию.
    const char* json = R"({"brightness": 42})";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));

    TEST_ASSERT_EQUAL_UINT8(42, cfg.brightness);
    TEST_ASSERT_EQUAL_STRING("https://routerai.ru/api/v1", cfg.api_base_url);
    TEST_ASSERT_EQUAL_STRING("~deepseek/deepseek-v4-flash-latest", cfg.api_model);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.temperature_tenths);
}

void test_parse_unknown_keys_are_skipped()
{
    const char* json = R"({
        "from_future_version": {"nested": [1, 2, {"deep": true}]},
        "another": "value",
        "brightness": 77
    })";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT8(77, cfg.brightness);
}

void test_parse_empty_object()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json("{}", cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT8(128, cfg.brightness);
}

void test_parse_rejects_non_object()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_FALSE(settings::parse_config_json("[1,2,3]", cfg, err, sizeof(err)));
    TEST_ASSERT_FALSE(settings::parse_config_json("null", cfg, err, sizeof(err)));
    TEST_ASSERT_FALSE(settings::parse_config_json("", cfg, err, sizeof(err)));
}

void test_parse_rejects_broken_json()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    // Нет закрывающей скобки.
    TEST_ASSERT_FALSE(settings::parse_config_json(R"({"brightness": 42)", cfg, err, sizeof(err)));
    // Нет двоеточия.
    TEST_ASSERT_FALSE(settings::parse_config_json(R"({"brightness" 42})", cfg, err, sizeof(err)));
}

void test_parse_ignores_out_of_range_numbers()
{
    // Температура 5.0 невозможна: поле должно остаться прежним, а не превратиться
    // в мусор при приведении к uint8_t.
    const char* json = R"({"temperature": 5.0, "brightness": 999})";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT8(3, cfg.temperature_tenths);
    TEST_ASSERT_EQUAL_UINT8(128, cfg.brightness);
}

void test_parse_flat_keys_still_work()
{
    // Плоская форма — не документирована, но старый конфиг не должен ломаться.
    const char* json = R"({"brightness": 33, "api_base_url": "https://flat.example/v1"})";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT8(33, cfg.brightness);
    TEST_ASSERT_EQUAL_STRING("https://flat.example/v1", cfg.api_base_url);
}

void test_parse_unknown_section_key_is_skipped()
{
    // "engine" в search и целые незнакомые секции не должны ломать разбор.
    const char* json = R"({
        "search": {"engine": "native", "enabled": false},
        "future": {"nested": [1, {"x": true}]}
    })";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));
    TEST_ASSERT_FALSE(cfg.search_enabled);
}

void test_validate_rejects_bad_power_thresholds()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    cfg.battery_warn_percent = 5;  // ниже critical — предупреждение не покажется
    cfg.battery_critical_percent = 10;

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "убывать") != nullptr);
}

/// Форма POSIX нужна потому, что newlib на ESP32 не знает базы зон IANA:
/// «Europe/Moscow» она молча превратит в UTC, и часы уедут на три часа.
/// Проверить это глазами на плате трудно — поэтому тестом.
void test_valid_timezone_posix()
{
    TEST_ASSERT_TRUE(settings::valid_timezone("YEKT-5"));
    TEST_ASSERT_TRUE(settings::valid_timezone("MSK-3"));
    TEST_ASSERT_TRUE(settings::valid_timezone("UTC-0"));
    TEST_ASSERT_TRUE(settings::valid_timezone("IST-5:30"));
    // Знак POSIX обратный: «UTC+5» — это UTC−5, и это законная запись.
    TEST_ASSERT_TRUE(settings::valid_timezone("UTC+5"));
}

void test_valid_timezone_rejects_iana_names()
{
    TEST_ASSERT_FALSE(settings::valid_timezone("Europe/Moscow"));
    TEST_ASSERT_FALSE(settings::valid_timezone("Asia/Yekaterinburg"));
    TEST_ASSERT_FALSE(settings::valid_timezone(""));
    TEST_ASSERT_FALSE(settings::valid_timezone(nullptr));
    // Смещение вне ±14 часов — опечатка, а не пояс.
    TEST_ASSERT_FALSE(settings::valid_timezone("XXX-15"));
}

void test_validate_rejects_iana_timezone()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    copy_str(cfg.timezone, sizeof(cfg.timezone), "Asia/Yekaterinburg");

    char err[128];
    TEST_ASSERT_FALSE(settings::validate(cfg, err, sizeof(err)));
    TEST_ASSERT_TRUE(strstr(err, "POSIX") != nullptr);
}

void test_truncated_utf8_stops_on_char_boundary()
{
    // Значение длиннее буфера: обрезка не должна разрезать кириллицу пополам —
    // иначе в файл уедет битый UTF-8.
    const char* json = R"({"time": {"tz": "Москва-очень-длинное-имя-таймзоны"}})";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));

    // Проверяем, что в строке нет «висящего» стартового байта UTF-8 без
    // продолжения, то есть длина в символах корректна.
    size_t i = 0;
    while (cfg.timezone[i] != '\0') {
        const unsigned char c = static_cast<unsigned char>(cfg.timezone[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        for (size_t k = 1; k < len; ++k) {
            TEST_ASSERT_NOT_EQUAL('\0', cfg.timezone[i + k]);
        }
        i += len;
    }
}

void test_parse_reasoning_effort_as_string()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(R"({"reasoning_effort": "high"})", cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_UINT8(3, cfg.reasoning_effort);
}

void test_parse_escaped_strings()
{
    const char* json = R"({"api_model": "model\/with\"escape"})";

    settings::Config cfg;
    settings::make_default_config(cfg);

    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, cfg, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("model/with\"escape", cfg.api_model);
}

// --- Запись JSON ---

void test_write_config_roundtrip()
{
    settings::Config cfg;
    settings::make_default_config(cfg);
    copy_str(cfg.api_base_url, sizeof(cfg.api_base_url), "https://example.com/v1");
    cfg.temperature_tenths = 7;
    cfg.brightness = 42;
    cfg.layout_russian = true;

    char json[768];
    const size_t n = settings::write_config_json(cfg, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN(0, n);

    // Читаем обратно — значения должны совпасть.
    settings::Config back;
    settings::make_default_config(back);
    char err[128];
    TEST_ASSERT_TRUE(settings::parse_config_json(json, back, err, sizeof(err)));

    TEST_ASSERT_EQUAL_STRING(cfg.api_base_url, back.api_base_url);
    TEST_ASSERT_EQUAL_UINT8(cfg.temperature_tenths, back.temperature_tenths);
    TEST_ASSERT_EQUAL_UINT8(cfg.brightness, back.brightness);
    TEST_ASSERT_EQUAL(cfg.layout_russian, back.layout_russian);
    TEST_ASSERT_EQUAL_STRING(cfg.api_model, back.api_model);
}

void test_write_config_has_no_secrets()
{
    // В файл не должно попадать ничего из Secrets — даже по имени ключа.
    settings::Config cfg;
    settings::make_default_config(cfg);

    char json[768];
    const size_t n = settings::write_config_json(cfg, json, sizeof(json));
    TEST_ASSERT_GREATER_THAN(0, n);

    TEST_ASSERT_NULL(strstr(json, "api_key"));
    TEST_ASSERT_NULL(strstr(json, "password"));
    TEST_ASSERT_NULL(strstr(json, "ssid"));
}

void test_write_config_fails_on_small_buffer()
{
    settings::Config cfg;
    settings::make_default_config(cfg);

    char json[16];
    TEST_ASSERT_EQUAL_UINT(0, settings::write_config_json(cfg, json, sizeof(json)));
}

// --- Экранирование ---

void test_json_escape_quotes_and_slashes()
{
    char buf[64];
    settings::json_escape("a\"b\\c", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", buf);
}

void test_json_escape_newline()
{
    char buf[32];
    settings::json_escape("a\nb", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("a\\nb", buf);
}

void test_json_escape_keeps_utf8()
{
    // Кириллица не требует экранирования — UTF-8 в JSON допустим как есть.
    char buf[64];
    settings::json_escape("город", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("город", buf);
}

void test_json_escape_control_char()
{
    char buf[32];
    settings::json_escape("a\x01", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("a\\u0001", buf);
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_sd_paths_are_relative_to_mount_point);

    RUN_TEST(test_defaults_are_filled);
    RUN_TEST(test_defaults_are_valid);

    RUN_TEST(test_validate_rejects_empty_url);
    RUN_TEST(test_validate_rejects_url_without_scheme);
    RUN_TEST(test_validate_rejects_url_without_host);
    RUN_TEST(test_validate_rejects_empty_model);
    RUN_TEST(test_validate_rejects_bad_temperature);
    RUN_TEST(test_validate_accepts_http);

    RUN_TEST(test_parse_full_config);
    RUN_TEST(test_parse_partial_keeps_defaults);
    RUN_TEST(test_parse_unknown_keys_are_skipped);
    RUN_TEST(test_parse_empty_object);
    RUN_TEST(test_parse_rejects_non_object);
    RUN_TEST(test_parse_rejects_broken_json);
    RUN_TEST(test_parse_ignores_out_of_range_numbers);
    RUN_TEST(test_parse_reasoning_effort_as_string);
    RUN_TEST(test_parse_escaped_strings);
    RUN_TEST(test_parse_flat_keys_still_work);
    RUN_TEST(test_parse_unknown_section_key_is_skipped);
    RUN_TEST(test_validate_rejects_bad_power_thresholds);
    RUN_TEST(test_valid_timezone_posix);
    RUN_TEST(test_valid_timezone_rejects_iana_names);
    RUN_TEST(test_validate_rejects_iana_timezone);
    RUN_TEST(test_truncated_utf8_stops_on_char_boundary);

    RUN_TEST(test_write_config_roundtrip);
    RUN_TEST(test_write_config_has_no_secrets);
    RUN_TEST(test_write_config_fails_on_small_buffer);

    RUN_TEST(test_json_escape_quotes_and_slashes);
    RUN_TEST(test_json_escape_newline);
    RUN_TEST(test_json_escape_keeps_utf8);
    RUN_TEST(test_json_escape_control_char);

    return UNITY_END();
}
