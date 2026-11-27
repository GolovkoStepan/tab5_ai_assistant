// Экран списка тредов.
//
// Список перечитывается с карты при каждом входе: треды появляются и с
// компьютера, и из самого приложения. Читается только начало файла — заголовок,
// поэтому десяток тредов открывается за доли секунды.
//
// Удаление требует подтверждения вторым нажатием и привязано к имени треда, а
// не к номеру строки: список может перечитаться, и удалять надо тот тред,
// который пользователь видел перед собой.
//
// Экран не открывает тред сам: он выставляет запрос, а переход в чат делает
// main. Так экран не зависит от ChatScreen.

#include "ui/threads_screen.h"

#include <stdio.h>
#include <string.h>

#include "ui/draw.h"
#include "ui/tray.h"
#include "util/chat_list.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 16;
constexpr int kTitleY = 12;
constexpr int kListY = 76;
constexpr int kRowH = 34;
/// Сколько строк видно. Окно списка кончается выше нижней полосы (76 +
/// 15·34 = 586), иначе перерисовка списка стирала бы её верх.
constexpr int kVisibleRows = 15;

/// Нижняя полоса: подтверждение удаления и ошибка. Начинается ровно там, где
/// кончается список (76 + 15·34 = 586): щель между ними осталась бы от
/// предыдущего экрана — на плате в ней висело поле ввода чата («Вопрос…»).
constexpr int kFooterY = kListY + kVisibleRows * kRowH;
constexpr int kErrorY = kFooterY + 34;

/// Ширина полосы прокрутки. Три пикселя на экране 1280 — это почти невидимая
/// нить, а ползунок по ней ещё и делится на части перерисовкой строк.
constexpr int kScrollW = 6;

/// Где кончается содержимое строки и начинается полоса.
///
/// Строки заливаются ровно до этой границы: раньше заливка шла на всю ширину и
/// стирала полосу на тех строках, которые перерисовывались (при смене выбора),
/// а draw_scrollbar() после них не вызывался — в полосе появлялись «дыры»,
/// прыгавшие вместе с курсором. Столбец полосы теперь принадлежит только ей.
int scrollbar_x()
{
    return M5.Display.width() - kScrollW;
}

constexpr uint16_t kColorSelected = 0x0320;  // тёмно-зелёный
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorBad = TFT_ORANGE;
constexpr uint16_t kColorFrame = 0x18E3;

bool is_char(const hw::KeyEvent& ev)
{
    return ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] != '\0';
}

/// Ctrl+N — новый тред. «т» — та же клавиша в ЙЦУКЕН.
bool is_ctrl_n(const hw::KeyEvent& ev)
{
    return ev.ctrl && is_char(ev) && (strcmp(ev.utf8, "n") == 0 || strcmp(ev.utf8, "т") == 0);
}

}  // namespace

void ThreadsScreen::init(hw::ChatStore& store)
{
    _store = &store;
}

void ThreadsScreen::onEnter()
{
    _selected = 0;
    _first = 0;
    _armed = -1;
    _armed_slug[0] = '\0';
    _armed_title[0] = '\0';
    _error[0] = '\0';
    _notice_ok = false;
    _selected_slug[0] = '\0';
    _open_request = false;
    _new_request = false;
    _open_removed = false;
    _list_valid = false;

    reload();

    _need_clear = true;
    _redraw_all = true;
}

void ThreadsScreen::onExit()
{
    // Взведённое удаление не должно пережить уход с экрана: иначе следующее
    // открытие списка позволило бы стереть тред одним нажатием Del.
    _armed = -1;
    _armed_slug[0] = '\0';
    _armed_title[0] = '\0';
}

void ThreadsScreen::reload()
{
    _count = 0;
    if (_store == nullptr) return;

    // Пустой список — не ошибка: тредов могло не быть вообще. Отличить
    // недоступную SD здесь нечем, поэтому экран просто предложит создать
    // первый тред.
    _count = _store->list_threads(_items, hw::ChatStore::kMaxListedThreads);

    if (_selected >= static_cast<int>(_count)) _selected = _count > 0 ? static_cast<int>(_count) - 1 : 0;
    if (_selected < 0) _selected = 0;
    clamp_scroll();
}

