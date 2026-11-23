#pragma once

// Примитивы отрисовки поверх M5GFX. Только железная часть: логики здесь нет,
// поэтому и тестов нет — проверяется глазами на плате.
//
// Заголовочный файл без .cpp: функции крошечные, а лишний объект в сборке
// ничего не даёт.

#include <M5Unified.h>

namespace ui {

/// Шрифт интерфейса: efontJA (Efont Biwidth), 16 px.
///
/// Почему не lgfxJapanGothic_16, который стоит по умолчанию в примерах M5GFX:
/// в нём два дефекта, критичных для редактора текста.
///   1. Обратный слэш (U+005C) нарисован как иена `¥` — шрифт следует JIS X 0201,
///      где 0x5C это знак иены, а не обратный слэш.
///   2. Подчёркивание (U+005F) — пустой глиф (w=0, h=0), то есть не рисуется
///      вообще. Проверено разбором таблицы глифов, не на глаз.
/// У efontJA_16 оба знака корректны, есть кириллица, «», №, стрелки, тире и
/// многоточие. Покрытие нужного набора — 171 символ из 172 (пустой только
/// пробел, так и должно быть).
inline const lgfx::IFont* font()
{
    return &lgfx::fonts::efontJA_16;
}

/// Рисует строку с явно заданным выравниванием.
///
/// Выравнивание НЕ наследуется между вызовами намеренно: ячейки сетки
/// переключают datum на middle_center, и если после этого не задать его заново,
/// строка уезжает и обрезается слева. Это уже случалось на диагностике.
inline void text(const char* text, int x, int y, uint16_t fg, uint16_t bg = TFT_BLACK, const lgfx::IFont* f = nullptr)
{
    M5.Display.setFont(f ? f : ui::font());
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(text, x, y);
}

/// Строка, прижатая к правому краю.
inline void text_right(const char* text, int right, int y, uint16_t fg, uint16_t bg = TFT_BLACK)
{
    M5.Display.setFont(ui::font());
    M5.Display.setTextDatum(textdatum_t::top_right);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(text, right, y);
    // Возвращаем обычное выравнивание: иначе следующая строка слева уедет.
    M5.Display.setTextDatum(textdatum_t::top_left);
}

/// Строка, центрированная по горизонтали.
inline void text_center(const char* text, int center_x, int y, uint16_t fg, uint16_t bg = TFT_BLACK)
{
    M5.Display.setFont(ui::font());
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(text, center_x, y);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

}  // namespace ui
