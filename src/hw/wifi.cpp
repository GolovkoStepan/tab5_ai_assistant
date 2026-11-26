// Wi-Fi через ESP32-C6: обёртка над Arduino WiFi, неблокирующее подключение.
//
// Плата ESP32-P4 не имеет радио: Wi-Fi живёт на сопроцессоре C6 и поднимается
// по SDIO. Отсюда два следствия, которые видны в коде ниже:
//   * первый вызов WiFi.mode() занимает сотни миллисекунд — это подъём шины;
//   * сканирование асинхронное, иначе UI замирал бы на 2..5 с.

#include "hw/wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>

namespace hw {
namespace {

constexpr const char* kTag = "wifi";

/// Драйвер C6 отдаёт RSSI в dBm; 0 значит «нет данных» и в список не годится.
bool rssi_usable(int32_t rssi)
{
    return rssi < 0;
}

const char* status_name(wl_status_t s)
{
    switch (s) {
    case WL_NO_SHIELD: return "нет модуля";
    case WL_STOPPED: return "остановлено";
    case WL_IDLE_STATUS: return "простаивает";
    case WL_NO_SSID_AVAIL: return "сеть не найдена";
    case WL_SCAN_COMPLETED: return "скан завершён";
    case WL_CONNECTED: return "подключено";
    case WL_CONNECT_FAILED: return "подключение не удалось";
    case WL_CONNECTION_LOST: return "связь потеряна";
    case WL_DISCONNECTED: return "не подключено";
    default: return "неизвестно";
    }
}

}  // namespace

bool WifiManager::begin()
{
    // Сопроцессор C6 может быть не готов сразу после сброса P4: первый
    // WiFi.mode() поднимает SDIO. Повторяем несколько раз, а не падаем.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (WiFi.mode(WIFI_STA)) {
            _begun = true;
            _radio_on = true;
            break;
        }
        ESP_LOGW(kTag, "WiFi.mode(STA) не удался, попытка %d", attempt + 1);
        delay(300);
    }

    if (!_begun) return false;

    // Радио C6 живёт за SDIO и не поддерживает энергосбережение так, как
    // встроенное: без отключения сон даёт обрывы связи в простое.
    WiFi.setSleep(false);

    // Автопереподключение: если сеть пропала, Arduino сам попробует снова.
    // Это спасает от «утром включил — вечером отвалилось».
    WiFi.setAutoReconnect(true);

    update_ip();

    // MAC читать здесь нельзя: esp_hosted поднимается в фоне, и сразу после
    // WiFi.mode() адрес ещё нулевой (00:00:00:00:00:00). Раньше это было не
    // видно только потому, что перед чтением шёл delay + сканирование.
    // Поэтому MAC подхватываем в tick(), когда драйвер отдаст настоящий.
    ESP_LOGI(kTag, "радио готово");
    return true;
}

bool WifiManager::start_scan()
{
    if (!_begun || !_radio_on) return false;

    // Асинхронный скан: метод возвращает -1, а результат забираем по
    // scanComplete() в tick(). Синхронный вариант держал бы UI секунды.
    const int16_t started = WiFi.scanNetworks(/*async=*/true);
    if (started == WIFI_SCAN_FAILED) {
        ESP_LOGW(kTag, "скан не запустился");
        return false;
    }
    return true;
}

int16_t WifiManager::scan_state()
{
    return WiFi.scanComplete();
}

WifiManager::Scan WifiManager::scan() const
{
    // -1 = идёт, -2 = сорвался, >=0 = готово. Коды Arduino, но наружу они не
    // выходят: экран видит только наш enum.
    const int16_t s = const_cast<WifiManager*>(this)->scan_state();
    if (s == WIFI_SCAN_RUNNING) return Scan::Running;
    if (s == WIFI_SCAN_FAILED) return Scan::Failed;
    if (s >= 0) return Scan::Done;
    return Scan::Idle;
}
bool WifiManager::collect(wifi::NetworkList& out)
{
    wifi::clear(out);

    const int16_t found = WiFi.scanComplete();
    if (found < 0) return false;

    for (int16_t i = 0; i < found; ++i) {
        const int32_t rssi = WiFi.RSSI(i);
        if (!rssi_usable(rssi)) continue;

        const String ssid = WiFi.SSID(i);
        const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        wifi::add(out, ssid.c_str(), static_cast<int>(rssi), secure);
    }

    wifi::sort(out);

    ESP_LOGI(kTag, "скан: %d сетей, в списке %u", found, static_cast<unsigned>(out.count));
    return true;
}

void WifiManager::clear_scan()
{
    WiFi.scanDelete();
}

bool WifiManager::start_connect(const char* ssid, const char* password)
{
    // Радио выключено командой — подключение не запустится, и состояние
    // осталось бы «Connecting» до таймаута. Говорим об этом сразу.
    if (!_begun || !_radio_on || !ssid || ssid[0] == '\0') return false;

    // Открытая сеть: пароль пустой — это нормально, а не ошибка ввода.
    // Проверяем длину только если пароль задан.
    const char* pass = password ? password : "";
    if (pass[0] != '\0' && !wifi::password_valid(pass)) {
        ESP_LOGW(kTag, "пароль не подходит по длине или содержит непечатаемые байты");
        set_state(State::Failed);
        return false;
    }

    snprintf(_ssid, sizeof(_ssid), "%s", ssid);
    set_state(State::Connecting);

    // Не блокируемся: begin() с tryConnect запускает подключение и сразу
    // возвращает текущий статус. Ожидание — в tick().
    const wl_status_t status = WiFi.begin(ssid, pass);
    ESP_LOGI(kTag, "подключение к «%s»: %s", ssid, status_name(status));
    return true;
}

