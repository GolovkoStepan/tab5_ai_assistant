// Экран настроек API.
//
// Правки идут в черновик (_draft) и уходят в файл только по Ctrl+S. Иначе каждая
// буква адреса переписывала бы SD-карту, а неудачная правка сохранялась бы
// молча — включая «нет модели» и «нет адреса».
//
// Перерисовка точечная, как и на других экранах: при вводе трогается только
// строка поля, полный кадр — при входе и смене режима.

#include "ui/api_settings_screen.h"

#include <string.h>

#include "app_config.h"
#include "ui/draw.h"
#include "ui/tray.h"
#include "util/api_key.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 14;
constexpr int kTitleY = 12;
constexpr int kRowsY = 62;
constexpr int kRowH = 64;
constexpr int kLabelY = 6;
constexpr int kValueY = 30;
/// Полоса ошибки ниже последней строки формы: полей стало семь (с веб-поиском),
/// и прежние 400 px накрыли бы строку «Источников».
constexpr int kErrorY = 540;
constexpr int kHintsY = 662;

constexpr uint16_t kColorSelected = 0x0320;  // тёмно-зелёный
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorBad = TFT_ORANGE;
constexpr uint16_t kColorFrame = 0x18E3;

/// Отступ справа от каретки, чтобы она не прилипала к подсказке.
constexpr int kCaretPad = 12;

/// Ширина и высота каретки.
constexpr int kCaretW = 2;
constexpr int kCaretH = 17;

/// Буфер для обрезки значения по ширине. Статический, а не на стеке: адрес
/// занимает под 200 байт, а функция вызывается на каждую перерисовку строки.
char g_fit_buf[settings::kMaxUrl + 8];

bool is_char(const hw::KeyEvent& ev)
{
    return ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] != '\0';
}

bool is_ctrl_s(const hw::KeyEvent& ev)
{
    // Ctrl+S, в том числе при русской раскладке (там на этой клавише «ы»).
    return ev.ctrl && is_char(ev) && (strcmp(ev.utf8, "s") == 0 || strcmp(ev.utf8, "ы") == 0);
}

/// Названия уровней рассуждений. Индекс — значение из настроек 0..3.
const char* effort_name(uint8_t level)
{
    switch (level) {
    case 0: return "none — выключены";
    case 1: return "low — низкий";
    case 2: return "medium — средний";
    case 3: return "high — высокий";
    default: return "?";
    }
}

/// Подпись поля с числом результатов. Показываем и «сколько страниц в ответе»,
/// и «сколько это стоит», потому что поиск тарифицируется за запрос.
const char* results_note(uint8_t n)
{
    if (n <= 1) return "одна страница";
    if (n <= 4) return "несколько страниц";
    return "широкая выдача";
}

}  // namespace

void ApiSettingsScreen::init(hw::SettingsStore& settings, hw::WifiManager& wifi)
{
    _settings = &settings;
    _wifi = &wifi;
}

void ApiSettingsScreen::onEnter()
{
    _mode = Mode::Browsing;
    _selected = Field::BaseUrl;
    _error[0] = '\0';
    _notice_ok = false;

    if (_settings != nullptr) {
        _draft = _settings->config();
        _has_key = _settings->has_api_key();

        // Ключ НЕ подставляем в буфер: строка ключа на экране — подсказка для
        // того, кто смотрит через плечо. Ввод начинается с пустого поля, а
        // существующий ключ остаётся в NVS, пока не введён новый.
        _api_key[0] = '\0';
    }

    _redraw_all = true;
}

void ApiSettingsScreen::onExit()
{
    // Несохранённый ключ не должен остаться в памяти экрана.
    _api_key[0] = '\0';
}

// --- Клавиатура -------------------------------------------------------------

