// Драйвер клавиатуры A164 поверх официальной M5Unit-KEYBOARD.
//
// Библиотека работает в Normal-режиме и отдаёт события матрицы (row/col) через
// очередь. Мы превращаем их в символы и спецклавиши с учётом модификаторов.
// Модификаторы отслеживаем сами: в Normal-режиме байт модификаторов не приходит.

#include "hw/keyboard.h"

#include <M5Unified.h>

#include "app_config.h"

namespace hw {
namespace {

using keymap::Special;
using m5::unit::tab5_keyboard::Event;
using m5::unit::tab5_keyboard::EventType;
using m5::unit::tab5_keyboard::Mode;

/// Позиции модификаторов в матрице (см. util/keymap.cpp).
constexpr uint8_t kRowSym = 3, kColSym = 0;
constexpr uint8_t kRowAa = 3, kColAa = 1;
constexpr uint8_t kRowCtrl = 4, kColCtrl = 0;
constexpr uint8_t kRowAlt = 4, kColAlt = 1;

/// Позиция пробела — нужна для Ctrl+Space (переключение раскладки).
constexpr uint8_t kRowSpace = 4, kColSpace = 13;

}  // namespace

bool Keyboard::begin()
{
    // Клавиатура висит на отдельной шине, а не на внутренней I2C платы.
    // Закрываем In_I2C-шину на этих пинах, иначе конфликт за GPIO.
    Wire.end();
    Wire.begin(cfg::kKeyboardSda, cfg::kKeyboardScl, 400000);

    auto kcfg = _keyboard.config();
    kcfg.mode = Mode::Normal;  // нужны физические позиции для ЙЦУКЕН
    kcfg.irq_pin = cfg::kKeyboardInt;
    kcfg.software_repeat = true;  // удержание стрелок и Backspace
    kcfg.repeat_initial_ms = 400;
    // 80 мс — значение по умолчанию в библиотеке. Меньше ставить нельзя: кадр
    // прокрутки ленты на этой плате занимает 55–70 мс (сдвиг готового
    // изображения в PSRAM плюс дорисовка открывшейся полосы), и при 60 мс
    // события приходили чаще, чем экран успевал дорисоваться.
    kcfg.repeat_rate_ms = 80;
    _keyboard.config(kcfg);

    if (!_units.add(_keyboard, Wire)) return false;
    if (!_units.begin()) return false;

    _firmware_version = _keyboard.firmwareVersion();
    return true;
}

bool Keyboard::translate(const Event& ev, KeyEvent& out)
{
    if (ev.type != EventType::Key) return false;

    const uint8_t row = ev.key.row;
    const uint8_t col = ev.key.col;

    // События отпускания нужны только для модификаторов: печатный символ
    // отправляется в момент нажатия, чтобы не дублировать текст.
    if (!ev.key.pressed) {
        if (row == kRowCtrl && col == kColCtrl) _ctrl = false;
        if (row == kRowAlt && col == kColAlt) _alt = false;
        if (row == kRowAa && col == kColAa) _shift = false;
        if (row == kRowSym && col == kColSym) _sym = false;
        return false;
    }

    // Модификаторы: сначала обновляем состояние, потом отдаём событие.
    //
    // Отдавать событие обязательно, хотя символа у модификатора нет: иначе
    // потребитель не узнает, что клавиша вообще нажималась. На диагностике
    // это выглядело как «нажал Sym, Aa, Ctrl, Alt — счётчик не вырос».
    // Потребитель, которому нужен только текст, проверяет type == Char.
    keymap::Special modifier = keymap::Special::None;
    if (row == kRowCtrl && col == kColCtrl) {
        _ctrl = true;
        modifier = keymap::Special::Ctrl;
    } else if (row == kRowAlt && col == kColAlt) {
        _alt = true;
        modifier = keymap::Special::Alt;
    } else if (row == kRowAa && col == kColAa) {
        _shift = !_shift;  // защёлка: Caps Lock, а не «удержание»
        modifier = keymap::Special::Aa;
    } else if (row == kRowSym && col == kColSym) {
        _sym = true;
        modifier = keymap::Special::Sym;
    }

    if (modifier != keymap::Special::None) {
        out.type = KeyEvent::Type::Special;
        out.special = modifier;
        out.utf8[0] = '\0';
        out.row = row;
        out.col = col;
        out.ctrl = _ctrl;
        out.alt = _alt;
        out.shift = _shift;
        return true;
    }

    // Ctrl+Space — переключение раскладки. В текст не попадает, но событие
    // отдать обязательно: иначе ни лог, ни индикатор на экране не узнают, что
    // раскладка сменилась, и пишут старое значение до следующего нажатия.
    // Это тот же класс ошибки, что был с модификаторами, — молчаливая клавиша.
    if (_ctrl && row == kRowSpace && col == kColSpace) {
        toggleLayout();
        out.type = KeyEvent::Type::Special;
        out.special = Special::LayoutToggle;
        out.utf8[0] = '\0';
        out.row = row;
        out.col = col;
        out.ctrl = _ctrl;
        out.alt = _alt;
        out.shift = _shift;
        return true;
    }

    out.ctrl = _ctrl;
    out.alt = _alt;
    out.shift = _shift;
    out.row = row;
    out.col = col;

    const Special sp = keymap::special(row, col);
    if (sp != Special::None) {
        out.type = KeyEvent::Type::Special;
        out.special = sp;
        out.utf8[0] = '\0';
        return true;
    }

    const size_t n = keymap::char_utf8(row, col, _layout, _shift, _sym, out.utf8, sizeof(out.utf8) - 1);
    if (n == 0) return false;
    out.utf8[n] = '\0';
    out.type = KeyEvent::Type::Char;
    out.special = Special::None;
    return true;
}

/// Пауза после отказа светодиодов. Клавиатура съёмная: если её отстегнули,
/// каждая запись — таймаут I2C, и повторять это каждый кадр нельзя. 5 секунд
/// достаточно, чтобы пристёгнутая обратно клавиатура ожила без перезагрузки,
/// и достаточно редко, чтобы не тормозить интерфейс.
constexpr uint32_t kLedRetryMs = 5000;

bool Keyboard::ledFailed()
{
    _led_dead = true;
    _led_retry_at_ms = millis() + kLedRetryMs;
    // Считаем, что в клавиатуре ничего не записано: после возврата на шину
    // придётся выставить и режим, и цвет, и яркость заново. Желаемые значения
    // (_led_brightness_wanted, _led_left/_led_right) не трогаем — их и надо
    // будет повторить.
    _led_custom = false;
    _led_written = false;
    _led_brightness = 0xFF;
    return false;
}

bool Keyboard::setLedBrightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    _led_brightness_wanted = percent;

