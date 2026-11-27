// Экран «Параметры»: яркость, подсветка, раскладка, пороги разряда, таймзона.
//
// Правки идут в черновик и уходят в config.json по Ctrl+S. Исключение —
// яркость: она применяется сразу, ещё до сохранения. Иначе вечером «убавить
// яркость» было бы невидимым действием: экран слепит, а человек смотрит на
// цифры. При выходе без сохранения main возвращает прежнюю яркость.
//
// Шаги и ограничения — в util/device_settings (тесты на хосте). Здесь только
// состояния, отрисовка и запись файла.

#include "ui/device_settings_screen.h"

#include <stdio.h>
#include <string.h>

#include "ui/draw.h"
#include "ui/tray.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 14;
constexpr int kTitleY = 12;
constexpr int kRowsY = 62;
constexpr int kRowH = 58;
constexpr int kLabelY = 5;
constexpr int kValueY = 26;
/// Полоса под сообщение: ниже последней строки формы (62 + 7·58 = 468).
constexpr int kErrorY = 490;
/// Нижняя граница формы: до неё чистим строки.
constexpr int kRowsBottom = kRowsY + static_cast<int>(DeviceSettingsScreen::kRowCount) * kRowH;

constexpr uint16_t kColorSelected = 0x0320;  // тёмно-зелёный
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorBad = TFT_ORANGE;

/// Буфер подписи значения. Статический: на стеке задачи интерфейса уже живут
/// другие буферы, а функция вызывается на каждую перерисовку строки.
char g_value[160];

bool is_char(const hw::KeyEvent& ev)
{
    return ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] != '\0';
}

bool is_ctrl_s(const hw::KeyEvent& ev)
{
    // Ctrl+S, в том числе в русской раскладке (там на этой клавише «ы»).
    return ev.ctrl && is_char(ev) && (strcmp(ev.utf8, "s") == 0 || strcmp(ev.utf8, "ы") == 0);
}

/// Ступень яркости словами: по числу не видно, «ярко это или терпимо».
const char* brightness_note(uint8_t value)
{
    if (value <= 32) return "едва видно";
    if (value <= 80) return "для темноты";
    if (value <= 160) return "обычная";
    if (value <= 220) return "ярко";
    return "на солнце";
}

const char* timeout_note(uint8_t minutes)
{
    if (minutes == 0) return "не гаснет";
    if (minutes <= 2) return "быстро гаснет";
    if (minutes <= 10) return "обычно";
    return "долго горит";
}

}  // namespace

void DeviceSettingsScreen::init(hw::SettingsStore& settings)
{
    _settings = &settings;
}

void DeviceSettingsScreen::onEnter()
{
    _selected = Row::Brightness;
    _error[0] = '\0';
    _notice_ok = false;
    _applied = false;

    if (_settings != nullptr) _draft = _settings->config();

    _redraw_all = true;
}

void DeviceSettingsScreen::onExit()
{
    // Несохранённый черновик не должен пережить уход с экрана: иначе
    // следующее открытие показало бы чужие правки, которые никто не сохранял.
    if (_settings != nullptr) _draft = _settings->config();
    _error[0] = '\0';
    _notice_ok = false;
    _applied = false;
}

bool DeviceSettingsScreen::take_applied()
{
    const bool was = _applied;
    _applied = false;
    return was;
}

bool DeviceSettingsScreen::unsaved() const
{
    if (_settings == nullptr) return false;
    const settings::Config& saved = _settings->config();
    if (_draft.brightness != saved.brightness) return true;
    if (_draft.screen_timeout_min != saved.screen_timeout_min) return true;
    if (_draft.layout_russian != saved.layout_russian) return true;
    if (_draft.battery_warn_percent != saved.battery_warn_percent) return true;
    if (_draft.battery_critical_percent != saved.battery_critical_percent) return true;
    if (_draft.battery_shutdown_percent != saved.battery_shutdown_percent) return true;
    if (strcmp(_draft.timezone, saved.timezone) != 0) return true;
    return false;
}

