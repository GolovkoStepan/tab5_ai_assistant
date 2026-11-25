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
#include "util/led_state.h"

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

    /// Физическая позиция клавиши в матрице. Нужна диагностике и UI,
    /// чтобы сопоставить событие с нарисованной клавишей.
    uint8_t row{0};
    uint8_t col{0};

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

    /// Физически зажата ли клавиша в позиции (row, col). Для подсветки в UI.
    bool isPressed(uint8_t row, uint8_t col) const { return _keyboard.isPressed(row, col); }

    /// Общая яркость светодиодов, 0..100. Одна на оба: отдельных регистров
    /// прошивка клавиатуры не даёт.
    ///
    /// Значение запоминается: если клавиатура сейчас не отвечает (её отстегнули
    /// в момент вызова), яркость применится при следующей удачной записи. Без
    /// этого вызов один раз в setup() терялся бы до перезагрузки.
    bool setLedBrightness(uint8_t percent);

    /// Цвета двух светодиодов: 0 — левый, 1 — правый.
    ///
    /// Первый вызов переводит клавиатуру в режим Custom: в заводском Bind цвет
    /// задаёт прошивка клавиатуры (синий = Normal), и записи игнорируются.
    /// Побочный эффект: родная индикация режима пропадает — дальше цветом
    /// управляем только мы.
    ///
    /// @return false, если клавиатура не отвечает (она съёмная)
    bool setLeds(const led::Rgb& left, const led::Rgb& right);

private:
    /// Запомнить отказ светодиодов и назначить время следующей попытки.
    bool ledFailed();

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

    /// Клавиатура переведена в режим Custom (цветами управляем мы).
    bool _led_custom{false};
    /// Клавиатура не ответила на запись светодиодов. Повторять на каждом
    /// кадре бессмысленно: отстёгнутая клавиатура висит на мёртвой шине, и
    /// каждая попытка — это таймаут I2C в задаче интерфейса. Поэтому после
    /// отказа ждём и пробуем снова не чаще, чем раз в kLedRetryMs — чтобы
    /// пристёгнутая обратно клавиатура ожила без перезагрузки.
    bool _led_dead{false};
    uint32_t _led_retry_at_ms{0};
    /// Что уже записано в клавиатуру. Нужно, чтобы не писать по I2C каждый
    /// кадр: цвет меняется редко, а шина занята событиями клавиш.
    led::Rgb _led_left{};
    led::Rgb _led_right{};
    bool _led_written{false};
    /// Что уже записано в клавиатуру. 0xFF — «ещё не записывали»: 0..100
    /// допустимые значения, 0xFF не занято.
    uint8_t _led_brightness{0xFF};
    /// Что нужно записать. Живёт отдельно от _led_brightness, чтобы желаемое
    /// значение не терялось при отказе I2C.
    uint8_t _led_brightness_wanted{0xFF};
};

}  // namespace hw