bool ApiSettingsScreen::onKey(const hw::KeyEvent& ev)
{
    // Ctrl+S сохраняет из любого режима: правка могла остаться незакрытой.
    if (is_ctrl_s(ev)) {
        save();
        return false;
    }

    if (_mode == Mode::Editing) {
        if (ev.type == hw::KeyEvent::Type::Special) {
            // У поля без видимой каретки стрелки и Delete смысла не имеют:
            // двигать нечего, а стирание «где-то там» выглядело бы поломкой.
            if (!caret_editable(_selected) &&
                (ev.special == keymap::Special::Left || ev.special == keymap::Special::Right ||
                    ev.special == keymap::Special::Delete)) {
                return false;
            }

            switch (ev.special) {
            case keymap::Special::Escape: cancel_edit(); return false;
            case keymap::Special::Enter: confirm_edit(); return false;
            case keymap::Special::Backspace: edit_backspace(); return false;
            case keymap::Special::Delete: edit_delete(); return false;

            // Каретка. Ctrl+стрелка — сразу в начало или конец: адрес бывает
            // длиной под 190 символов, и щёлкать по нему стрелкой — не работа.
            case keymap::Special::Left:
                if (ev.ctrl)
                    caret_to_start();
                else
                    caret_left();
                return false;
            case keymap::Special::Right:
                if (ev.ctrl)
                    caret_to_end();
                else
                    caret_right();
                return false;

            case keymap::Special::Space: insert_text(" "); return false;
            default: return false;
            }
        }

        if (!is_char(ev) || ev.ctrl || ev.alt) return false;
        edit_char(ev);
        return false;
    }

    // --- Навигация ---
    if (ev.type != hw::KeyEvent::Type::Special) return false;

    switch (ev.special) {
    case keymap::Special::Escape: return true;  // выйти

    case keymap::Special::Up:
        if (static_cast<uint8_t>(_selected) > 0) {
            _selected = static_cast<Field>(static_cast<uint8_t>(_selected) - 1);
            _redraw_rows = true;
        }
        return false;

    case keymap::Special::Down:
        if (static_cast<size_t>(static_cast<uint8_t>(_selected)) + 1 < kFieldCount) {
            _selected = static_cast<Field>(static_cast<uint8_t>(_selected) + 1);
            _redraw_rows = true;
        }
        return false;

    case keymap::Special::Enter:
    case keymap::Special::Space:
        if (stepper(_selected)) {
            adjust_value(1);
        } else {
            begin_edit();
        }
        return false;

    case keymap::Special::Left:
        if (stepper(_selected)) adjust_value(-1);
        return false;

    case keymap::Special::Right:
        if (stepper(_selected)) adjust_value(1);
        return false;

    default: return false;
    }
}

bool ApiSettingsScreen::caret_editable(Field f)
{
    return f == Field::BaseUrl || f == Field::Model;
}

void ApiSettingsScreen::edit_char(const hw::KeyEvent& ev)
{
    insert_text(ev.utf8);
}

void ApiSettingsScreen::insert_text(const char* text)
{
    if (text == nullptr || text[0] == '\0') return;

    char* buf = field_buffer(_selected, nullptr);
    size_t cap = 0;
    field_buffer(_selected, &cap);
    if (buf == nullptr || cap == 0) return;

    const size_t len = text_len(buf);
    const size_t n = strlen(text);
    if (len + n >= cap) {
        fail("Строка не помещается в поле");
        return;
    }

    // У ключа на экране только хвост, каретки не видно: вставка «по месту
    // каретки» была бы вставкой в невидимую позицию. Туда — только в конец.
    if (!caret_editable(_selected))
        caret_to_end_of_field();
    else
        clamp_caret();

    const size_t at = _caret;
    if (at > len) return;

    // Хвост сдвигаем вместе с завершающим нулём — символ вставляется ПО МЕСТУ
    // каретки, а не в конец. Пробел приходит сюда же: для поля это обычный
    // символ текста, а не спецклавиша.
    memmove(buf + at + n, buf + at, len - at + 1);
    memcpy(buf + at, text, n);
    _caret = at + n;

    _notice_ok = false;
    _redraw_rows = true;
}

void ApiSettingsScreen::edit_backspace()
{
    char* buf = field_buffer(_selected, nullptr);
    if (buf == nullptr) return;

    clamp_caret();
    if (_caret == 0) return;

    // Удаляем целый UTF-8 символ: иначе кириллица в модели рассыпалась бы.
    size_t from = _caret - 1;
    while (from > 0 && (static_cast<unsigned char>(buf[from]) & 0xC0) == 0x80)
        --from;

    const size_t len = text_len(buf);
    memmove(buf + from, buf + _caret, len - _caret + 1);
    _caret = from;

    _redraw_rows = true;
}

