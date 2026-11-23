// Экран настройки Wi-Fi: список сетей, ввод пароля, подключение.
//
// Перерисовка точечная: полный кадр — при смене режима, нажатие в поле пароля
// трогает только поле, таймер подключения — только полосу. Полная перерисовка
// на каждую букву давала бы мерцание, которое в этом проекте уже приходилось
// лечить (HARDWARE_VERIFICATION.md, грабли 5).

#include "ui/wifi_screen.h"

#include <string.h>

#include "app_config.h"
#include "ui/draw.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 12;
constexpr int kTitleY = 14;
constexpr int kListY = 74;
constexpr int kRowH = 40;
constexpr int kHintsY = 686;

/// Область ошибки — между списком и подсказками.
constexpr int kErrorY = kHintsY - 34;

/// Сколько строк помещается между списком и областью ошибки.
constexpr int kVisibleRows = (kErrorY - 8 - kListY) / kRowH;

constexpr uint16_t kColorSelected = 0x0320;  // тёмно-зелёный
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorBad = TFT_ORANGE;
constexpr uint16_t kColorFrame = 0x18E3;  // тёмно-серый

constexpr int kFieldY = kListY + 44;
constexpr int kFieldH = 46;

bool is_char(const hw::KeyEvent& ev)
{
    return ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] != '\0';
}

/// Сколько UTF-8 символов в строке.
size_t utf8_chars(const char* s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++n;
    }
    return n;
}

}  // namespace

void WifiScreen::init(hw::WifiManager& wifi, hw::SettingsStore& settings)
{
    _wifi = &wifi;
    _settings = &settings;
}

void WifiScreen::clear_band(int y, int h)
{
    M5.Display.fillRect(0, y, M5.Display.width(), h, TFT_BLACK);
}

// --- Жизненный цикл ---------------------------------------------------------

void WifiScreen::onEnter()
{
    if (!_wifi) return;

    _error[0] = '\0';
    _password[0] = '\0';
    _password_len = 0;
    _target_ssid[0] = '\0';

    // Скан при входе запускаем всегда: список нужен и когда уже подключены,
    // иначе выбрать другую сеть нельзя.
    //
    // Но не во время подключения: скан занимает радио и может сорвать то,
    // что уже началось. Тогда показываем полосу прогресса — по её завершении
    // экран сам перейдёт в список.
    if (_wifi->connecting()) {
        enter_mode(Mode::Connecting);
        _redraw_all = true;
        return;
    }

    start_scan();
    _redraw_all = true;
}

void WifiScreen::onExit()
{
    _password[0] = '\0';
    _password_len = 0;
    _target_ssid[0] = '\0';
    _error[0] = '\0';
}

void WifiScreen::enter_mode(Mode m)
{
    if (_mode == m) return;
    _mode = m;
    _redraw_all = true;
    _error[0] = '\0';
}

void WifiScreen::start_scan()
{
    if (!_wifi) return;

    _scan_pending = _wifi->start_scan();
    if (!_scan_pending) {
        fail("Wi-Fi недоступен: радио не отвечает");
        return;
    }
    _mode = Mode::Scanning;
    _redraw_all = true;
}

void WifiScreen::fail(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text);
    _redraw_error = true;
    _redraw_all = true;
}

// --- Клавиатура -------------------------------------------------------------

bool WifiScreen::onKey(const hw::KeyEvent& ev)
{
    // Escape всегда возвращает: из пароля и подключения — в список, из
    // списка — наружу. Отменённое подключение драйвер снимает сам по таймауту,
    // а здесь мы просто перестаём ждать.
    if (ev.type == hw::KeyEvent::Type::Special && ev.special == keymap::Special::Escape) {
        if (_mode == Mode::Password || _mode == Mode::Connecting) {
            if (_mode == Mode::Connecting) {
                _wifi->disconnect();
                _user_connect = false;
            }
            _password[0] = '\0';
            _password_len = 0;
            enter_mode(Mode::List);
            return false;
        }
        return true;
    }

    switch (_mode) {
    case Mode::Password: handle_password_key(ev); return false;

    case Mode::List:
    case Mode::Scanning:
        if (ev.type != hw::KeyEvent::Type::Special) return false;

        switch (ev.special) {
        case keymap::Special::Up:
            if (_selected > 0) {
                --_selected;
                _redraw_list = true;
            }
            return false;
        case keymap::Special::Down:
            if (_selected + 1 < _list.count) {
                ++_selected;
                _redraw_list = true;
            }
            return false;
        case keymap::Special::Enter:
        case keymap::Special::Space: open_password(); return false;
        case keymap::Special::Delete:
            // Повторный скан: список мог устареть, сеть могли включить позже.
            start_scan();
            return false;
        default: return false;
        }

    case Mode::Connecting: return false;
    }

    return false;
}

