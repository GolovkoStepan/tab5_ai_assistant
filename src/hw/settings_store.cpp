// Хранилище настроек: NVS для секретов, файл на SD для остального.
//
// Используем Arduino Preferences, а не прямые вызовы nvs_*: библиотека уже
// входит в core, обходит ручное управление handle и не требует тянуть
// IDF-заголовки nvs_flash.
//
// Запись файла идёт через временный файл с последующим переименованием: если
// питание пропадёт посреди записи, старый конфиг останется целым, а не
// превратится в обрезанный JSON.

#include "hw/settings_store.h"

#include <Preferences.h>
#include <SD_MMC.h>
#include <esp_log.h>

#include "app_config.h"

namespace hw {
namespace {

constexpr const char* kTag = "settings";

/// Пространство имён NVS. Отдельное от системного, чтобы не пересечься
/// с Wi-Fi-хранилищем Arduino.
constexpr const char* kNvsNamespace = "tab5cfg";

constexpr const char* kKeySsid = "wifi_ssid";
constexpr const char* kKeyPassword = "wifi_pass";
constexpr const char* kKeyApiKey = "api_key";

/// Буфер под файл конфига. Настройки плоские, 1 КБ хватает с большим запасом.
constexpr size_t kFileBuffer = 1024;

/// Временный файл для атомарной записи. Путь от корня SD, как и kConfigPath.
constexpr const char* kTempPath = "/config.json.tmp";

bool read_file(const char* path, char* buf, size_t buf_len, size_t* out_len)
{
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    const size_t size = f.size();
    if (size == 0 || size >= buf_len) {
        f.close();
        return false;
    }

    const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), size);
    f.close();
    if (n != size) return false;

    buf[n] = '\0';
    if (out_len) *out_len = n;
    return true;
}

}  // namespace

bool SettingsStore::begin()
{
    settings::make_default_config(_config);
    _secrets = settings::Secrets{};

    load_from_nvs();

    bool file_ok = load_from_file();

    // Даже если файл был, значения проверяем: битая правка руками не должна
    // привести к работе с адресом без схемы.
    char err[128];
    if (!settings::validate(_config, err, sizeof(err))) {
        ESP_LOGW(kTag, "конфиг некорректен (%s) — сброшен к значениям по умолчанию", err);
        settings::make_default_config(_config);
        file_ok = false;
    }

    // Файла нет или он битый: создаём эталонный, чтобы его можно было править
    // руками и бэкапить. Ошибку записи не считаем фатальной — работаем
    // на значениях по умолчанию (например, если SD не смонтирована).
    if (!file_ok) {
        char write_err[128];
        if (save_to_file(write_err, sizeof(write_err))) {
            ESP_LOGI(kTag, "создан config.json со значениями по умолчанию");
        } else {
            ESP_LOGW(kTag, "config.json не прочитан и не создан (%s) — значения по умолчанию", write_err);
        }
    }

    ESP_LOGI(kTag, "настройки: model=%s, tz=%s, Wi-Fi %s, ключ API %s", _config.api_model, _config.timezone,
        has_wifi() ? "есть" : "нет", has_api_key() ? "есть" : "нет");
    return file_ok;
}

bool SettingsStore::load_from_nvs()
{
    Preferences prefs;
    // Открываем на запись, а не только на чтение: если пространства ещё нет
    // (первый запуск, ни одного секрета не сохранено), read-only открытие
    // логирует ошибку NOT_FOUND уровня E и выглядит как сбой. Создание пустого
    // пространства безвредно и убирает ложную тревогу из лога.
    if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return false;

    auto read_str = [&](const char* key, char* dst, size_t dst_len) {
        // isKey() проверяем обязательно: getString() на отсутствующем ключе
        // пишет ошибку уровня E в лог, и штатный «секретов ещё нет» выглядит
        // как сбой NVS.
        if (!prefs.isKey(key)) {
            dst[0] = '\0';
            return;
        }
        const String value = prefs.getString(key, "");
        snprintf(dst, dst_len, "%s", value.c_str());
    };

    read_str(kKeySsid, _secrets.wifi_ssid, sizeof(_secrets.wifi_ssid));
    read_str(kKeyPassword, _secrets.wifi_password, sizeof(_secrets.wifi_password));
    read_str(kKeyApiKey, _secrets.api_key, sizeof(_secrets.api_key));

    prefs.end();
    return true;
}

bool SettingsStore::save_to_nvs()
{
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return false;

    // Пустое значение означает «секрета нет»: такой ключ надо удалить, а не
    // записать. putString("") возвращает 0, и проверка «> 0» принимала бы
    // штатный сброс настроек за ошибку записи.
    auto put_or_remove = [&](const char* key, const char* value) {
        if (value[0] == '\0') {
            prefs.remove(key);
            return true;
        }
        return prefs.putString(key, value) > 0;
    };

    const bool ok = put_or_remove(kKeySsid, _secrets.wifi_ssid) &&
                    put_or_remove(kKeyPassword, _secrets.wifi_password) && put_or_remove(kKeyApiKey, _secrets.api_key);

    prefs.end();
    return ok;
}

bool SettingsStore::load_from_file()
{
    char buf[kFileBuffer];
    size_t len = 0;
    if (!read_file(cfg::kConfigPath, buf, sizeof(buf), &len)) return false;

    char err[128];
    if (!settings::parse_config_json(buf, _config, err, sizeof(err))) {
        ESP_LOGW(kTag, "ошибка разбора config.json: %s", err);
        return false;
    }
    return true;
}

