#pragma once

// Драйвер клавиатуры A164: обёртка над официальной M5Unit-KEYBOARD.
//
// Библиотека отдаёт сырые события матрицы (строка/столбец) и сама умеет
// авто-повтор и отслеживание удержания. Наша задача — превратить позицию
// клавиши в символ с учётом раскладки (латиница/ЙЦУКЕН), регистра и слоя
// символов, и не дать модификаторам утечь в текст.
//
// Раскладка живёт в util/keymap.h — это чистые функции с юнит-тестами.

#include <stdint.h>

#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>

#include "util/keymap.h"

namespace hw {

/// Одно логическое событие клавиатуры.
struct KeyEvent {
    enum class Type : uint8_t {
        Char,     ///< печатный символ в поле utf8
        Special,  ///< управляющая клавиша в поле special
    };

    Type type{Type::Special};
    keymap::Special special{keymap::Special::None};
    /// UTF-8 символ + завершающий ноль. До 4 байт данных.
    char utf8[5]{};

    bool ctrl{false};
    bool alt{false};
    bool shift{false};

    /// Удобная проверка: это Ctrl+<клавиша>?
    bool isCtrl(keymap::Special s) const { return ctrl && type == Type::Special && special == s; }
};

/// Клавиатура A164 на ExtPort1 (отдельная шина I2C: SDA=G0, SCL=G1, INT=G50).
class Keyboard {
public:
    /// Инициализация. Возвращает false, если клавиатура не отвечает.
    bool begin();

    /// Забрать следующее событие. Возвращает false, если очередь пуста.
    /// Вызывать в цикле, пока возвращает true.
    bool poll(KeyEvent& out);

    /// Переключить раскладку (Ctrl+Space и тап по индикатору).
    void toggleLayout();
    keymap::Layout layout() const { return _layout; }
    void setLayout(keymap::Layout layout) { _layout = layout; }

    /// Версия прошивки клавиатуры, прочитанная в begin(). 0 — не прочитана.
    uint8_t firmwareVersion() const { return _firmware_version; }

private:
    /// Обработать событие и, если получился символ/спецклавиша, вернуть true.
    bool translate(const m5::unit::tab5_keyboard::Event& ev, KeyEvent& out);

    m5::unit::UnitUnified _units;
    m5::unit::UnitTab5Keyboard _keyboard;

    keymap::Layout _layout{keymap::Layout::Latin};
    uint8_t _firmware_version{0};

    // Состояние модификаторов — ведём сами, потому что в Normal-режиме
    // библиотека отдаёт только позиции, а не байт модификаторов.
    bool _ctrl{false};
    bool _alt{false};
    bool _shift{false};
    bool _sym{false};
};

}  // namespace hw