// --- Касание ----------------------------------------------------------------

bool WifiScreen::onTouch(int x, int y)
{
    // В режиме списка касание строки выбирает и подключает: два тапа по одной
    // сети — лишний шаг, а промахнуться по строке проще, чем по маленькой
    // кнопке «далее».
    if (_mode != Mode::List) return false;

    // Нижняя граница — начало области ошибки: там уже не строки списка.
    // Верхнюю проверяем строго: при y чуть выше kListY целочисленное деление
    // отрицательной разности даёт 0 и «выбирало» бы первую строку.
    if (x < 0 || y < kListY || y >= kListY + kVisibleRows * kRowH) return false;

    const int row = (y - kListY) / kRowH;

    int first = 0;
    if (_selected >= kVisibleRows) first = _selected - kVisibleRows + 1;
    const int idx = first + row;
    if (idx < 0 || idx >= static_cast<int>(_list.count)) return false;

    _selected = static_cast<uint8_t>(idx);
    _redraw_list = true;
    open_password();
    return true;
}

void WifiScreen::handle_password_key(const hw::KeyEvent& ev)
{
    if (ev.type == hw::KeyEvent::Type::Special) {
        switch (ev.special) {
        case keymap::Special::Backspace:
            if (_password_len > 0) {
                // Удаляем целый UTF-8 символ, а не один байт: иначе кириллица
                // в пароле рассыпалась бы на обломки.
                do {
                    --_password_len;
                } while (_password_len > 0 && (static_cast<unsigned char>(_password[_password_len]) & 0xC0) == 0x80);
                _password[_password_len] = '\0';
                _redraw_field = true;
            }
            return;
        case keymap::Special::Enter: begin_connect(); return;
        case keymap::Special::Space:
            // Пробел — печатный символ пароля, а не спецклавиша.
            if (_password_len + 1 < sizeof(_password)) {
                _password[_password_len++] = ' ';
                _password[_password_len] = '\0';
                _redraw_field = true;
            }
            return;
        default: return;
        }
    }

    if (!is_char(ev)) return;

    const size_t n = strlen(ev.utf8);
    if (_password_len + n < sizeof(_password)) {
        memcpy(_password + _password_len, ev.utf8, n);
        _password_len += n;
        _password[_password_len] = '\0';
        _redraw_field = true;
    }
}

void WifiScreen::open_password()
{
    if (_list.empty()) return;

    const wifi::Network& net = _list[_selected];
    snprintf(_target_ssid, sizeof(_target_ssid), "%s", net.ssid);
    _target_secure = net.secure;

    _password[0] = '\0';
    _password_len = 0;

    if (!_target_secure) {
        // Открытая сеть: пароля нет, подключаемся сразу.
        begin_connect();
        return;
    }
    enter_mode(Mode::Password);
}

void WifiScreen::begin_connect()
{
    if (!_wifi || !_settings) return;

    if (_target_secure && !wifi::password_valid(_password)) {
        fail("Пароль: от 8 до 63 символов");
        return;
    }

    if (!_wifi->start_connect(_target_ssid, _password)) {
        fail("Не удалось начать подключение");
        return;
    }

    _user_connect = true;
    _progress_step = -1;
    enter_mode(Mode::Connecting);
    _redraw_all = true;
}  /// Пароль сохраняется только после успешного подключения.
///
/// Раньше я записывал его в NVS сразу при вводе — и при опечатке в NVS остался
/// бы неверный пароль, который при следующей загрузке давал бы «не удалось»
/// без всякого объяснения. Правильный порядок: проверили связь, потом
/// сохранили.
void WifiScreen::save_after_connect()
{
    if (!_settings || _target_ssid[0] == '\0') return;

    if (_settings->set_wifi(_target_ssid, _password)) {
        Serial.printf("Wi-Fi: пароль для «%s» сохранён в NVS\n", _target_ssid);
    } else {
        Serial.printf("Wi-Fi: не удалось сохранить пароль для «%s»\n", _target_ssid);
    }
}