void ThreadsScreen::draw_selection()
{
    // Перерисовываем обе строки: ту, что нарисована подсвеченной, и новую.
    // Обе — по их НОМЕРАМ в _items, а не по запомненным экранным строкам: окно
    // могло сдвинуться, и прежний номер теперь лежит на другой высоте.
    //
    // Ширина заливки в draw_row() кончается до полосы прокрутки, поэтому сама
    // полоса от этого не страдает и перерисовывать её здесь не нужно.
    draw_row(_drawn_selected - _first, true);
    draw_row(_selected - _first, true);
    _drawn_selected = _selected;
}

void ThreadsScreen::clamp_scroll()
{
    const int count = static_cast<int>(_count);
    if (count <= kVisibleRows) {
        _first = 0;
        return;
    }
    if (_first > _selected) _first = _selected;
    if (_selected >= _first + kVisibleRows) _first = _selected - kVisibleRows + 1;
    if (_first < 0) _first = 0;
    if (_first > count - kVisibleRows) _first = count - kVisibleRows;
}

bool ThreadsScreen::onKey(const hw::KeyEvent& ev)
{
    if (_store == nullptr) return true;

    if (ev.type == hw::KeyEvent::Type::Special && ev.special == keymap::Special::Escape) {
        // Взведённое удаление снимается, и только второе Esc выходит с экрана:
        // одно нажатие не должно одновременно отменять удаление и уводить.
        if (_armed >= 0) {
            _armed = -1;
            _armed_slug[0] = '\0';
            _armed_title[0] = '\0';
            _error[0] = '\0';
            _notice_ok = false;
            _redraw_all = true;
            return false;
        }
        return true;
    }

    if (is_ctrl_n(ev)) {
        _new_request = true;
        return false;
    }

    switch (ev.type) {
    case hw::KeyEvent::Type::Special:
        switch (ev.special) {
        case keymap::Special::Up:
            // Выбор и окно меняются, а рисует их draw(): он сам решит, хватает
            // ли перерисовать две строки или нужен сдвиг изображения.
            if (_selected > 0) {
                --_selected;
                clamp_scroll();
            }
            return false;
        case keymap::Special::Down:
            if (_selected + 1 < static_cast<int>(_count)) {
                ++_selected;
                clamp_scroll();
            }
            return false;
        case keymap::Special::Enter:
        case keymap::Special::Space:
            if (_count > 0) {
                snprintf(_selected_slug, sizeof(_selected_slug), "%s", _items[_selected].slug);
                _open_request = true;
            }
            return false;
        case keymap::Special::Delete: remove_selected(_armed == _selected); return false;
        default: return false;
        }

    default: return false;
    }
}

bool ThreadsScreen::onTouch(int x, int y)
{
    (void)x;
    if (_store == nullptr) return true;
    if (y < kListY || y >= kListY + kVisibleRows * kRowH) return false;

    const int row = _first + (y - kListY) / kRowH;
    if (row < 0 || row >= static_cast<int>(_count)) return false;

    // Первый тап выбирает строку, повторный по той же — открывает. Так список
    // работает и без клавиатуры, и случайное касание не уводит из экрана.
    if (row == _selected) {
        snprintf(_selected_slug, sizeof(_selected_slug), "%s", _items[_selected].slug);
        _open_request = true;
        return true;
    }

    _selected = row;
    clamp_scroll();
    return false;
}

void ThreadsScreen::remove_selected(bool confirmed)
{
    if (_count == 0) return;

    const hw::ChatStore::ThreadInfo& item = _items[_selected];

    if (!confirmed) {
        snprintf(_armed_slug, sizeof(_armed_slug), "%s", item.slug);
        snprintf(_armed_title, sizeof(_armed_title), "%s", item.title[0] != '\0' ? item.title : item.slug);
        _armed = _selected;
        _error[0] = '\0';
        _notice_ok = false;
        _redraw_all = true;
        return;
    }

    // Удаляем именно подтверждённый тред, а не тот, что сейчас на строке:
    // список мог перечитаться, и номера строк уже другие.
    //
    // Имя копируем в ЛОКАЛЬНЫЙ буфер: дальше _armed_slug обнуляется, и
    // указатель на него стал бы пустой строкой — remove() отверг бы её как
    // недопустимое имя, и на экране появилось бы «Не удалось удалить» без
    // самого имени.
    char slug[48];
    snprintf(slug, sizeof(slug), "%s", _armed_slug[0] != '\0' ? _armed_slug : item.slug);

    // Открытый тред во время ответа удалять нельзя: удаление чистит арену, в
    // которую в этот момент пишется текст ответа. Проверяем ДО снятия
    // подтверждения, чтобы оно не осталось висеть «взведённым».
    if (_chat_busy && strcmp(_store->slug(), slug) == 0) {
        _armed = -1;
        _armed_slug[0] = '\0';
        _armed_title[0] = '\0';
        _error[0] = '\0';
        snprintf(_error, sizeof(_error), "Идёт ответ в этом треде — удалить нельзя");
        _notice_ok = false;
        _redraw_all = true;
        return;
    }

    _armed = -1;
    _armed_slug[0] = '\0';
    _armed_title[0] = '\0';

    // Открытый тред надо определить ДО удаления: remove() сам обнуляет имя,
    // если удалили именно его, и после вызова «пусто» уже ничего не значит
    // (оно бывает и когда открытого треда не было вовсе).
    const bool was_open = strcmp(_store->slug(), slug) == 0;

    if (!_store->remove(slug)) {
        _error[0] = '\0';
        snprintf(_error, sizeof(_error), "Не удалось удалить %s", slug);
        _notice_ok = false;
        _redraw_all = true;
        return;
    }

    // remove() сам сбрасывает открытый тред, если удалили именно его. Новый
    // создаёт ChatScreen: ему же надо сбросить ленту и поле ввода, иначе на
    // экране остался бы разговор, которого больше нет на карте.
    if (was_open) _open_removed = true;

    reload();

    _error[0] = '\0';
    snprintf(_error, sizeof(_error), "Тред удалён");
    _notice_ok = true;
    _redraw_all = true;
}

