#pragma once

// Хранилище настроек на устройстве: секреты в NVS, остальное на SD.
//
// Логика — в util/settings.h (чистые функции с тестами на хосте). Здесь только
// работа с железом: NVS через Preferences и файл /sdcard/config.json.
//
// Почему секреты в NVS: SD-карту легко вынуть и прочитать на компьютере,
// поэтому Wi-Fi-пароль и ключ API не должны туда попадать (Р-006).

#include "util/settings.h"

namespace hw {

class SettingsStore {
public:
    /// Загрузить настройки. Секреты — из NVS, остальное — из config.json.
    /// Отсутствующий или битый файл не считается ошибкой: берутся значения
    /// по умолчанию, а причина пишется в журнал.
    /// @return true, если config.json прочитан и разобран без ошибок
    bool begin();

    /// Текущие настройки.
    const settings::Config& config() const { return _config; }
    const settings::Secrets& secrets() const { return _secrets; }

    /// Сохранить все настройки: секреты в NVS, остальное в файл.
    /// @param err буфер для причины отказа
    bool save(char* err, size_t err_len);

    /// Сбросить к значениям по умолчанию: файл перезаписывается, секреты
    /// стираются из NVS.
    bool reset_all(char* err, size_t err_len);

    /// Есть ли сохранённые учётные данные Wi-Fi.
    bool has_wifi() const { return _secrets.wifi_ssid[0] != '\0'; }

    /// Задан ли ключ API.
    bool has_api_key() const { return _secrets.api_key[0] != '\0'; }

    /// Записать секреты Wi-Fi и сохранить в NVS.
    bool set_wifi(const char* ssid, const char* password);

    /// Записать ключ API и сохранить в NVS.
    bool set_api_key(const char* key);

    /// Проверить текущий конфиг на корректность (см. settings::validate).
    bool validate(char* err, size_t err_len) const { return settings::validate(_config, err, err_len); }

private:
    bool load_from_nvs();
    bool save_to_nvs();
    bool load_from_file();
    bool save_to_file(char* err, size_t err_len);

    settings::Config _config;
    settings::Secrets _secrets;
};

}  // namespace hw