void ApiSettingsScreen::edit_delete()
{
    char* buf = field_buffer(_selected, nullptr);
    if (buf == nullptr) return;

    clamp_caret();
    const size_t len = text_len(buf);
    if (_caret >= len) return;

    // Символ ПОД кареткой: до следующей границы UTF-8.
    size_t to = _caret + 1;
    while (to < len && (static_cast<unsigned char>(buf[to]) & 0xC0) == 0x80)
        ++to;

    memmove(buf + _caret, buf + to, len - to + 1);
    _redraw_rows = true;
}

void ApiSettingsScreen::caret_left()
{
    if (_caret == 0) return;
    --_caret;
    clamp_caret();
    _redraw_rows = true;
}

void ApiSettingsScreen::caret_right()
{
    char* buf = field_buffer(_selected, nullptr);
    const size_t len = text_len(buf);
    if (_caret >= len) return;
    ++_caret;
    clamp_caret();
    _redraw_rows = true;
}

void ApiSettingsScreen::caret_to_start()
{
    _caret = 0;
    _redraw_rows = true;
}

void ApiSettingsScreen::caret_to_end()
{
    caret_to_end_of_field();
    _redraw_rows = true;
}

void ApiSettingsScreen::clamp_caret()
{
    char* buf = field_buffer(_selected, nullptr);
    if (buf == nullptr) {
        _caret = 0;
        return;
    }

    const size_t len = text_len(buf);
    if (_caret > len) _caret = len;

    // Внутрь многобайтного символа каретка попасть не должна: иначе вставка
    // разрезала бы букву и M5GFX нарисовал бы вместо неё рамку.
    while (_caret > 0 && (static_cast<unsigned char>(buf[_caret]) & 0xC0) == 0x80)
        --_caret;
}

void ApiSettingsScreen::caret_to_end_of_field()
{
    char* buf = field_buffer(_selected, nullptr);
    _caret = text_len(buf);
}

void ApiSettingsScreen::begin_edit()
{
    size_t cap = 0;
    if (field_buffer(_selected, &cap) == nullptr) return;

    _mode = Mode::Editing;
    caret_to_end_of_field();
    _error[0] = '\0';
    _notice_ok = false;
    _redraw_rows = true;
    _redraw_error = true;
    _redraw_hints = true;
}

void ApiSettingsScreen::cancel_edit()
{
    // Отмена возвращает прежнее значение поля: правка не должна применяться
    // «наполовину».
    if (_settings != nullptr) _draft = _settings->config();
    _api_key[0] = '\0';

    _mode = Mode::Browsing;
    _caret = 0;
    _redraw_rows = true;
    _redraw_hints = true;
}

void ApiSettingsScreen::confirm_edit()
{
    _mode = Mode::Browsing;
    _caret = 0;

    // Пустой адрес или модель — почти наверняка случайное стирание. Сохранить
    // такое значит получить «модель не найдена» через минуту.
    if (_selected == Field::BaseUrl && _draft.api_base_url[0] == '\0') {
        fail("Адрес API не может быть пустым");
    } else if (_selected == Field::Model && _draft.api_model[0] == '\0') {
        fail("Модель не может быть пустой");
    }

    _redraw_rows = true;
    _redraw_hints = true;
}

bool ApiSettingsScreen::stepper(Field f)
{
    return f == Field::Temperature || f == Field::Reasoning || f == Field::Search || f == Field::SearchResults;
}