const char* ThreadsScreen::take_selected_slug()
{
    // Пусто, если открытие не запрашивали: у вызывающего единственный признак
    // «есть запрос» — непустое имя. Иначе любое нажатие Enter открывало бы
    // строку, даже когда список только что перечитался.
    return _open_request ? _selected_slug : "";
}

void ThreadsScreen::clear_request()
{
    _selected_slug[0] = '\0';
    _open_request = false;
}

bool ThreadsScreen::take_new_request()
{
    const bool was = _new_request;
    _new_request = false;
    return was;
}

bool ThreadsScreen::take_open_removed()
{
    const bool was = _open_removed;
    _open_removed = false;
    return was;
}

void ThreadsScreen::notice(const char* text)
{
    fail(text != nullptr ? text : "");
}

void ThreadsScreen::fail(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text);
    _notice_ok = false;
    _redraw_all = true;
}

void ThreadsScreen::draw()
{
    if (_need_clear) {
        // Экран мог показывать чат с полем ввода и статусом. Чистим целиком
        // один раз при входе: перечислить все щели между областями можно, но
        // одна забытая полоска останется висеть чужим текстом — так «Вопрос…»
        // из поля ввода чата и оказался в списке тредов.
        M5.Display.fillScreen(TFT_BLACK);
        _need_clear = false;
    }

    if (_redraw_all) {
        draw_header();
        draw_list();
        draw_footer();
        draw_hints();
        _redraw_all = false;
        return;
    }

    // Окно сдвинулось — прокрутка сдвигом готового изображения. Перерисовка
    // пятнадцати строк (заливка области плюс полсотни надписей с фоном) стоит
    // десятки миллисекунд, а автоповтор стрелки в клавиатуре — 80 мс: экран не
    // успевал дорисоваться, и список мерцал.
    if (_list_valid && _drawn_first != _first) {
        if (shift_list()) return;

        // Сдвиг не сложился (список стал короче, окно уехало дальше строки) —
        // рисуем заново. Заголовок тоже: число тредов могло измениться.
        draw_header();
        draw_list();
        return;
    }

    // Окно стоит, а выбор переехал — две строки.
    if (_drawn_selected != _selected) draw_selection();
}

void ThreadsScreen::draw_header()
{
    M5.Display.fillRect(0, 0, M5.Display.width(), kListY, TFT_BLACK);

    char left[64];
    snprintf(left, sizeof(left), "Треды: %u", static_cast<unsigned>(_count));
    ui::text(left, kMargin, kTitleY, TFT_WHITE);

    // Открытый тред — справа: так видно, какой разговор сейчас в чате, и что
    // удаление относится не обязательно к нему.
    //
    // Правее — трей, поэтому подпись прижимается к его левой границе, а не к
    // краю экрана: иначе они бы накладывались.
    if (_store != nullptr && _store->slug()[0] != '\0') {
        char right[96];
        snprintf(right, sizeof(right), "открыт: %s", _store->slug());
        ui::text_right(right, ui::tray_left_edge(M5.Display.width()), kTitleY, kColorFaint);
    }

    ui::draw_tray(M5.Display.width());
}

