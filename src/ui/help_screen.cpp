// Экран помощи и меню переходов.
//
// Подсказки в нижней полосе чата выросли до двух колонок и перестали помещаться
// в 1280 px: правая строка наезжала на левую. Сокращать их до нечитаемых
// аббревиатур не хотелось, поэтому команды собраны здесь, а внизу чата осталась
// одна короткая строка.
//
// Левая колонка — меню переходов: стрелки, Enter, тап. Правая — справочник по
// командам экранов; нажимать их отсюда нельзя, и это подписано.
//
// Экран не переключает себя сам: он выставляет Target, а переход делает main.

#include "ui/help_screen.h"

#include <M5Unified.h>

#include "ui/draw.h"
#include "ui/tray.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 16;
constexpr int kTitleY = 12;
constexpr int kListY = 84;
constexpr int kRowH = 38;

/// Левая колонка меню — до этой координаты. Правая начинается с kRightX.
constexpr int kLeftW = 470;
constexpr int kRightX = kLeftW + 40;

constexpr uint16_t kColorSelected = 0x0320;  // тёмно-зелёный
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorFrame = 0x18E3;

/// Пункт меню: подпись, клавиша и что откроется.
struct Item {
    const char* keys;
    const char* title;
    HelpScreen::Target target;
};

constexpr Item kItems[] = {
    {"Ctrl+T", "Треды", HelpScreen::Target::Threads},
    {"Ctrl+O", "Документы", HelpScreen::Target::Notes},
    {"Ctrl+A", "Настройки API", HelpScreen::Target::ApiSettings},
    {"Ctrl+G", "Параметры", HelpScreen::Target::DeviceSettings},
    {"Ctrl+W", "Wi-Fi", HelpScreen::Target::Wifi},
    {"Ctrl+D", "Диагностика", HelpScreen::Target::Diagnostic},
    {"Ctrl+I", "О программе", HelpScreen::Target::About},
};

constexpr int kItemCount = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));

/// Справочник команд по экранам: подпись раздела и строка команд.
struct Section {
    const char* title;
    const char* lines[4];
};

constexpr Section kSections[] = {
    {"Чат",
        {
            "Enter — отправить · Esc — прервать или очистить",
            "↑↓ — прокрутка ленты · Alt+↑↓ — история вопросов",
            "Ctrl+S — ответ в архив · Ctrl+N — новый тред",
            "Ctrl+R — сброс треда · тап — тоже прокрутка",
        }},
    {"Документы (Ctrl+O)",
        {
            "↑ ↓ — выбор · Enter — открыть · ← → — страницы",
            "Ctrl+N — новая заметка · / — поиск",
            "Enter — править · Ctrl+S — сохранить",
            "Ctrl+Z — отмена · Del — удалить · Esc — назад",
        }},
    {"Треды · API · Wi-Fi · Параметры",
        {
            "↑↓ — выбор · Enter — открыть или править",
            "←→ — значение или страницы · Del — удалить",
            "Ctrl+N — новый тред · / — поиск в тредах",
            "Ctrl+S — сохранить · Esc — выход без правки",
        }},
    {"Везде",
        {
            "Ctrl+Space — раскладка ЙЦУКЕН / латиница",
            "Ctrl+G — параметры · Ctrl+M — меню",
            "Ctrl+I — о программе · Ctrl+D — диагностика",
            "Esc на экране — назад, обычно в чат",
        }},
};

constexpr int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

}  // namespace

void HelpScreen::onEnter()
{
    _selected = 0;
    _drawn_selected = -1;
    _target = Target::None;
    _redraw_all = true;
}

HelpScreen::Target HelpScreen::take_target()
{
    const Target t = _target;
    _target = Target::None;
    return t;
}

bool HelpScreen::onKey(const hw::KeyEvent& ev)
{
    if (ev.type == hw::KeyEvent::Type::Special) {
        switch (ev.special) {
        case keymap::Special::Escape: return true;

        case keymap::Special::Up:
            if (_selected > 0) --_selected;
            return false;

        case keymap::Special::Down:
            if (_selected + 1 < kItemCount) ++_selected;
            return false;

        case keymap::Special::Enter:
        case keymap::Special::Space: _target = kItems[_selected].target; return false;

        default: return false;
        }
    }

    // Ctrl+M, Ctrl+W, Ctrl+A разбирает main до того, как событие дойдёт сюда:
    // это глобальные команды, и они работают с любого экрана. Здесь остаются
    // только стрелки, Enter и Esc — то, что принадлежит самому меню.

    return false;
}