// --- Клавиатура -------------------------------------------------------------

bool DeviceSettingsScreen::onKey(const hw::KeyEvent& ev)
{
    // Ctrl+S сохраняет из любого состояния.
    if (is_ctrl_s(ev)) {
        save();
        return false;
    }

    if (ev.type != hw::KeyEvent::Type::Special) return false;

    switch (ev.special) {
    case keymap::Special::Escape: return true;  // выйти

    case keymap::Special::Up:
        if (static_cast<uint8_t>(_selected) > 0) {
            _selected = static_cast<Row>(static_cast<uint8_t>(_selected) - 1);
            _redraw_rows = true;
        }
        return false;

    case keymap::Special::Down:
        if (static_cast<size_t>(static_cast<uint8_t>(_selected)) + 1 < kRowCount) {
            _selected = static_cast<Row>(static_cast<uint8_t>(_selected) + 1);
            _redraw_rows = true;
        }
        return false;

    // ← → и Enter — шаг. Отдельного режима правки нет: все семь значений
    // перебираются, текстом здесь набирать нечего.
    case keymap::Special::Left: adjust(_selected, -1); return false;
    case keymap::Special::Right:
    case keymap::Special::Enter:
    case keymap::Special::Space: adjust(_selected, 1); return false;

    default: return false;
    }
}

bool DeviceSettingsScreen::onTouch(int x, int y)
{
    (void)x;
    if (y < kRowsY || y >= kRowsBottom) return false;

    const int index = (y - kRowsY) / kRowH;
    const Row r = static_cast<Row>(index);

    // Повторный тап по выбранной строке — шаг вперёд: на плате без мыши это
    // единственный способ менять значение касанием.
    if (r == _selected) {
        adjust(r, 1);
        return true;
    }

    _selected = r;
    _redraw_rows = true;
    return true;
}

// --- Значения ---------------------------------------------------------------

void DeviceSettingsScreen::row_value(Row r, char* out, size_t out_len) const
{
    if (out == nullptr || out_len == 0) return;
    out[0] = '\0';

    switch (r) {
    case Row::Brightness:
        snprintf(out, out_len, "%u%% — %s", static_cast<unsigned>(_draft.brightness * 100 / 255),
            brightness_note(_draft.brightness));
        return;
    case Row::Timeout:
        if (_draft.screen_timeout_min == 0) {
            snprintf(out, out_len, "не гаснет");
        } else {
            snprintf(out, out_len, "%u мин — %s", static_cast<unsigned>(_draft.screen_timeout_min),
                timeout_note(_draft.screen_timeout_min));
        }
        return;
    case Row::Layout: snprintf(out, out_len, "%s", _draft.layout_russian ? "ЙЦУКЕН" : "латиница"); return;
    case Row::Warn:
    case Row::Critical:
    case Row::Shutdown: {
        const devsettings::Battery which = r == Row::Warn       ? devsettings::Battery::Warn
                                           : r == Row::Critical ? devsettings::Battery::Critical
                                                                : devsettings::Battery::Shutdown;
        snprintf(out, out_len, "%u%%", static_cast<unsigned>(devsettings::battery_value(_draft, which)));
        return;
    }
    case Row::Timezone: {
        // Показываем подпись из списка, а не POSIX-строку: «Екатеринбург
        // (UTC+5)» понятно, «YEKT-5» — нет.
        const int index = devsettings::zone_index(_draft.timezone);
        if (index >= 0) {
            snprintf(out, out_len, "%s", devsettings::zone_at(static_cast<size_t>(index))->name);
        } else {
            snprintf(out, out_len, "%s — не из списка", _draft.timezone);
        }
        return;
    }
    default: return;
    }
}