void ApiSettingsScreen::adjust_value(int delta)
{
    if (_selected == Field::Temperature) {
        const int t = static_cast<int>(_draft.temperature_tenths) + delta;
        if (t >= 0 && t <= 20) _draft.temperature_tenths = static_cast<uint8_t>(t);
    } else if (_selected == Field::Reasoning) {
        const int r = static_cast<int>(_draft.reasoning_effort) + delta;
        if (r >= 0 && r <= 3) _draft.reasoning_effort = static_cast<uint8_t>(r);
    } else if (_selected == Field::Search) {
        // Флажок: ← выключает, → и Enter включают. Переключение «всегда» на
        // любое нажатие сбивало бы с толку — не видно, в какую сторону шаг.
        _draft.search_enabled = (delta > 0);
    } else if (_selected == Field::SearchResults) {
        // 1..10 — то же, что разрешает settings::validate и web::kMaxSources.
        const int n = static_cast<int>(_draft.search_max_results) + delta;
        if (n >= 1 && n <= 10) _draft.search_max_results = static_cast<uint8_t>(n);
    }
    _notice_ok = false;
    _redraw_rows = true;
}

// --- Касание ----------------------------------------------------------------

bool ApiSettingsScreen::onTouch(int x, int y)
{
    if (y < kRowsY || y >= kRowsY + static_cast<int>(kFieldCount) * kRowH) return false;

    const int row = (y - kRowsY) / kRowH;
    const Field f = static_cast<Field>(row);

    if (f == _selected && stepper(f)) {
        // Повторное касание по полю-шагу — шаг вперёд.
        adjust_value(1);
        return true;
    }

    _selected = f;
    _error[0] = '\0';
    _notice_ok = false;
    _redraw_rows = true;
    _redraw_error = true;

    if (stepper(f)) return true;

    begin_edit();
    (void)x;
    return true;
}

void ApiSettingsScreen::tick()
{
    // Экран ничего не ждёт: вся работа — по событиям клавиатуры и касания.
}

bool ApiSettingsScreen::dirty() const
{
    return _redraw_all || _redraw_rows || _redraw_error || _redraw_hints;
}

// --- Сохранение -------------------------------------------------------------

void ApiSettingsScreen::save()
{
    if (_settings == nullptr) return;

    _error[0] = '\0';
    _notice_ok = false;

    // Ключ проверяем до записи: не-ASCII или пробел в заголовке означают, что
    // запрос не уйдёт вовсе, а выглядеть это будет как «сервер молчит».
    if (_api_key[0] != '\0') {
        apikey::clean(_api_key);

        char err[128];
        if (!apikey::validate(_api_key, settings::kMaxApiKey - 1, err, sizeof(err))) {
            fail(err);
            return;
        }

        if (!_settings->set_api_key(_api_key)) {
            fail("Не удалось сохранить ключ в NVS");
            return;
        }

        _has_key = true;
        _api_key[0] = '\0';
    }

    char err[160];
    if (!_settings->set_api_settings(_draft, err, sizeof(err))) {
        fail(err);
        return;
    }

    // Черновик берём из хранилища: set_api_settings часть полей подменила
    // своими (яркость, раскладку), и в черновике иначе остались бы старые.
    _draft = _settings->config();

    _error[0] = '\0';
    snprintf(_error, sizeof(_error), "Сохранено");
    _notice_ok = true;

    _redraw_rows = true;
    _redraw_error = true;
}

void ApiSettingsScreen::fail(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text);
    _notice_ok = false;
    _redraw_error = true;
}

// --- Поля -------------------------------------------------------------------

const char* ApiSettingsScreen::field_name(Field f) const
{
    switch (f) {
    case Field::BaseUrl: return "Адрес API";
    case Field::ApiKey: return "Ключ API (хранится в NVS)";
    case Field::Model: return "Модель";
    case Field::Temperature: return "Температура";
    case Field::Reasoning: return "Рассуждения";
    case Field::Search: return "Веб-поиск";
    case Field::SearchResults: return "Источников в ответе";
    default: return "";
    }
}

size_t ApiSettingsScreen::text_len(const char* buf)
{
    return buf != nullptr ? strlen(buf) : 0;
}