bool HelpScreen::onTouch(int x, int y)
{
    if (x >= kLeftW) return false;

    const int row = (y - kListY) / kRowH;
    if (row < 0 || row >= kItemCount) return false;

    // Повторный тап по выбранному пункту открывает его — как в списке тредов
    // и в архиве. Иначе на устройстве без клавиатуры меню было бы тупиком.
    if (row == _selected) {
        _target = kItems[row].target;
        return true;
    }

    _selected = row;
    return true;
}

void HelpScreen::draw()
{
    if (_redraw_all) {
        _redraw_all = false;
        M5.Display.fillScreen(TFT_BLACK);
        draw_rows();
        _drawn_selected = _selected;

        M5.Display.drawFastHLine(0, ui::kHintY - 8, M5.Display.width(), kColorFrame);
        // Все подсказки меню — внизу, как на остальных экранах. Раньше
        // «Ctrl+M — закрыть» стояла справа в шапке, то есть на месте трея:
        // подсказка про закрытие оказывалась в том углу, который читают как
        // состояние устройства, и спорила со значками Wi-Fi и батареи.
        ui::text("↑ ↓ — выбор · Enter — открыть · Esc или Ctrl+M — закрыть", kMargin, ui::kHintY, kColorFaint);
        return;
    }

    if (_drawn_selected != _selected) draw_selection();
}

void HelpScreen::draw_rows()
{
    ui::text("Меню и команды", kMargin, kTitleY, TFT_WHITE);
    ui::draw_tray(M5.Display.width());

    // Заголовки колонок: без них неясно, что слева можно нажимать, а справа
    // нельзя. Это единственное место, где разница вообще видна.
    ui::text("Перейти", kMargin, kListY - 32, kColorFaint);
    ui::text("Команды", kRightX, kListY - 32, kColorFaint);

    for (int i = 0; i < kItemCount; ++i) {
        const int y = kListY + i * kRowH;

        // Клавиша — отдельной колонкой: так список читается как таблица, и
        // глазу не надо искать сочетание внутри строки.
        ui::text(kItems[i].keys, kMargin, y + 6, kColorDim);
        ui::text(kItems[i].title, kMargin + 100, y + 6, TFT_WHITE);
    }

    // Разделитель колонок: без него правая часть читалась бы как продолжение
    // строк меню.
    const int list_h = kItemCount * kRowH;
    M5.Display.drawFastVLine(kLeftW, kListY - 8, list_h + 8, kColorFrame);

    int y = kListY;
    for (int s = 0; s < kSectionCount; ++s) {
        ui::text(kSections[s].title, kRightX, y, TFT_CYAN);
        y += 24;
        for (int l = 0; l < 4; ++l) {
            if (kSections[s].lines[l] == nullptr) break;
            ui::text(kSections[s].lines[l], kRightX, y, kColorDim);
            y += 22;
        }
        y += 12;
    }

    draw_selection();
}

void HelpScreen::draw_selection()
{
    const auto paint = [this](int row, bool selected) {
        const int y = kListY + row * kRowH;
        M5.Display.fillRect(0, y, kLeftW, kRowH, selected ? kColorSelected : TFT_BLACK);
        ui::text(kItems[row].keys, kMargin, y + 6, selected ? TFT_WHITE : kColorDim);
        ui::text(kItems[row].title, kMargin + 100, y + 6, TFT_WHITE);
    };

    // Стираем по нарисованному номеру, а не по предыдущему выбору: между
    // кадрами могло пройти несколько нажатий.
    if (_drawn_selected >= 0 && _drawn_selected < kItemCount && _drawn_selected != _selected)
        paint(_drawn_selected, false);

    paint(_selected, true);
    _drawn_selected = _selected;
}

}  // namespace ui