bool DeviceSettingsScreen::adjust(Row r, int delta)
{
    _notice_ok = false;
    _error[0] = '\0';

    switch (r) {
    case Row::Brightness: {
        const uint8_t next = devsettings::brightness_step(_draft.brightness, delta);
        if (next == _draft.brightness) return false;
        _draft.brightness = next;
        // Яркость применяется сразу: иначе правка невидима, а вечером она и
        // нужна. В файл уйдёт по Ctrl+S; при выходе без сохранения main
        // вернёт прежнее значение.
        _applied = true;
        _redraw_rows = true;
        return true;
    }
    case Row::Timeout: {
        const uint8_t next = devsettings::timeout_step(_draft.screen_timeout_min, delta);
        if (next == _draft.screen_timeout_min) return false;
        _draft.screen_timeout_min = next;
        _redraw_rows = true;
        return true;
    }
    case Row::Layout:
        // Переключатель: любой шаг меняет сторону, «в какую сторону шаг» здесь
        // не видно — значений всего два.
        _draft.layout_russian = !_draft.layout_russian;
        _applied = true;
        _redraw_rows = true;
        return true;
    case Row::Warn:
    case Row::Critical:
    case Row::Shutdown: {
        const devsettings::Battery which = r == Row::Warn       ? devsettings::Battery::Warn
                                           : r == Row::Critical ? devsettings::Battery::Critical
                                                                : devsettings::Battery::Shutdown;
        if (!devsettings::battery_step(_draft, which, delta)) {
            // Упёрлись в соседний порог или границу диапазона. Молчать нельзя:
            // человек жмёт стрелку и не понимает, почему число не меняется.
            const char* order = "Порядок обязателен: предупреждение > критический > выключение";
            notice(order);
            return false;
        }
        _redraw_rows = true;
        return true;
    }
    case Row::Timezone: {
        const devsettings::Zone& zone = devsettings::zone_step(_draft.timezone, delta);
        if (strcmp(zone.posix, _draft.timezone) == 0) return false;
        snprintf(_draft.timezone, sizeof(_draft.timezone), "%s", zone.posix);
        // Таймзона применяется сразу: часы в трее должны смениться на глазах,
        // иначе непонятно, сработало ли.
        _applied = true;
        _redraw_rows = true;
        return true;
    }
    default: return false;
    }
}

void DeviceSettingsScreen::sync_layout(bool russian)
{
    if (_draft.layout_russian == russian) return;
    _draft.layout_russian = russian;
    _redraw_rows = true;
}

void DeviceSettingsScreen::sync_timezone(const char* posix)
{
    if (posix == nullptr) return;
    if (strcmp(_draft.timezone, posix) == 0) return;
    snprintf(_draft.timezone, sizeof(_draft.timezone), "%s", posix);
    _redraw_rows = true;
}

bool DeviceSettingsScreen::apply_step(devsettings::Param param, int delta)
{
    switch (param) {
    case devsettings::Param::Brightness: return adjust(Row::Brightness, delta);
    case devsettings::Param::Timeout: return adjust(Row::Timeout, delta);
    case devsettings::Param::Layout: return adjust(Row::Layout, delta);
    case devsettings::Param::Warn: return adjust(Row::Warn, delta);
    case devsettings::Param::Critical: return adjust(Row::Critical, delta);
    case devsettings::Param::Shutdown: return adjust(Row::Shutdown, delta);
    case devsettings::Param::Timezone: return adjust(Row::Timezone, delta);
    case devsettings::Param::Save: save(); return true;
    default: return false;
    }
}

// --- Сохранение -------------------------------------------------------------

void DeviceSettingsScreen::save()
{
    if (_settings == nullptr) return;

    const settings::Config before = _settings->config();

    char err[160];
    if (!_settings->set_device_settings(_draft, err, sizeof(err))) {
        fail(err[0] != '\0' ? err : "Не удалось сохранить параметры");
        // Черновик возвращаем к сохранённому: иначе экран показывал бы
        // несохранённые значения как рабочие.
        _draft = before;
        _redraw_rows = true;
        return;
    }

    _draft = _settings->config();
    _applied = true;
    notice("Параметры сохранены");
    _redraw_rows = true;
}