bool SettingsStore::save_to_file(char* err, size_t err_len)
{
    char buf[kFileBuffer];
    const size_t n = settings::write_config_json(_config, buf, sizeof(buf));
    if (n == 0) {
        snprintf(err, err_len, "конфиг не помещается в буфер (%u байт)", static_cast<unsigned>(sizeof(buf)));
        return false;
    }

    // Сначала во временный файл: обрыв питания посреди записи не испортит
    // рабочий конфиг.
    File f = SD_MMC.open(kTempPath, FILE_WRITE);
    if (!f) {
        snprintf(err, err_len, "не удалось создать %s", kTempPath);
        return false;
    }

    const size_t written = f.write(reinterpret_cast<const uint8_t*>(buf), n);
    f.close();
    if (written != n) {
        SD_MMC.remove(kTempPath);
        snprintf(err, err_len, "записано %u из %u байт", static_cast<unsigned>(written), static_cast<unsigned>(n));
        return false;
    }

    SD_MMC.remove(cfg::kConfigPath);
    if (!SD_MMC.rename(kTempPath, cfg::kConfigPath)) {
        SD_MMC.remove(kTempPath);
        snprintf(err, err_len, "не удалось заменить %s", cfg::kConfigPath);
        return false;
    }
    return true;
}

bool SettingsStore::save(char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';

    char local_err[128];
    if (!settings::validate(_config, local_err, sizeof(local_err))) {
        snprintf(err, err_len, "%s", local_err);
        return false;
    }

    if (!save_to_nvs()) {
        snprintf(err, err_len, "%s", "не удалось записать секреты в NVS");
        return false;
    }
    return save_to_file(err, err_len);
}

bool SettingsStore::reset_all(char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';

    settings::make_default_config(_config);
    _secrets = settings::Secrets{};

    Preferences prefs;
    if (prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }

    return save_to_file(err, err_len);
}

bool SettingsStore::set_wifi(const char* ssid, const char* password)
{
    if (ssid == nullptr) return false;
    snprintf(_secrets.wifi_ssid, sizeof(_secrets.wifi_ssid), "%s", ssid);
    snprintf(_secrets.wifi_password, sizeof(_secrets.wifi_password), "%s", password ? password : "");
    return save_to_nvs();
}

bool SettingsStore::set_api_key(const char* key)
{
    if (key == nullptr) return false;
    snprintf(_secrets.api_key, sizeof(_secrets.api_key), "%s", key);
    return save_to_nvs();
}

bool SettingsStore::set_layout_russian(bool russian, char* err, size_t err_len)
{
    if (_config.layout_russian == russian) return true;
    _config.layout_russian = russian;

    // Пишем только файл, NVS не трогаем: раскладка — не секрет, а лишняя
    // запись в NVS изнашивает раздел.
    return save_to_file(err, err_len);
}

bool SettingsStore::set_timezone(const char* tz, char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';

    if (!settings::valid_timezone(tz)) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "таймзона должна быть в формате POSIX: YEKT-5, MSK-3");
        return false;
    }
    if (strlen(tz) >= sizeof(_config.timezone)) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "таймзона длиннее 31 символа — не помещается");
        return false;
    }

    // Сначала в память: если запись на карту не удастся, _config и файл не
    // разойдутся — обе останутся в прежнем состоянии.
    char before[sizeof(_config.timezone)];
    memcpy(before, _config.timezone, sizeof(before));
    snprintf(_config.timezone, sizeof(_config.timezone), "%s", tz);

    if (!save_to_file(err, err_len)) {
        memcpy(_config.timezone, before, sizeof(before));
        return false;
    }
    return true;
}

bool SettingsStore::set_api_settings(const settings::Config& cfg, char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';

    char local_err[128];
    if (!settings::validate(cfg, local_err, sizeof(local_err))) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", local_err);
        return false;
    }

    // Раскладку и яркость экран настроек API не меняет, а они относятся к
    // текущему состоянию устройства: берём их из СВОИХ настроек, а не из
    // черновика экрана. Иначе сохранение адреса вернуло бы яркость к той, что
    // была при входе в экран.
    settings::Config next = cfg;
    next.layout_russian = _config.layout_russian;
    next.brightness = _config.brightness;
    next.screen_timeout_min = _config.screen_timeout_min;
    next.battery_warn_percent = _config.battery_warn_percent;
    next.battery_critical_percent = _config.battery_critical_percent;
    next.battery_shutdown_percent = _config.battery_shutdown_percent;

    // Сначала в память — иначе при отказе записи экран показывал бы сохранённое,
    // а на карте осталось бы старое.
    _config = next;

    if (!save_to_file(err, err_len)) return false;
    return true;
}

bool SettingsStore::set_device_settings(const settings::Config& cfg, char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';

    char local_err[128];
    if (!settings::validate(cfg, local_err, sizeof(local_err))) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", local_err);
        return false;
    }

    // Поля API экран параметров не меняет: берём их из СВОИХ настроек, а не из
    // черновика. Иначе сохранение яркости вернуло бы адрес API и модель к тем,
    // что были при входе на экран.
    settings::Config next = cfg;
    snprintf(next.api_base_url, sizeof(next.api_base_url), "%s", _config.api_base_url);
    snprintf(next.api_model, sizeof(next.api_model), "%s", _config.api_model);
    next.temperature_tenths = _config.temperature_tenths;
    next.reasoning_effort = _config.reasoning_effort;
    next.search_enabled = _config.search_enabled;
    next.search_max_results = _config.search_max_results;

    // Сначала в память — иначе при отказе записи экран показывал бы сохранённое,
    // а на карте осталось бы старое.
    _config = next;

    if (!save_to_file(err, err_len)) return false;
    return true;
}

}  // namespace hw