// --- Периодика --------------------------------------------------------------

void WifiScreen::tick()
{
    if (!_wifi) return;

    // Скан завершился — забираем результат. Работает независимо от того,
    // виден экран или нет: tick() вызывается всегда.
    if (_scan_pending) {
        switch (_wifi->scan()) {
        case hw::WifiManager::Scan::Done:
            _wifi->collect(_list);
            _wifi->clear_scan();
            _scan_pending = false;
            _mode = Mode::List;
            _selected = 0;
            _redraw_all = true;
            break;
        case hw::WifiManager::Scan::Failed:
            _wifi->clear_scan();
            _scan_pending = false;
            _mode = Mode::List;
            fail("Скан не удался — Del, чтобы повторить");
            break;
        case hw::WifiManager::Scan::Running:
        case hw::WifiManager::Scan::Idle: break;
        }
    }

    // Подключение завершилось тем или иным исходом.
    if (_mode == Mode::Connecting) {
        if (_wifi->connected()) {
            // Пароль сохраняем только если его ввёл пользователь: при
            // автоподключении к сохранённой сети в буфере пусто, и запись
            // затёрла бы рабочий пароль пустой строкой.
            if (_user_connect) save_after_connect();
            _user_connect = false;
            enter_mode(Mode::List);
        } else if (_wifi->state() == hw::WifiManager::State::Failed) {
            // Стираем пароль: если он неверный, повторять его бессмысленно.
            _password[0] = '\0';
            _password_len = 0;
            _user_connect = false;
            enter_mode(Mode::Password);
            fail("Не удалось. Проверьте пароль");
        } else {
            // Идёт подключение: полосу прогресса обновляем не чаще, чем
            // меняется её заливка (сотые доли — 200 шагов на весь таймаут),
            // иначе она перерисовывалась бы на каждой итерации цикла.
            const uint32_t step = _wifi->connect_elapsed_ms() / 100;
            if (step != _progress_step) {
                _progress_step = step;
                _redraw_progress = true;
            }
        }
    }

    // Строка состояния в заголовке меняется сама по себе: Arduino рвёт связь
    // при потере сигнала и переподключается. Следим, чтобы экран не врал.
    if (_wifi->state() != _last_state) {
        _last_state = _wifi->state();
        _redraw_title = true;
        _redraw_all = true;
    }
}

bool WifiScreen::dirty() const
{
    return _redraw_all || _redraw_title || _redraw_list || _redraw_field || _redraw_progress || _redraw_error ||
           _redraw_hints;
}

// --- Отрисовка --------------------------------------------------------------

void WifiScreen::draw()
{
    if (_redraw_all) {
        _redraw_all = false;
        _redraw_title = _redraw_list = _redraw_field = false;
        _redraw_progress = _redraw_error = _redraw_hints = false;

        M5.Display.fillScreen(TFT_BLACK);
        draw_title();
        draw_hints();

        switch (_mode) {
        case Mode::Scanning: ui::text("Ищу сети…", kMargin, kListY + 20, TFT_WHITE); break;
        case Mode::List: draw_list(); break;
        case Mode::Password:
            draw_password_frame();
            draw_password_field();
            break;
        case Mode::Connecting:
            draw_connecting_text();
            draw_progress();
            break;
        }

        draw_error();
        return;
    }

    if (_redraw_title) {
        _redraw_title = false;
        clear_band(kTitleY, 32);
        draw_title();
    }
    if (_redraw_list) {
        _redraw_list = false;
        clear_band(kListY - 4, kVisibleRows * kRowH + 8);
        draw_list();
    }
    if (_redraw_field) {
        _redraw_field = false;
        draw_password_field();
    }
    if (_redraw_progress) {
        _redraw_progress = false;
        draw_progress();
    }
    if (_redraw_error) {
        _redraw_error = false;
        draw_error();
    }
    if (_redraw_hints) {
        _redraw_hints = false;
        draw_hints();
    }
}