void ApiSettingsScreen::field_value(Field f, char* out, size_t out_len) const
{
    if (out == nullptr || out_len == 0) return;
    out[0] = '\0';

    switch (f) {
    case Field::BaseUrl: snprintf(out, out_len, "%s", _draft.api_base_url); return;
    case Field::ApiKey:
        if (_api_key[0] != '\0') {
            // Показываем только хвост: так видно, что вставилось именно то, но
            // целиком ключ не светится на экране.
            const size_t len = strlen(_api_key);
            const size_t shown = len < 6 ? len : 6;
            snprintf(out, out_len, "…%s (%u симв.)", _api_key + (len - shown), static_cast<unsigned>(len));
        } else if (_has_key) {
            snprintf(out, out_len, "сохранён · Enter — заменить");
        } else {
            snprintf(out, out_len, "не задан · Enter — ввести");
        }
        return;
    case Field::Model: snprintf(out, out_len, "%s", _draft.api_model); return;
    case Field::Temperature: snprintf(out, out_len, "%.1f", _draft.temperature_tenths / 10.0); return;
    case Field::Reasoning: snprintf(out, out_len, "%s", effort_name(_draft.reasoning_effort)); return;
    case Field::Search: snprintf(out, out_len, "%s", _draft.search_enabled ? "включён" : "выключен"); return;
    case Field::SearchResults:
        // При выключенном поиске число ни на что не влияет — говорим об этом
        // прямо, иначе настройка выглядит рабочей и её правят впустую.
        if (!_draft.search_enabled) {
            snprintf(out, out_len, "%u · поиск выключен", static_cast<unsigned>(_draft.search_max_results));
        } else if (_draft.search_max_results == 0) {
            // 0 в файле означает «как решит сервер» (умолчание — 5). Стрелка
            // вправо поднимет до 1: явное число понятнее серверного умолчания.
            snprintf(out, out_len, "0 — по усмотрению сервера");
        } else {
            snprintf(out, out_len, "%u — %s", static_cast<unsigned>(_draft.search_max_results),
                results_note(_draft.search_max_results));
        }
        return;
    default: return;
    }
}

char* ApiSettingsScreen::field_buffer(Field f, size_t* cap)
{
    if (cap != nullptr) *cap = 0;

    switch (f) {
    case Field::BaseUrl:
        if (cap) *cap = sizeof(_draft.api_base_url);
        return _draft.api_base_url;
    case Field::ApiKey:
        if (cap) *cap = sizeof(_api_key);
        return _api_key;
    case Field::Model:
        if (cap) *cap = sizeof(_draft.api_model);
        return _draft.api_model;
    default: return nullptr;
    }
}

// --- Отрисовка --------------------------------------------------------------

void ApiSettingsScreen::clear_band(int y, int h)
{
    M5.Display.fillRect(0, y, M5.Display.width(), h, TFT_BLACK);
}

void ApiSettingsScreen::draw()
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

void ApiSettingsScreen::draw_all()
{
    M5.Display.fillScreen(TFT_BLACK);
    draw_rows();
    draw_error();
    draw_hints();
}

void ApiSettingsScreen::draw_rows()
{
    clear_band(0, kRowsY);
    ui::text("Настройки API", kMargin, kTitleY, TFT_WHITE);

    // Справа — только проблема: когда сети нет, настройки бесполезны, и надо
    // знать, что идти за ней в Ctrl+W. «сеть есть» здесь ничего не добавляло:
    // значок в трее и без того показывает, что подключение есть.
    if (_wifi != nullptr && !_wifi->connected()) {
        ui::text_right("нет сети · Ctrl+W", ui::tray_left_edge(M5.Display.width()), kTitleY, kColorBad);
    }

    ui::draw_tray(M5.Display.width());

    for (size_t i = 0; i < kFieldCount; ++i) {
        draw_field_row(static_cast<Field>(i));
    }
}