void DeviceSettingsScreen::fail(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text != nullptr ? text : "");
    _notice_ok = false;
    _redraw_error = true;
}

void DeviceSettingsScreen::notice(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text != nullptr ? text : "");
    _notice_ok = true;
    _redraw_error = true;
}

// --- Отрисовка --------------------------------------------------------------

void DeviceSettingsScreen::draw()
{
    if (_redraw_all) {
        _redraw_all = false;
        _redraw_rows = _redraw_error = _redraw_hints = false;
        draw_all();
        return;
    }

    if (_redraw_rows) {
        _redraw_rows = false;
        draw_rows();
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

void DeviceSettingsScreen::draw_all()
{
    M5.Display.fillScreen(TFT_BLACK);
    draw_rows();
    draw_error();
    draw_hints();
}

void DeviceSettingsScreen::draw_rows()
{
    M5.Display.fillRect(0, 0, M5.Display.width(), kRowsY, TFT_BLACK);
    ui::text("Параметры устройства", kMargin, kTitleY, TFT_WHITE);
    ui::draw_tray(M5.Display.width());

    for (size_t i = 0; i < kRowCount; ++i) {
        draw_row(static_cast<Row>(i));
    }

    // Хвост формы: если строк стало меньше (а kRowCount фиксирован), там могла
    // остаться полоска прежнего экрана.
    if (kRowsBottom < kErrorY) {
        M5.Display.fillRect(0, kRowsBottom, M5.Display.width(), kErrorY - kRowsBottom, TFT_BLACK);
    }
}

void DeviceSettingsScreen::draw_row(Row r)
{
    const int index = static_cast<uint8_t>(r);
    const int y = kRowsY + index * kRowH;
    const bool sel = (r == _selected);

    M5.Display.fillRect(0, y, M5.Display.width(), kRowH, sel ? kColorSelected : TFT_BLACK);
    if (sel) M5.Display.drawRect(0, y, M5.Display.width() - 1, kRowH - 1, kColorDim);

    const char* name = "";
    switch (r) {
    case Row::Brightness: name = "Яркость экрана"; break;
    case Row::Timeout: name = "Гасить подсветку"; break;
    case Row::Layout: name = "Раскладка клавиатуры"; break;
    case Row::Warn: name = devsettings::battery_name(devsettings::Battery::Warn); break;
    case Row::Critical: name = devsettings::battery_name(devsettings::Battery::Critical); break;
    case Row::Shutdown: name = devsettings::battery_name(devsettings::Battery::Shutdown); break;
    case Row::Timezone: name = "Часовой пояс"; break;
    default: break;
    }
    ui::text(name, kMargin, y + kLabelY, sel ? TFT_YELLOW : kColorFaint);

    row_value(r, g_value, sizeof(g_value));
    ui::text(g_value, kMargin, y + kValueY, TFT_WHITE);

    // Подсказка справа: «← → шаг» на каждой строке — шум, но на выбранной она
    // объясняет, что стрелки работают.
    if (sel) {
        ui::text_right("← → изменить", M5.Display.width() - kMargin, y + kRowH / 2 - 8, kColorFaint);
    }
}

void DeviceSettingsScreen::draw_error()
{
    M5.Display.fillRect(0, kErrorY, M5.Display.width(), ui::kHintY - 8 - kErrorY, TFT_BLACK);
    if (_error[0] == '\0') return;

    ui::text(ui::fit_width(_error, M5.Display.width() - 2 * kMargin, g_value, sizeof(g_value)), kMargin, kErrorY,
        _notice_ok ? TFT_GREEN : kColorBad);
}

void DeviceSettingsScreen::draw_hints()
{
    M5.Display.fillRect(0, ui::kHintY - 8, M5.Display.width(), 34, TFT_BLACK);
    ui::text("↑↓ — выбор · ←→ — изменить · Ctrl+S — сохранить · Esc — выход", kMargin, ui::kHintY, kColorFaint);
}

}  // namespace ui