void ThreadsScreen::draw_list()
{
    const int list_h = kVisibleRows * kRowH;

    if (_count == 0) {
        M5.Display.fillRect(0, kListY, scrollbar_x(), list_h, TFT_BLACK);
        ui::text("Тредов нет.", kMargin, kListY + 20, kColorDim);
        ui::text("Ctrl+N — создать первый.", kMargin, kListY + 48, kColorFaint);
        _drawn_selected = _selected;
        remember_list();
        return;
    }

    // Сплошной заливки области нет: каждая строка заливается перед своим
    // текстом. Заливка 1280×510 стоит десятки миллисекунд, и всё это время на
    // месте списка пусто — панель сканирует единственный фреймбуфер.
    int rows = 0;
    for (int i = 0; i < kVisibleRows; ++i) {
        if (_first + i >= static_cast<int>(_count)) break;
        draw_row(i, true);
        ++rows;
    }

    // Список стал короче — хвост области надо почистить, иначе останутся
    // строки прежнего списка.
    if (rows < kVisibleRows) {
        const int from = kListY + rows * kRowH;
        M5.Display.fillRect(0, from, scrollbar_x(), kListY + list_h - from, TFT_BLACK);
    }

    draw_scrollbar();
    _drawn_selected = _selected;
    remember_list();
}

void ThreadsScreen::remember_list()
{
    _drawn_first = _first;
    _drawn_count = _count;
    _list_valid = true;
}

bool ThreadsScreen::shift_list()
{
    if (!_list_valid) return false;
    if (_drawn_count != _count) return false;
    if (_drawn_first == _first) return false;

    // Сдвиг имеет смысл только когда список длиннее окна: иначе строки прижаты
    // к верху, и сдвиг разъехался бы с содержимым.
    if (_count <= static_cast<size_t>(kVisibleRows)) return false;

    // Окно сдвигается ровно на одну строку: так работает клавиатурный выбор, и
    // изображение при этом уезжает на kRowH.
    const int dy = _first - _drawn_first;
    if (dy != 1 && dy != -1) return false;

    const int shift = dy * kRowH;
    const int list_h = kVisibleRows * kRowH;

    // Копируем только содержимое, не задевая столбец полосы: иначе на время
    // кадра полоса уехала бы вместе с текстом. Ползунок всё равно надо
    // перерисовать — его положение зависит от _first.
    const int shift_w = scrollbar_x();

    if (dy > 0) {
        // Окно ушло вниз по списку (показаны более старые строки ниже):
        // изображение уезжает ВВЕРХ, снизу открывается новая строка.
        const int d = shift;
        M5.Display.copyRect(0, kListY, shift_w, list_h - d, 0, kListY + d);
        draw_row(kVisibleRows - 1, true);
    } else {
        // Окно ушло вверх по списку: изображение уезжает ВНИЗ, сверху
        // открывается новая строка.
        const int d = -shift;
        M5.Display.copyRect(0, kListY + d, shift_w, list_h - d, 0, kListY);
        draw_row(0, true);
    }

    // Подсветка уехала вместе с изображением: она нарисована на строке, которая
    // теперь принадлежит другому треду. Обе строки — прежнюю и новую —
    // перерисовываем уже по новым местам.
    draw_selection();

    draw_scrollbar();
    remember_list();
    return true;
}

