#pragma once

// Wi-Fi через ESP32-C6: обёртка над Arduino WiFi с неблокирующим подключением.
//
// Почему неблокирующее: `WiFi.begin()` с ожиданием держит UI-задачу до 20 с.
// На экране это выглядит как зависшая плата, а на самом деле идёт DHCP.
// Поэтому подключение разбито на «начать» и «проверять в tick()»: интерфейс
// продолжает отрисовываться и показывает «подключаюсь…».

#include <stdint.h>

#include "util/settings.h"
#include "util/wifi_list.h"

namespace hw {

class WifiManager {
public:
    /// Состояние подключения.
    enum class State : uint8_t {
        Idle,        ///< не подключено, ничего не происходит
        Connecting,  ///< идёт подключение
        Connected,   ///< подключено, есть адрес
        Failed,      ///< подключение не удалось (таймаут или отказ)
    };

    /// Инициализация: режим STA, автопереподключение, диапазон 2.4 ГГц.
    /// Wi-Fi на плате живёт на сопроцессоре C6, поэтому begin() заметно
    /// небыстрый — первый вызов поднимает SDIO.
    bool begin();

    // --- Сканирование ---

    /// Состояние скана. Своё перечисление, а не коды Arduino: экран не должен
    /// включать WiFi.h ради константы -1.
    enum class Scan : uint8_t {
        Idle,     ///< не запускался
        Running,  ///< идёт
        Done,     ///< завершён, результаты готовы
        Failed,   ///< сорвался
    };

    /// Запустить асинхронное сканирование.
    /// @return false, если запустить не удалось (радио не поднялось)
    bool start_scan();

    /// Состояние сканирования.
    Scan scan() const;

    /// Переложить результаты в список. Вызывать при Scan::Done,
    /// затем clear_scan().
    bool collect(wifi::NetworkList& out);

    /// Освободить результаты скана.
    void clear_scan();

    // --- Подключение ---

    /// Начать подключение. Возвращает false сразу, если параметры не годятся.
    /// Дальше состояние проверяется через state() и tick().
    bool start_connect(const char* ssid, const char* password);

    /// Подключиться по сохранённым учётным данным, если они есть и сеть не
    /// подключена. Ничего не делает, если данных нет.
    void auto_connect(const settings::Secrets& secrets);

    /// Вызывать в цикле: снимает состояние Wi-Fi и ловит таймаут.
    void tick();

    /// Отключиться и вернуться в Idle.
    void disconnect();

    // --- Радио целиком (диагностика) ---

    /// Выключить радио: `WiFi.mode(WIFI_OFF)` останавливает стек, и
    /// автопереподключение уже не поднимет связь само.
    ///
    /// Зачем это нужно, если есть disconnect(): тот лишь рвёт текущее
    /// соединение, а `setAutoReconnect(true)` тут же начинает новое. Для
    /// проверки «что будет без интернета» нужен именно выключенный драйвер.
    /// @return false, если радио не поднималось вовсе
    bool power_off();

    /// Включить радио обратно. Подключение к сохранённой сети запускает
    /// вызывающий (`auto_connect`) — у драйвера нет учётных данных.
    /// @param[out] elapsed_ms сколько занял подъём драйвера (может быть nullptr)
    /// @return false, если драйвер не поднялся
    bool power_on(uint32_t* elapsed_ms = nullptr);

    /// Радио включено. Отличается от connected(): связь может быть, а радио
    /// выключено командой — тогда «нет сети» это ожидаемое состояние.
    bool radio_on() const { return _radio_on; }

    State state() const { return _state; }
    bool connected() const { return _state == State::Connected; }
    bool connecting() const { return _state == State::Connecting; }

    /// Имя сети, к которой подключены (или пытались).
    const char* ssid() const { return _ssid; }

    /// Адрес в виде строки. "—", если адреса нет.
    const char* ip_text() const { return _ip; }

    /// Текущий RSSI, дБм. 0 — связи нет (так его отдаёт драйвер).
    ///
    /// Читается напрямую из WiFi, а не кэшируется в tick(): значение меняется
    /// от помех и расстояния, и трею нужен свежий уровень сигнала, а не
    /// запомненный при подключении.
    int rssi() const;

    /// MAC-адрес радио C6. Пустая строка, пока драйвер не отдал настоящий:
    /// esp_hosted поднимается в фоне, и сразу после begin() адрес нулевой.
    const char* mac_text() const { return _mac; }

    /// Человеческое описание состояния для экрана и лога.
    const char* status_text() const;

    /// Сколько миллисекунд идёт текущее подключение.
    uint32_t connect_elapsed_ms() const;

    /// Таймаут подключения, после которого состояние становится Failed.
    static constexpr uint32_t kConnectTimeoutMs = 20000;

private:
    /// Коды состояния скана Arduino (-1 идёт, -2 ошибка, >=0 готово).
    /// Внутреннее: наружу отдаётся только Scan.
    int16_t scan_state();

    void set_state(State s);
    void update_ip();

    State _state{State::Idle};
    uint32_t _connect_started{0};
    char _ssid[wifi::kMaxSsidLen]{};
    char _ip[16]{"—"};
    char _mac[18]{};
    bool _begun{false};
    /// Радио включено. Сбрасывается power_off() и поднимается power_on().
    bool _radio_on{false};
};

}  // namespace hw