void WifiScreen::draw_title()
{
    ui::text("Настройка Wi-Fi", kMargin, kTitleY, TFT_WHITE);

    if (!_wifi) return;

    char right[128];
    switch (_wifi->state()) {
    case hw::WifiManager::State::Connected:
        snprintf(right, sizeof(right), "Подключено: %s · %s", _wifi->ssid(), _wifi->ip_text());
        break;
    case hw::WifiManager::State::Connecting:
        snprintf(right, sizeof(right), "Подключаюсь к «%s»…", _wifi->ssid());
        break;
    case hw::WifiManager::State::Failed: snprintf(right, sizeof(right), "Не подключено"); break;
    case hw::WifiManager::State::Idle: snprintf(right, sizeof(right), "Нет сети"); break;
    }

    const uint16_t color = _wifi->connected() ? TFT_GREEN : TFT_CYAN;
    ui::text_right(right, M5.Display.width() - kMargin, kTitleY, color);
}

void WifiScreen::draw_signal(int x, int y, int rssi)
{
    const uint8_t level = wifi::signal_level(rssi);
    if (level == 0) {
        ui::text("×", x, y - 3, TFT_DARKGRAY);
        return;
    }

    for (uint8_t i = 0; i < 4; ++i) {
        const int h = 4 + i * 4;
        const int bx = x + i * 5;
        const int by = y + 16 - h;
        const uint16_t color = i < level ? TFT_GREEN : kColorFrame;
        M5.Display.fillRect(bx, by, 4, h, color);
    }
}

void WifiScreen::draw_list()
{
    if (_list.empty()) {
        ui::text("Сетей не найдено", kMargin, kListY + 20, kColorDim);
        ui::text("Del — искать заново", kMargin, kListY + 48, TFT_DARKGRAY);
        return;
    }

    // Прокрутка окном: _selected всегда внутри видимого диапазона.
    int first = 0;
    if (_selected >= kVisibleRows) first = _selected - kVisibleRows + 1;

    for (int i = 0; i < kVisibleRows; ++i) {
        const int idx = first + i;
        if (idx >= static_cast<int>(_list.count)) break;

        const wifi::Network& net = _list[idx];
        const bool sel = (idx == _selected);
        const int y = kListY + i * kRowH;

        if (sel) {
            M5.Display.fillRect(kMargin - 6, y - 6, M5.Display.width() - 2 * kMargin + 12, kRowH - 2, kColorSelected);
        }

        ui::text(sel ? ">" : " ", kMargin, y, sel ? TFT_YELLOW : TFT_DARKGRAY);
        ui::text(net.ssid, kMargin + 20, y, TFT_WHITE);

        // Справа: защита и уровень. Столбцы фиксированные, чтобы строки не
        // «плясали».
        char tail[48];
        snprintf(tail, sizeof(tail), "%s  %d дБм", wifi::security_name(net.secure), net.rssi);
        ui::text_right(tail, M5.Display.width() - kMargin - 40, y, net.secure ? kColorDim : TFT_DARKGRAY);

        draw_signal(M5.Display.width() - kMargin - 26, y, net.rssi);
    }

    // Полоса прокрутки: видно, что список длиннее окна.
    if (_list.count > kVisibleRows) {
        const int track_h = kVisibleRows * kRowH;
        const int x = M5.Display.width() - 5;
        M5.Display.fillRect(x, kListY, 3, track_h, kColorFrame);
        const int thumb_h = track_h * kVisibleRows / _list.count;
        const int thumb_y = kListY + (track_h - thumb_h) * first / (_list.count - kVisibleRows);
        M5.Display.fillRect(x, thumb_y, 3, thumb_h, TFT_LIGHTGRAY);
    }
}

void WifiScreen::draw_password_frame()
{
    char head[96];
    snprintf(head, sizeof(head), "Пароль для «%s»", _target_ssid);
    ui::text(head, kMargin, kListY, TFT_WHITE);

    // Рамка поля ввода.
    M5.Display.drawRect(kMargin, kFieldY, M5.Display.width() - 2 * kMargin, kFieldH, kColorDim);

    ui::text(
        "Enter — подключиться · Esc — назад · Ctrl+Space — раскладка", kMargin, kFieldY + kFieldH + 16, TFT_DARKGRAY);
    if (_target_secure) {
        ui::text("Пароль не отображается: 8–63 символа WPA2", kMargin, kFieldY + kFieldH + 44, kColorFrame);
    }
}