void ApiSettingsScreen::draw_caret_line(const char* value, int y, int room, uint16_t bg)
{
    if (value == nullptr || room <= kCaretPad) return;

    M5.Display.setFont(ui::font());

    const size_t len = strlen(value);
    size_t caret = _caret;
    if (caret > len) caret = len;

    // Ширина префикса — это и есть положение каретки на экране.
    char prefix[settings::kMaxUrl + 8];
    if (caret >= sizeof(prefix)) caret = sizeof(prefix) - 1;
    memcpy(prefix, value, caret);
    prefix[caret] = '\0';

    const int caret_x = M5.Display.textWidth(prefix);
    const int total_w = M5.Display.textWidth(value);

    // Прокручиваем так, чтобы каретка была видна: сдвиг — минимальный из
    // нужного и допустимого (дальше конца строки не уезжаем, иначе справа
    // останется пустое место).
    const int usable = room - kCaretPad;
    int shift = 0;
    if (caret_x > usable) shift = caret_x - usable;
    const int max_shift = total_w - usable;
    if (max_shift <= 0) {
        shift = 0;
    } else if (shift > max_shift) {
        shift = max_shift;
    }

    const int x = kMargin - shift;

    // Клип по полю значения: строка физически шире окна, и без клипа она
    // залезла бы на подсказку справа и на соседние строки.
    M5.Display.setClipRect(kMargin, y - 2, room, kCaretH + 4);
    M5.Display.fillRect(kMargin, y - 2, room, kCaretH + 4, bg);
    ui::text_plain(value, x, y, TFT_WHITE);
    M5.Display.fillRect(x + caret_x, y, kCaretW, kCaretH, TFT_WHITE);
    M5.Display.clearClipRect();
}

void ApiSettingsScreen::draw_field_row(Field f)
{
    const int idx = static_cast<uint8_t>(f);
    const int y = kRowsY + idx * kRowH;
    const bool sel = (f == _selected);
    const bool numeric = stepper(f);
    const bool editing = sel && _mode == Mode::Editing;

    M5.Display.fillRect(0, y, M5.Display.width(), kRowH, sel ? kColorSelected : TFT_BLACK);
    if (sel) M5.Display.drawRect(0, y, M5.Display.width() - 1, kRowH - 1, kColorDim);

    ui::text(field_name(f), kMargin, y + kLabelY, sel ? TFT_YELLOW : kColorFaint);

    // Подсказка справа: у числовых — что означает значение, у строковых — как
    // редактировать. Считаем её ширину заранее: длинный адрес не должен на неё
    // наезжать.
    const char* hint = nullptr;
    if (numeric) {
        if (f == Field::Temperature) {
            hint = "← → шаг 0.1";
        } else if (f == Field::Reasoning) {
            hint = "← → уровень";
        } else if (f == Field::Search) {
            hint = "← → вкл/выкл";
        } else {
            hint = "← → число";
        }
    } else if (editing) {
        hint = "Enter — принять · Esc — отменить";
    }
    int hint_room = 0;
    if (hint != nullptr) {
        M5.Display.setFont(ui::font());
        hint_room = M5.Display.textWidth(hint) + 2 * kMargin;
        ui::text_right(hint, M5.Display.width() - kMargin, y + kValueY, kColorFaint);
    }

    char value[settings::kMaxUrl + 48];
    field_value(f, value, sizeof(value));

    // Строковое поле в правке показываем как есть, с кареткой: иначе не видно,
    // куда именно встанет буква, и ← → выглядят как «ничего не происходит».
    // У ключа на экране только хвост и счётчик — каретки там нет.
    if (editing && caret_editable(f)) {
        draw_caret_line(value, y + kValueY, M5.Display.width() - hint_room, sel ? kColorSelected : TFT_BLACK);
        return;
    }

    ui::text(ui::fit_width(value, M5.Display.width() - 2 * kMargin - hint_room, g_fit_buf, sizeof(g_fit_buf)), kMargin,
        y + kValueY, TFT_WHITE);
}

void ApiSettingsScreen::draw_error()
{
    clear_band(kErrorY, 60);
    if (_error[0] == '\0') return;

    ui::text(_error, kMargin, kErrorY, _notice_ok ? TFT_GREEN : kColorBad);
}

void ApiSettingsScreen::draw_hints()
{
    clear_band(kHintsY - 8, M5.Display.height() - (kHintsY - 8));

    if (_mode == Mode::Editing) {
        ui::text("← → — по символам · Ctrl+← → — в начало/конец · Delete — удалить · Enter — принять · Esc — отмена",
            kMargin, kHintsY, kColorFaint);
    } else {
        ui::text("↑↓ — выбор · Enter — правка/шаг · ←→ — значение · Ctrl+S — сохранить · Esc — выход", kMargin, kHintsY,
            kColorFaint);
    }
}

}  // namespace ui
