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
    kcfg.repeat_rate_ms = 60;
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

    // Модификаторы сначала обновляем, само нажатие символом не считается.
    if (row == kRowCtrl && col == kColCtrl) {
        _ctrl = true;
        return false;
    }
    if (row == kRowAlt && col == kColAlt) {
        _alt = true;
        return false;
    }
    if (row == kRowAa && col == kColAa) {
        _shift = !_shift;
        return false;
    }
    if (row == kRowSym && col == kColSym) {
        _sym = true;
        return false;
    }

    // Ctrl+Space — переключение раскладки, в текст не попадает.
    if (_ctrl && row == kRowSpace && col == kColSpace) {
        toggleLayout();
        return false;
    }

    out.ctrl = _ctrl;
    out.alt = _alt;
    out.shift = _shift;

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

bool Keyboard::poll(KeyEvent& out)
{
    _units.update();

    if (_keyboard.empty()) return false;

    const Event ev = _keyboard.oldest();
    _keyboard.discard();

    return translate(ev, out);
}

void Keyboard::toggleLayout()
{
    _layout = (_layout == keymap::Layout::Latin) ? keymap::Layout::Russian : keymap::Layout::Latin;
}

}  // namespace hw
