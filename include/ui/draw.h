#pragma once

// Примитивы отрисовки поверх M5GFX. Только железная часть: логики здесь нет,
// поэтому и тестов нет — проверяется глазами на плате.
//
// Заголовочный файл без .cpp: функции крошечные, а лишний объект в сборке
// ничего не даёт.

#include <M5Unified.h>

#include <stdio.h>
#include <string.h>

namespace ui {

/// Базовая линия нижней строки подсказок — одна на все экраны.
///
/// Подсказка «что нажимать» стоит на каждом экране в одном и том же месте, и
/// разъезд на два пикселя между экранами при переходах заметен: строка словно
/// подпрыгивает. 692 = 720 - 16 (высота шрифта) - 12 (воздух снизу). Горизонтальная
/// линия над подсказкой рисуется на `kHintY - 8`.
constexpr int kHintY = 692;

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

/// Рисует строку БЕЗ фона: промежутки между глифами не закрашиваются.
///
/// Нужно там, где область уже залита одним цветом. Заливка фона под каждым
/// глифом стоит почти столько же, сколько сам глиф: на ленте чата это 222 мс
/// против 125 мс на кадр. Панель TAB5 физически портретная (720×1280), а
/// рисуем мы в альбомной (1280×720), то есть каждый глиф ещё и поворачивается
/// программно — экономить здесь есть на чём.
inline void text_plain(const char* text, int x, int y, uint16_t fg, const lgfx::IFont* f = nullptr)
{
    M5.Display.setFont(f ? f : ui::font());
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(fg);
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

/// Обрезать строку по ПИКСЕЛЬНОЙ ширине, а не по числу символов.
///
/// Заголовки и имена состоят из кириллицы (2 байта) и латиницы (1 байт), поэтому
/// символьная оценка врёт в разы: 96 байт — это 96 латинских букв или 48
/// кириллических. Обрезка идёт по границе UTF-8, иначе M5GFX нарисовал бы
/// вместо буквы рамку.
///
/// @param scratch     буфер для результата (используется, только если режем)
/// @param scratch_len его размер, не меньше 2
/// @return исходную строку, если она помещается, иначе — обрезанную с «…»
inline const char* fit_width(const char* text, int room, char* scratch, size_t scratch_len)
{
    if (text == nullptr || room <= 0 || scratch == nullptr || scratch_len < 2) return "";

    // Шрифт ставим явно: мерим тем же, которым будем рисовать.
    M5.Display.setFont(ui::font());

    if (M5.Display.textWidth(text) <= room) return text;

    const size_t cap = scratch_len - 4;  // место под «…» (3 байта UTF-8)
    size_t len = strlen(text);
    if (len > cap) len = cap;
    memcpy(scratch, text, len);
    scratch[len] = '\0';

    while (len > 0) {
        --len;
        while (len > 0 && (static_cast<unsigned char>(scratch[len]) & 0xC0) == 0x80)
            --len;
        scratch[len] = '\0';
        char probe[192];
        snprintf(probe, sizeof(probe), "%s…", scratch);
        if (M5.Display.textWidth(probe) <= room) {
            snprintf(scratch, scratch_len, "%s", probe);
            return scratch;
        }
    }

    scratch[0] = '\0';
    return scratch;
}

}  // namespace ui