    if (_led_brightness == percent) return true;
    if (_led_dead && static_cast<int32_t>(millis() - _led_retry_at_ms) < 0) return false;

    // Яркость — глобальная на оба светодиода, регистр 0x03. Заводское
    // значение 20: на столе этого хватает, ночью — слепит, поэтому ставим 10.
    if (!_keyboard.writeBrightness(percent)) return ledFailed();
    _led_dead = false;
    _led_brightness = percent;
    return true;
}

bool Keyboard::setLeds(const led::Rgb& left, const led::Rgb& right)
{
    // Ничего не изменилось — не трогаем шину. Желаемую яркость проверяем тоже:
    // иначе отложенная из setup() запись не применилась бы, пока цвет совпал.
    const bool brightness_pending = _led_brightness_wanted != 0xFF && _led_brightness != _led_brightness_wanted;
    if (!_led_dead && _led_written && !brightness_pending && left == _led_left && right == _led_right) return true;
    if (_led_dead && static_cast<int32_t>(millis() - _led_retry_at_ms) < 0) return false;

    // Отложенная яркость: вызов из setup() мог не дойти до клавиатуры (её
    // отстегнули, шина не ответила). Повторяем здесь — это единственное место,
    // которое вызывается регулярно.
    if (brightness_pending) {
        if (!_keyboard.writeBrightness(_led_brightness_wanted)) return ledFailed();
        _led_brightness = _led_brightness_wanted;
    }

    // В заводском режиме Bind цветом управляет прошивка клавиатуры: левый
    // светодиод показывает модификаторы, правый — режим (синий = Normal).
    // Записи в регистры цвета в этом режиме игнорируются, поэтому переводим
    // в Custom один раз. Родная индикация режима при этом пропадает — она
    // всё равно бесполезна: режим у нас всегда Normal.
    if (!_led_custom) {
        if (!_keyboard.writeRgbMode(m5::unit::tab5_keyboard::RgbMode::Custom)) return ledFailed();
        _led_custom = true;
    }

    if (!_keyboard.writeRgb(0, left.r, left.g, left.b)) return ledFailed();
    if (!_keyboard.writeRgb(1, right.r, right.g, right.b)) return ledFailed();

    _led_dead = false;
    _led_left = left;
    _led_right = right;
    _led_written = true;
    return true;
}

bool Keyboard::poll(KeyEvent& out)
{
    _units.update();

    // В очереди могут лежать события, которые не дают символа: нажатие и
    // отпускание модификаторов, Ctrl+Space. Если выходить после первого такого
    // события, остальные ждут следующего вызова poll(), а вызывающий цикл
    // успевает сделать паузу — при быстрой печати это заметная задержка.
    // Поэтому дренируем очередь до первого «настоящего» события.
    while (!_keyboard.empty()) {
        const Event ev = _keyboard.oldest();
        _keyboard.discard();

        if (translate(ev, out)) return true;
    }

    return false;
}

void Keyboard::toggleLayout()
{
    _layout = (_layout == keymap::Layout::Latin) ? keymap::Layout::Russian : keymap::Layout::Latin;
}

}  // namespace hw