void WifiManager::auto_connect(const settings::Secrets& secrets)
{
    if (secrets.wifi_ssid[0] == '\0') return;
    if (_state == State::Connected || _state == State::Connecting) return;

    ESP_LOGI(kTag, "автоподключение к сохранённой сети «%s»", secrets.wifi_ssid);
    start_connect(secrets.wifi_ssid, secrets.wifi_password);
}

void WifiManager::tick()
{
    // MAC подхватываем один раз, когда драйвер C6 его отдаст: сразу после
    // begin() он ещё нулевой, и в логе было 00:00:00:00:00:00.
    if (_mac[0] == '\0') {
        const String mac = WiFi.macAddress();
        if (mac != "00:00:00:00:00:00" && mac.length() > 0) {
            snprintf(_mac, sizeof(_mac), "%s", mac.c_str());
            ESP_LOGI(kTag, "MAC C6: %s", _mac);
        }
    }

    if (_state != State::Connecting) {
        // Статус мог измениться сам: Arduino рвёт связь при потере сигнала и
        // переподключается. Следим, чтобы экран не показывал «подключено»,
        // когда сети уже нет.
        if (_state == State::Connected && WiFi.status() != WL_CONNECTED) {
            ESP_LOGW(kTag, "связь потеряна: %s", status_name(WiFi.status()));
            set_state(State::Idle);
        }
        return;
    }

    const wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        update_ip();
        set_state(State::Connected);
        ESP_LOGI(kTag, "подключено к «%s», адрес %s, RSSI %d", _ssid, _ip, WiFi.RSSI());
        return;
    }

    if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) {
        ESP_LOGW(kTag, "подключение к «%s» отклонено: %s", _ssid, status_name(status));
        set_state(State::Failed);
        return;
    }

    // WL_CONNECTED не наступил за отведённое время: чаще всего неверный пароль,
    // который драйвер молча переспрашивает по кругу.
    if (connect_elapsed_ms() > kConnectTimeoutMs) {
        ESP_LOGW(kTag, "таймаут подключения к «%s» (%s)", _ssid, status_name(status));
        set_state(State::Failed);
    }
}

int WifiManager::rssi() const
{
    // Не подключены — уровня нет. Драйвер в этом состоянии отдаёт 0, и он же
    // попадает в пороги: 0 дБм выглядел бы как отличный сигнал, а на деле
    // связи нет вовсе.
    if (_state != State::Connected) return 0;
    return WiFi.RSSI();
}

void WifiManager::disconnect()
{
    if (!_begun) return;
    WiFi.disconnect(/*wifioff=*/false);
    set_state(State::Idle);
    _ssid[0] = '\0';
    snprintf(_ip, sizeof(_ip), "%s", "—");
}

bool WifiManager::power_off()
{
    if (!_begun) return false;

    // Автопереподключение выключаем ДО остановки: иначе событие «связь
    // потеряна» успело бы запустить новое подключение прямо в момент отказа.
    WiFi.setAutoReconnect(false);
    _radio_on = false;

    // WIFI_OFF останавливает и стек, и драйвер C6 — связь не вернётся сама.
    // disconnect() для этого не годится: он рвёт соединение, а автопере-
    // подключение тут же поднимает новое, и «сети нет» проверить нельзя.
    if (!WiFi.mode(WIFI_OFF)) {
        ESP_LOGW(kTag, "радио не выключилось");
        _radio_on = true;
        WiFi.setAutoReconnect(true);
        return false;
    }

    set_state(State::Idle);
    _ssid[0] = '\0';
    snprintf(_ip, sizeof(_ip), "%s", "—");
    ESP_LOGW(kTag, "радио выключено командой (диагностика)");
    return true;
}

bool WifiManager::power_on(uint32_t* elapsed_ms)
{
    const uint32_t t0 = millis();
    // Повторяем, как в begin(): первый WiFi.mode() после остановки снова
    // поднимает SDIO к C6, и с первого раза это иногда не выходит.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (WiFi.mode(WIFI_STA)) {
            WiFi.setSleep(false);
            WiFi.setAutoReconnect(true);
            _begun = true;
            _radio_on = true;
            update_ip();
            if (elapsed_ms != nullptr) *elapsed_ms = millis() - t0;
            ESP_LOGI(kTag, "радио включено командой");
            return true;
        }
        ESP_LOGW(kTag, "радио не включилось, попытка %d", attempt + 1);
        delay(300);
    }
    if (elapsed_ms != nullptr) *elapsed_ms = millis() - t0;
    return false;
}

void WifiManager::set_state(State s)
{
    if (_state == s) return;
    _state = s;
    if (s == State::Connecting) _connect_started = millis();
    if (s != State::Connected) update_ip();
}

void WifiManager::update_ip()
{
    // Ориентируемся на реальный статус драйвера, а не на _state: в tick()
    // update_ip() вызывается ДО set_state(Connected), и проверка по _state
    // оставляла адрес пустым — в заголовке было «Подключено · —».
    if (WiFi.status() == WL_CONNECTED) {
        const String ip = WiFi.localIP().toString();
        snprintf(_ip, sizeof(_ip), "%s", ip.c_str());
    } else {
        snprintf(_ip, sizeof(_ip), "%s", "—");
    }
}

uint32_t WifiManager::connect_elapsed_ms() const
{
    if (_state != State::Connecting) return 0;
    return millis() - _connect_started;
}

const char* WifiManager::status_text() const
{
    switch (_state) {
    case State::Idle: return "нет сети";
    case State::Connecting: return "подключаюсь…";
    case State::Connected: return _ip;
    case State::Failed: return "не удалось";
    }
    return "неизвестно";
}

}  // namespace hw