void ThreadsScreen::draw_row(int row, bool fill_bg)
{
    if (row < 0 || row >= kVisibleRows) return;

    const int idx = _first + row;
    if (idx < 0 || idx >= static_cast<int>(_count)) return;

    const hw::ChatStore::ThreadInfo& item = _items[idx];
    const bool sel = (idx == _selected);
    const int y = kListY + row * kRowH;

    // Строку заливаем перед текстом: иначе надписи накладывались бы на хвосты
    // прошлого содержимого (заголовок бывает длиннее нового). Ширина — до
    // полосы прокрутки: её столбец эта функция не трогает никогда, иначе
    // перерисовка двух строк при смене выбора оставляла бы в полосе дыры.
    if (fill_bg) {
        M5.Display.fillRect(0, y, scrollbar_x(), kRowH, TFT_BLACK);
    }

    if (sel) {
        // Подсветка строго внутри строки, без выхода за её верхнюю границу:
        // рамка с y−3 залезала на предыдущую строку, и при снятии выбора её
        // верхние 3 px оставались висеть зелёной полоской — та самая «рамка»
        // на строках, которые уже не выбраны.
        M5.Display.fillRect(kMargin - 6, y, M5.Display.width() - 2 * kMargin + 12, kRowH - 2, kColorSelected);
    }

    // Столбцы фиксированные, чтобы строки не «плясали»: маркер, дата, заголовок,
    // размер.
    ui::text(sel ? ">" : " ", kMargin, y, sel ? TFT_YELLOW : kColorFaint);

    chatlist::Stamp stamp;
    stamp.year = item.year;
    stamp.month = item.month;
    stamp.day = item.day;
    stamp.hour = item.hour;
    stamp.minute = item.minute;
    stamp.real = item.time_known;

    char date[24];
    date[0] = '\0';
    chatlist::format_stamp(stamp, date, sizeof(date));
    ui::text(date, kMargin + 18, y, item.time_known ? kColorDim : kColorFaint);

    // Размер файла — правый столбец. Его ширину вычитаем из места под заголовок
    // до отрисовки: иначе длинный вопрос наезжал бы на размер.
    char size[24];
    size[0] = '\0';
    int size_room = 0;
    if (item.bytes > 0) {
        if (item.bytes >= 1024) {
            snprintf(size, sizeof(size), "%u КБ", static_cast<unsigned>(item.bytes / 1024));
        } else {
            snprintf(size, sizeof(size), "%u Б", static_cast<unsigned>(item.bytes));
        }
        M5.Display.setFont(ui::font());
        size_room = M5.Display.textWidth(size) + 16;
    }

    // Заголовок: если первый вопрос не разобрался, показываем имя треда — по
    // нему всё равно видно, когда разговор был.
    const int title_x = kMargin + 18 + 92;
    const char* title = item.title[0] != '\0' ? item.title : item.slug;
    static char title_buf[112];
    const int title_room = M5.Display.width() - kMargin - size_room - title_x;
    ui::text(ui::fit_width(title, title_room, title_buf, sizeof(title_buf)), title_x, y,
        item.title[0] != '\0' ? TFT_WHITE : kColorFaint);

    if (size[0] != '\0') ui::text_right(size, M5.Display.width() - kMargin, y, kColorFaint);
}

void ThreadsScreen::draw_scrollbar()
{
    const int track_top = kListY;
    const int track_h = kVisibleRows * kRowH;
    const int x = scrollbar_x();

    // Сам столбец чистим всегда: после сдвига изображения ползунок с прежнего
    // места остался бы висеть, если полоса стала не нужна.
    M5.Display.fillRect(x, track_top, kScrollW, track_h, TFT_BLACK);
    if (_count <= static_cast<size_t>(kVisibleRows)) return;

    M5.Display.fillRect(x, track_top, kScrollW, track_h, kColorFrame);

    int thumb_h = static_cast<int>(static_cast<long>(track_h) * kVisibleRows / static_cast<long>(_count));
    if (thumb_h < 20) thumb_h = 20;

    const int travel = track_h - thumb_h;
    const int max_first = static_cast<int>(_count) - kVisibleRows;
    const int thumb_y = track_top + (max_first > 0 ? travel * _first / max_first : 0);

    M5.Display.fillRect(x, thumb_y, kScrollW, thumb_h, kColorDim);
}

void ThreadsScreen::draw_footer()
{
    // Вся полоса разом: подтверждение и ошибка лежат рядом, и чистить их по
    // отдельности значило бы затирать соседнюю строку чёрным прямоугольником.
    M5.Display.fillRect(0, kFooterY, M5.Display.width(), ui::kHintY - 8 - kFooterY, TFT_BLACK);

    if (_armed >= 0) {
        char text[160];
        snprintf(text, sizeof(text), "Удалить «%s»? Del — ещё раз, Esc — отмена", _armed_title);
        static char armed_buf[176];
        ui::text(ui::fit_width(text, M5.Display.width() - 2 * kMargin, armed_buf, sizeof(armed_buf)), kMargin, kFooterY,
            kColorBad);
    }

    if (_error[0] != '\0') {
        static char err_buf[176];
        ui::text(ui::fit_width(_error, M5.Display.width() - 2 * kMargin, err_buf, sizeof(err_buf)), kMargin, kErrorY,
            _notice_ok ? kColorDim : kColorBad);
    }
}

void ThreadsScreen::draw_hints()
{
    M5.Display.fillRect(0, ui::kHintY - 8, M5.Display.width(), 34, TFT_BLACK);
    ui::text("↑↓ — выбор · Enter — открыть · Ctrl+N — новый · Del — удалить · Esc — в чат", kMargin, ui::kHintY,
        kColorFaint);
}

}  // namespace ui
