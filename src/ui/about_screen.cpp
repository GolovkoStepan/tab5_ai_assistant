// Экран «О программе».
//
// Сведений немного, и все они — только чтение. Поэтому экран простой: пары
// «подпись — значение» в две колонки и одна подсказка внизу. Никаких кнопок:
// настройки живут в своих экранах, и дублировать их здесь значило бы держать
// два места, где одно и то же значение меняется.
//
// Экран не переключает себя сам: он возвращает true из onKey/onTouch, а экран
// назначения выбирает main — как у остальных экранов.
//
// Перерисовка построчная. Снимок приходит раз в секунду, и в нём меняются
// «Работает» и «Память» — полный кадр 1280x720 каждую секунду на панели TAB5
// виден как мерцание. Поэтому draw() заливает и рисует заново только строки,
// чьё значение изменилось.

#include "ui/about_screen.h"

#include <M5Unified.h>

#include <string.h>

#include "ui/draw.h"
#include "ui/tray.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 16;
constexpr int kTitleY = 12;
constexpr int kListY = 96;
constexpr int kRowH = 42;

/// Ширина колонки подписи. Значения начинаются с этой координаты: так они
/// выстраиваются в столбец, и глаз читает список, а не ищет начало каждой
/// строки.
constexpr int kLabelW = 190;

/// Область значения кончается там, где начинается трей: он есть и на этом
/// экране, а длинный адрес API иначе уехал бы под часы.
constexpr int kValuePad = 24;

/// Разделитель между «железом и программой» и «состоянием». Строки после него
/// сдвинуты вниз на его высоту с отступами.
constexpr int kDividerAfter = 6;
constexpr int kDividerGap = 26;

constexpr uint16_t kColorLabel = TFT_DARKGRAY;
constexpr uint16_t kColorValue = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorFrame = 0x18E3;
/// Пустое значение — не ошибка, но и не данные: «—» читается как «нет», а
/// пустая строка рядом с подписью выглядела бы обрезанной.
constexpr const char* kEmpty = "—";

/// Подписи строк в порядке показа. Число подписей и порядок полей в Info
/// связаны: поле строки n берётся field_at(n).
constexpr const char* kLabels[] = {
    "Версия",
    "Модель",
    "Адрес API",
    "Таймзона",
    "Время",
    "Работает",
    "Память",
    "PSRAM",
    "SD-карта",
    "Клавиатура",
    "Сеть",
};

constexpr int kRowCount = static_cast<int>(sizeof(kLabels) / sizeof(kLabels[0]));

const char* field_at(const AboutScreen::Info& i, int idx)
{
    switch (idx) {
    case 0: return i.version;
    case 1: return i.model;
    case 2: return i.api_url;
    case 3: return i.timezone;
    case 4: return i.local_time;
    case 5: return i.uptime;
    case 6: return i.heap;
    case 7: return i.psram;
    case 8: return i.sd_card;
    case 9: return i.keyboard;
    case 10: return i.wifi;
    default: return "";
    }
}

/// Верхняя координата строки. Одна формула и для полной отрисовки, и для
/// точечной: разъехавшись, они стёрли бы не ту строку.
int row_y(int idx)
{
    if (idx < kDividerAfter) return kListY + idx * kRowH;
    return kListY + kDividerAfter * kRowH + kDividerGap + (idx - kDividerAfter) * kRowH;
}

int divider_y()
{
    return kListY + kDividerAfter * kRowH + 6;
}

/// Залить полосу строки и нарисовать её. Ширина — до трея: он живёт в правом
/// верхнем углу, но на всякий случай не трогаем и его.
void draw_row(int idx, const AboutScreen::Info& info)
{
    const int y = row_y(idx);
    const int right = ui::tray_left_edge(M5.Display.width());
    M5.Display.fillRect(kMargin, y - 4, right - kMargin, kRowH - 2, TFT_BLACK);

    ui::text(kLabels[idx], kMargin, y, kColorLabel);

    const char* value = field_at(info, idx);
    const char* shown = (value[0] != '\0') ? value : kEmpty;

    char scratch[200];
    const int value_x = kMargin + kLabelW;
    const int value_room = right - value_x - kValuePad;
    shown = ui::fit_width(shown, value_room, scratch, sizeof(scratch));
    ui::text(shown, value_x, y, kColorValue);
}

/// Нарисовано ли то же самое. Сравниваем поля по одному, а не memcmp по
/// структуре: между полями лежит выравнивание, и его содержимое не определено.
bool same(const AboutScreen::Info& a, const AboutScreen::Info& b)
{
    for (int i = 0; i < kRowCount; ++i) {
        if (strcmp(field_at(a, i), field_at(b, i)) != 0) return false;
    }
    return true;
}

}  // namespace

void AboutScreen::set_info(const Info& info)
{
    // Содержимое не изменилось — перерисовывать нечего. Снимок приходит раз в
    // секунду, а меняются в нём две-три строки.
    if (_drawn_valid && same(info, _drawn)) return;
    _info = info;
}

void AboutScreen::onEnter()
{
    // Вход всегда с чистой панели: экран открывают поверх другого.
    _redraw_all = true;
    _drawn_valid = false;
}

bool AboutScreen::onKey(const hw::KeyEvent& ev)
{
    // Выход — Esc. Стрелок и Enter здесь нет: нажимать нечего.
    return ev.type == hw::KeyEvent::Type::Special && ev.special == keymap::Special::Escape;
}

bool AboutScreen::onTouch(int x, int y)
{
    // Полоса подсказки работает как «назад»: экран не имеет кнопок, а палец
    // без клавиатуры должен уметь выйти. Ниже подсказки — уже не нажатие.
    (void)x;
    return y >= ui::kHintY - 12;
}

void AboutScreen::draw()
{
    if (_redraw_all) {
        _redraw_all = false;

        M5.Display.fillScreen(TFT_BLACK);
        ui::text("О программе", kMargin, kTitleY, TFT_WHITE);
        ui::draw_tray(M5.Display.width());

        const int right = ui::tray_left_edge(M5.Display.width());
        M5.Display.drawFastHLine(kMargin, divider_y(), right - kMargin - kValuePad, kColorFrame);
        ui::text("Esc — назад · Ctrl+M — меню команд", kMargin, ui::kHintY, kColorFaint);

        for (int i = 0; i < kRowCount; ++i)
            draw_row(i, _info);

        _drawn = _info;
        _drawn_valid = true;
        return;
    }

    if (!_drawn_valid) return;

    // Точечно: только строки, чьё значение правда изменилось.
    bool changed = false;
    for (int i = 0; i < kRowCount; ++i) {
        if (strcmp(field_at(_info, i), field_at(_drawn, i)) == 0) continue;
        draw_row(i, _info);
        changed = true;
    }

    if (changed) _drawn = _info;
}

bool AboutScreen::dirty() const
{
    if (_redraw_all) return true;
    if (!_drawn_valid) return false;
    // Есть ли хоть одно расхождение — тот же вопрос, что задаёт draw().
    return !same(_info, _drawn);
}

}  // namespace ui
