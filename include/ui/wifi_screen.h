#pragma once

// Экран настройки Wi-Fi: список сетей и ввод пароля.
//
// Отдельный класс, а не функции в main.cpp: экран имеет своё состояние
// (выбранная сеть, буфер пароля, идёт подключение) и свой цикл отрисовки.
// Диагностический экран bring-up ему не мешает — они просто переключаются.
//
// Правило проекта: **экран не перерисовывается целиком по событию.** Полный
// кадр — только при входе в режим; нажатие клавиши трогает лишь поле ввода,
// а таймер подключения — только полосу прогресса. Иначе ввод пароля мерцал бы
// на каждой букве, как когда-то мерцал экран диагностики на касании.

#include <stdint.h>

#include "hw/keyboard.h"
#include "hw/settings_store.h"
#include "hw/wifi.h"
#include "util/wifi_list.h"

namespace ui {

class WifiScreen {
public:
    /// Привязать экран к драйверам. Вызывается один раз в setup(): tick()
    /// работает и до первого открытия экрана, чтобы подключение к сохранённой
    /// сети завершалось в фоне.
    void init(hw::WifiManager& wifi, hw::SettingsStore& settings);

    /// Открыть экран: запустить скан и, если сеть сохранена, подключиться.
    void onEnter();

    /// Обработать событие клавиатуры.
    /// @return true, если экран нужно закрыть (возврат к предыдущему)
    bool onKey(const hw::KeyEvent& ev);

    /// Обработать касание. Возвращает true, если касание обработано.
    /// Палец по экрану — естественный способ выбрать сеть: стрелки на
    /// съёмной клавиатуре есть, но целиться в строку пальцем привычнее.
    bool onTouch(int x, int y);

    /// Периодическая работа: состояние скана, результат подключения, таймауты.
    /// Не рисует — только выставляет флаги перерисовки. Вызывать в цикле
    /// **всегда**, а не только когда экран виден.
    void tick();

    /// Перерисовать то, что помечено изменившимся.
    void draw();

    /// Есть ли что перерисовывать.
    bool dirty() const;

    /// Забыть пользовательский ввод при выходе из экрана.
    void onExit();

private:
    /// Что происходит прямо сейчас.
    enum class Mode : uint8_t {
        Scanning,
        List,
        Password,
        Connecting,
    };

    void start_scan();
    void open_password();
    void begin_connect();
    void enter_mode(Mode m);
    void fail(const char* text);
    void save_after_connect();

    /// Обработка ввода в режиме пароля.
    void handle_password_key(const hw::KeyEvent& ev);

    // Отрисовка. Каждая функция чистит ровно свою область.
    void draw_title();
    void draw_list();
    void draw_password_frame();
    void draw_password_field();
    void draw_connecting_text();
    void draw_progress();
    void draw_error();
    void draw_hints();

    /// Нарисовать индикатор уровня сигнала палочками.
    static void draw_signal(int x, int y, int rssi);

    /// Очистить горизонтальную полосу на всю ширину.
    static void clear_band(int y, int h);

    hw::WifiManager* _wifi{nullptr};
    hw::SettingsStore* _settings{nullptr};

    Mode _mode{Mode::Scanning};
    wifi::NetworkList _list{};
    uint8_t _selected{0};

    /// Буфер вводимого пароля. Парольная фраза WPA2 — до 63 байт, «сырой»
    /// PSK — ровно 64 шестнадцатеричных символа. Берём с запасом.
    char _password[68]{};
    size_t _password_len{0};

    /// Чья сеть вводится (для заголовка экрана пароля).
    char _target_ssid[wifi::kMaxSsidLen]{};
    bool _target_secure{true};

    /// Ошибка последнего действия, если была.
    char _error[96]{};

    /// Скан был запущен, но результата ещё нет.
    bool _scan_pending{false};

    /// Подключение запустил пользователь (а не автоподключение). Только в этом
    /// случае пароль из буфера сохраняется в NVS: при автоподключении буфер
    /// пуст, и запись затёрла бы рабочий пароль пустой строкой.
    bool _user_connect{false};

    /// Последнее видимое состояние драйвера: чтобы не перерисовывать заголовок
    /// каждый цикл.
    hw::WifiManager::State _last_state{hw::WifiManager::State::Idle};

    /// Шаг полосы прогресса (сотые доли таймаута), чтобы не перерисовывать её
    /// на каждой итерации цикла.
    long _progress_step{-1};

    /// Флаги перерисовки, чтобы не трогать экран без нужды.
    bool _redraw_all{true};
    bool _redraw_title{false};
    bool _redraw_list{false};
    bool _redraw_field{false};
    bool _redraw_progress{false};
    bool _redraw_error{false};
    bool _redraw_hints{false};
};

}  // namespace ui