void WifiScreen::draw_password_field()
{
    // Внутренняя область поля.
    M5.Display.fillRect(kMargin + 2, kFieldY + 2, M5.Display.width() - 2 * kMargin - 4, kFieldH - 4, TFT_BLACK);

    // Звёздочки по числу символов: пароль может быть кириллицей, и по байтам
    // он «длиннее», чем выглядит.
    const size_t chars = utf8_chars(_password, _password_len);
    constexpr size_t kMaskTail = 56;  // влезает в поле при 16 px
    const size_t show = chars < kMaskTail ? chars : kMaskTail;

    char shown[kMaskTail + 4] = {};
    for (size_t i = 0; i < show; ++i)
        shown[i] = '*';
    if (chars > kMaskTail) shown[0] = '…';

    ui::text(shown, kMargin + 10, kFieldY + 13, TFT_WHITE);

    // Курсор: вертикальная черта за последней звёздочкой.
    const int caret_x = kMargin + 10 + static_cast<int>(show) * 10;
    M5.Display.fillRect(caret_x, kFieldY + 9, 2, 26, TFT_YELLOW);
}

void WifiScreen::draw_connecting_text()
{
    // Имя берём из драйвера, а не из _target_ssid: экран мог открыться во время
    // автоподключения, когда _target_ssid пуст, и заголовок был бы «Подключаюсь
    // к «»».
    const char* name = (_target_ssid[0] != '\0') ? _target_ssid : (_wifi ? _wifi->ssid() : "");

    char head[128];
    snprintf(head, sizeof(head), "Подключаюсь к «%s»", name);
    ui::text(head, kMargin, kListY, TFT_WHITE);

    char sub[96];
    const uint32_t elapsed = _wifi ? _wifi->connect_elapsed_ms() : 0;
    snprintf(sub, sizeof(sub), "%.1f с из %u с · Esc — отменить", elapsed / 1000.0f,
        static_cast<unsigned>(hw::WifiManager::kConnectTimeoutMs / 1000));
    ui::text(sub, kMargin, kListY + 34, kColorDim);
}

void WifiScreen::draw_progress()
{
    const int bar_y = kListY + 74;
    const int bar_w = M5.Display.width() - 2 * kMargin;

    // Полоса перерисовывается целиком: она узкая, 8 px, и это дешевле, чем
    // считать изменившуюся часть.
    M5.Display.fillRect(kMargin, bar_y, bar_w, 8, TFT_BLACK);
    M5.Display.drawRect(kMargin, bar_y, bar_w, 8, kColorFrame);

    const uint32_t elapsed = _wifi ? _wifi->connect_elapsed_ms() : 0;
    uint32_t filled = bar_w * elapsed / hw::WifiManager::kConnectTimeoutMs;
    if (filled > static_cast<uint32_t>(bar_w)) filled = bar_w;
    if (filled > 2) M5.Display.fillRect(kMargin + 1, bar_y + 1, static_cast<int>(filled) - 2, 6, TFT_GREEN);
}

void WifiScreen::draw_error()
{
    clear_band(kErrorY, 26);
    if (_error[0]) ui::text(_error, kMargin, kErrorY, kColorBad);
}

void WifiScreen::draw_hints()
{
    const char* hint = "";
    switch (_mode) {
    case Mode::Scanning: hint = "Идёт скан сетей…"; break;
    case Mode::List: hint = "↑ ↓ — выбор · Enter — подключиться · Del — искать заново · Esc — назад"; break;
    case Mode::Password: hint = "Введите пароль · Enter — подключиться · Esc — назад"; break;
    case Mode::Connecting: hint = "Подключение… · Esc — отменить"; break;
    }

    M5.Display.drawFastHLine(0, kHintsY - 8, M5.Display.width(), kColorFrame);
    ui::text(hint, kMargin, kHintsY, TFT_DARKGRAY);
}

}  // namespace ui
