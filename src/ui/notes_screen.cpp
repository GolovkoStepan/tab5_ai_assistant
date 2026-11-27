// Экран архива документов: список, поиск, просмотр, редактор.
//
// Четыре режима в одном классе, а не четыре экрана: переход «список → документ
// → правка» — это один сценарий, и отдельные классы пришлось бы связывать
// указателями друг на друга ради общего буфера текста на 64 КБ.
//
// Список ищется и читается с карты при входе. Текст документа — один буфер в
// PSRAM: просмотр и редактор смотрят в него же, поэтому переход в правку ничего
// не перечитывает и не может показать другую версию файла.
//
// Арифметика каретки лежит в textedit/textlayout (тесты на хосте). Здесь —
// только файлы, состояния и отрисовка.

#include "ui/notes_screen.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "hw/clock.h"
#include "ui/draw.h"
#include "ui/tray.h"
#include "util/list_page.h"
#include "util/text_layout.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMargin = 16;
constexpr int kHeaderY = 10;

/// Список: строки по 34 px, как в списке тредов.
constexpr int kListY = 76;
constexpr int kRowH = 34;
constexpr int kVisibleRows = 15;

/// Текст документа: строки по 22 px, как в ленте чата.
constexpr int kTextY = 52;
constexpr int kLineH = 22;

/// Нижняя граница текста: 52 + 24·22 = 580.
///
/// Полоса состояния начинается ровно там, где кончается список (76 + 15·34 =
/// 586): оставлять между ними зазор нельзя — он не попадает ни в одну область
/// очистки, и в нём оставалась бы полоска прежнего экрана.
constexpr int kRowsY = 586;
constexpr int kStatusY = 586;
constexpr int kErrorY = 622;

/// Сколько строк текста видно: между полями заголовка и нижней полосой.
constexpr int kTextRows = (kRowsY - kTextY) / kLineH;

/// Ширина полосы прокрутки списка.
constexpr int kScrollW = 6;

constexpr uint16_t kColorSelected = 0x0320;  // тёмно-зелёный
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorBad = TFT_ORANGE;
constexpr uint16_t kColorFrame = 0x18E3;
constexpr uint16_t kColorCaret = TFT_CYAN;

bool is_char(const hw::KeyEvent& ev)
{
    return ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] != '\0';
}

bool is_special(const hw::KeyEvent& ev, keymap::Special s)
{
    return ev.type == hw::KeyEvent::Type::Special && ev.special == s;
}

/// Ctrl+<клавиша> с учётом ЙЦУКЕН: на клавише «s» в русской раскладке «ы».
bool is_ctrl(const hw::KeyEvent& ev, const char* latin, const char* russian)
{
    if (!ev.ctrl || !is_char(ev)) return false;
    return strcmp(ev.utf8, latin) == 0 || strcmp(ev.utf8, russian) == 0;
}

/// Копия строки для отрисовки: без завершающего перевода строки.
///
/// Табуляция рисуется ОДНИМ пробелом, а не четырьмя. Это не экономия: раскладка
/// (textlayout) считает `\t` за один столбец, и если нарисовать его четырьмя,
/// каретка на строке с табуляцией уезжала бы влево от символа под ней. Клавиша
/// Tab в редакторе вставляет пробелы, так что в новых файлах табуляции не
/// появляется вовсе; в правленых на компьютере она остаётся в файле как есть.
///
/// Перевод строки в строку не входит, и хвост оборванного UTF-8 отсекается:
/// разрезанная кириллица дала бы ромбик.
void expand_line(const char* src, size_t len, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;
    if (src == nullptr) {
        out[0] = '\0';
        return;
    }

    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        const char c = src[i];
        if (c == '\n' || c == '\r') break;

        // Копируем СИМВОЛ целиком и только если он влезает: обрезка посреди
        // UTF-8 дала бы ромбик вместо последней буквы строки.
        const size_t step = c == '\t' ? 1 : textedit::char_len(static_cast<unsigned char>(c));
        if (n + step + 1 > out_len) break;

        if (c == '\t') {
            out[n++] = ' ';
            ++i;
            continue;
        }

        // Символ мог оказаться битым (ведущий байт без достаточного числа
        // продолжений) — тогда не копируем его вовсе.
        if (i + step > len) break;
        memcpy(out + n, src + i, step);
        n += step;
        i += step;
    }

    out[n] = '\0';
}

int scrollbar_x()
{
    return M5.Display.width() - kScrollW;
}

/// Ширина одного глифа в пикселях — по кириллице, как в ленте чата.
int glyph_w()
{
    M5.Display.setFont(ui::font());
    const int w = M5.Display.textWidth("ШШШШШШШШШШ") / 10;
    return w > 0 ? w : 13;
}

size_t chars_per_line()
{
    const int w = glyph_w();
    const int room = M5.Display.width() - 2 * kMargin;
    const size_t n = room > 0 ? static_cast<size_t>(room / w) : 80;
    return n > 0 ? n : 1;
}

}  // namespace

// --- Жизненный цикл ---------------------------------------------------------

void NotesScreen::init(hw::DocStore& docs, hw::SettingsStore& settings)
{
    _docs = &docs;
    _settings = &settings;

    // Буфер текста и смещения строк — в PSRAM: 64 КБ и 4 КБ на стеке задачи
    // интерфейса (8 КБ) не поместятся, а раскладка пересчитывается на каждый
    // введённый символ.
    _text = static_cast<char*>(heap_caps_malloc(kMaxDocBytes + 1, MALLOC_CAP_SPIRAM));
    if (_text == nullptr) _text = static_cast<char*>(malloc(kMaxDocBytes + 1));

    _row_offsets = static_cast<uint32_t*>(heap_caps_malloc((kMaxRows + 1) * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    if (_row_offsets == nullptr) _row_offsets = static_cast<uint32_t*>(malloc((kMaxRows + 1) * sizeof(uint32_t)));

    if (_text == nullptr || _row_offsets == nullptr) Serial.println("Заметки: не хватило памяти под буферы");
}

void NotesScreen::onEnter()
{
    _mode = Mode::List;
    _confirm = ConfirmKind::None;
    _selected = 0;
    _first = 0;
    _scroll = 0;
    _error[0] = '\0';
    _notice_ok = false;
    _query[0] = '\0';
    _from_search = false;
    _list_valid = false;
    _dirty = false;
    _rel[0] = '\0';
    _meta = doc::Meta{};

    reload();

    _need_clear = true;
    _redraw_all = true;
}

void NotesScreen::onExit()
{
    // Уйти с несохранённой правкой отсюда нельзя: main блокирует команды смены
    // экрана, пока has_unsaved_edits(), а Esc в редакторе спрашивает
    // подтверждение. Так что сюда мы попадаем уже без правок, и сброс ниже —
    // страховка на случай, если экран переключат в обход (новый код в main).
    _dirty = false;
    _confirm = ConfirmKind::None;
    _mode = Mode::List;
}

bool NotesScreen::dirty() const
{
    if (_redraw_all || _redraw_text || _redraw_caret || _redraw_footer) return true;
    if (_mode != Mode::List && _mode != Mode::Search) return false;

    // Выбор и окно списка меняются без флага перерисовки: стрелка двигает
    // выделение, а draw() сам решает, хватит ли двух строк. Поэтому «грязно»
    // определяется ещё и расхождением с тем, что нарисовано.
    return !_list_valid || _drawn_selected != _selected || _drawn_first != _first;
}

// --- Список -----------------------------------------------------------------

void NotesScreen::reload()
{
    _page_offset = 0;
    _total = 0;
    _from_search = false;
    reload_page();
}

void NotesScreen::reload_page()
{
    _count = 0;
    _list_valid = false;
    if (_docs == nullptr || _row_offsets == nullptr || !_docs->ready()) {
        _total = 0;
        return;
    }

    _count = _docs->list_from("", _page_offset, _items, kMaxListedDocs);

    // Сколько ВСЕГО документов: list_from() копит его в _total_found, даже
    // когда страница короткая. Без этого заголовок врал бы «Документы: 60»
    // про архив из 137 файлов, и человек считал бы, что остальных нет.
    _total = _docs->total_found();

    if (_total == 0) _page_offset = 0;
    if (_selected >= static_cast<int>(_count)) _selected = _count > 0 ? static_cast<int>(_count) - 1 : 0;
    if (_selected < 0) _selected = 0;
    clamp_scroll();

    // В журнал — счётом, а не глазами: «страница перевернулась и показала
    // другие документы» иначе проверяется только у экрана (Р-046).
    Serial.printf("Архив: показано %u из %u, со смещения %u\n", static_cast<unsigned>(shown_count()),
        static_cast<unsigned>(_total), static_cast<unsigned>(_page_offset));
}

bool NotesScreen::has_next_page() const
{
    // У результатов поиска страниц нет: search() читает весь архив и отдаёт
    // первые kMaxListedDocs совпадений. Разрешить здесь страницу значило бы,
    // что → из результатов молча показала бы страницу ОБЫЧНОГО списка, и
    // найденное пропало бы с экрана без объяснения.
    if (_from_search) return false;
    return shown_count() < _total;
}

void NotesScreen::next_page()
{
    if (!has_next_page()) return;

    const size_t step = _count > 0 ? _count : kMaxListedDocs;
    const size_t next = listpage::next_offset(_total, _page_offset, step);
    if (next == _page_offset) return;

    _page_offset = next;
    reload_page();
    // После переворота страницы выделяем первую строку: иначе выбор остался бы
    // на той же высоте, но уже другой записи, и Enter открыл бы не то, что
    // человек видел.
    _selected = 0;
    _first = 0;
    _redraw_all = true;
}

void NotesScreen::prev_page()
{
    if (!has_prev_page()) return;

    const size_t step = kMaxListedDocs;
    _page_offset = _page_offset > step ? _page_offset - step : 0;
    reload_page();
    // Назад — в конец страницы, чтобы переход выглядел как прокрутка, а не
    // как прыжок в начало.
    _selected = _count > 0 ? static_cast<int>(_count) - 1 : 0;
    clamp_scroll();
    _redraw_all = true;
}

void NotesScreen::clamp_scroll()
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

void NotesScreen::open_selected()
{
    if (_count == 0) return;
    open_doc(_items[_selected].path);
}

bool NotesScreen::open_doc(const char* rel)
{
    if (_docs == nullptr || _text == nullptr) {
        fail("Не хватило памяти под документ");
        return false;
    }
    if (rel == nullptr || rel[0] == '\0') return false;

    doc::Meta meta;
    size_t body_len = 0;
    char err[160];
    if (!_docs->read(rel, meta, _text, kMaxDocBytes, &body_len, err, sizeof(err))) {
        fail(err[0] != '\0' ? err : "Документ не читается");
        return false;
    }

    snprintf(_rel, sizeof(_rel), "%s", rel);
    _meta = meta;

    if (!textedit::load(_ed, _text, kMaxDocBytes + 1, _text, body_len)) {
        fail("Документ не помещается в буфер");
        return false;
    }

    _dirty = false;
    _scroll = 0;
    _desired_col = 0;
    _mode = Mode::View;
    _error[0] = '\0';
    _notice_ok = false;

    after_edit(false);
    _scroll = 0;
    _redraw_all = true;

    return true;
}

// --- Редактор ---------------------------------------------------------------

void NotesScreen::after_edit(bool text_changed)
{
    if (_row_offsets == nullptr) return;

    const size_t width = chars_per_line();
    _rows = static_cast<int>(textlayout::layout(_ed.data, _ed.len, width, _row_offsets, kMaxRows));
    if (_rows < 1) _rows = 1;

    // Каретка могла уехать за окно и без правки — прокручиваем всегда.
    size_t row = 0;
    size_t col = 0;
    textlayout::caret_row_col(_ed.data, _ed.len, _ed.caret, width, &row, &col);

    const int first_before = _scroll;
    if (static_cast<int>(row) < _scroll) _scroll = static_cast<int>(row);
    if (static_cast<int>(row) >= _scroll + kTextRows) _scroll = static_cast<int>(row) - kTextRows + 1;

    const int max_scroll = _rows > kTextRows ? _rows - kTextRows : 0;
    if (_scroll < 0) _scroll = 0;
    if (_scroll > max_scroll) _scroll = max_scroll;

    // Строка состояния показывает строку и столбец каретки, поэтому
    // перерисовывается при любом движении, а не только при правке.
    _redraw_footer = true;

    if (text_changed) {
        _dirty = true;
        // Столбец, к которому тянулись стрелки вверх/вниз, больше не имеет
        // смысла: текст изменился, и каретка стоит там, куда её поставил ввод.
        _desired_col = 0;
        // Прошлая ошибка («достигнут предел 64 КБ») относилась к прежнему
        // состоянию: текст снова правится, и висеть она не должна.
        if (!_notice_ok) _error[0] = '\0';
        // Правка сдвигает всё ниже каретки (перенос по ширине), поэтому
        // перерисовываем окно целиком. Частичной перерисовки здесь быть не
        // может: одна вставленная буква переразбивает абзац.
        _redraw_text = true;
        return;
    }

    if (_scroll != first_before) {
        _redraw_text = true;
        return;
    }

    // Переехала только каретка — перерисовываем её одну, и старую тоже: стереть
    // голубую черту на прежнем месте иначе нечем.
    _redraw_caret = true;
}

void NotesScreen::begin_edit()
{
    if (_ed.data == nullptr) return;
    _mode = Mode::Edit;
    // Каретка в начало: правка начинается с осмотра, а не с конца текста.
    _ed.caret = 0;
    _desired_col = 0;
    _scroll = 0;
    _redraw_all = true;
}

void NotesScreen::flush_pending_save()
{
    // Сохраняем только если правили правда что-то: _dirty ставится при
    // изменении текста, а не при открытии файла. Иначе выключение переписывало
    // бы документ без причины — и «updated» менялся бы просто так.
    if (!_dirty || _mode != Mode::Edit) return;
    save_edit();
}

void NotesScreen::save_edit()
{
    if (_docs == nullptr || _ed.data == nullptr) return;

    if (_rel[0] == '\0') {
        fail("Документ не открыт");
        return;
    }

    // Заголовок — из фронтматтера, который мы сохранили при открытии. Если его
    // не было (документ без фронтматтера), берём имя файла: без заголовка
    // DocStore::write откажет, и заметка перестала бы сохраняться.
    doc::Meta meta = _meta;

    // Заметка, созданная руками, живёт в inbox/ со служебным именем — название
    // человек пишет в теле (Р-050). Заголовок обновляется из первой строки при
    // каждом сохранении: правка текста должна менять и название в списке.
    bool title_from_body = paths::is_inbox(_rel);
    if (title_from_body) {
        if (doc::title_from_body(_ed.data, _ed.len, meta.title, sizeof(meta.title)) == 0) {
            // Тело пустое — названия нет. Подставляем дату: без заголовка
            // DocStore::write откажет, и заметка перестала бы сохраняться.
            char ts[32];
            if (_settings != nullptr && hw::clock_sync::now_iso(_settings->config(), ts, sizeof(ts))) {
                // «2026-11-27T16:15:00+03:00» → «Заметка 2026-11-27 16:15».
                snprintf(meta.title, sizeof(meta.title), "Заметка %.10s %.5s", ts, ts + 11);
            } else {
                snprintf(meta.title, sizeof(meta.title), "Заметка");
            }
        }
    } else if (meta.title[0] == '\0') {
        // Без заголовка DocStore::write откажет, и заметка перестала бы
        // сохраняться. Берём имя файла, но без «.md»: «air-purifier.md» в
        // списке читалось бы как имя файла, а не как заголовок.
        const char* base = strrchr(_rel, '/');
        base = base != nullptr ? base + 1 : _rel;
        snprintf(meta.title, sizeof(meta.title), "%s", base);
        char* dot = strrchr(meta.title, '.');
        if (dot != nullptr && dot != meta.title) *dot = '\0';
    }

    char ts[32];
    if (_settings != nullptr && hw::clock_sync::now_iso(_settings->config(), ts, sizeof(ts))) {
        snprintf(meta.updated, sizeof(meta.updated), "%s", ts);
        if (meta.created[0] == '\0') snprintf(meta.created, sizeof(meta.created), "%s", ts);
    }

    bool created = false;
    char err[200];
    if (!_docs->write(_rel, meta, _ed.data, &created, err, sizeof(err))) {
        fail(err[0] != '\0' ? err : "Не удалось сохранить документ");
        return;
    }

    _meta = meta;
    _dirty = false;
    textedit::clear_undo(_ed);

    // Индекс — сразу, а не при выходе: файл уже на карте, и INDEX.md должен ему
    // соответствовать в ту же секунду.
    char idx_err[96];
    if (!_docs->rebuild_index(idx_err, sizeof(idx_err))) {
        Serial.printf("Заметки: индекс не пересобрался — %s\n", idx_err);
    }

    // Список устарел: дата обновления и заголовок в нём могли измениться.
    reload();

    notice(created ? "Документ создан" : "Сохранено");
    _redraw_all = true;
}

void NotesScreen::request_leave_edit()
{
    if (_dirty) {
        _confirm = ConfirmKind::DiscardEdits;
        _mode = Mode::Confirm;
        _redraw_footer = true;
        return;
    }

    _mode = Mode::View;
    after_edit(false);
    _redraw_all = true;
}

// --- Клавиатура -------------------------------------------------------------

bool NotesScreen::onKey(const hw::KeyEvent& ev)
{
    if (_docs == nullptr || _text == nullptr) return true;

    if (_mode == Mode::Confirm) {
        // Вопрос необратимый: случайная клавиша не должна ни удалить файл, ни
        // оставить пользователя без ответа.
        if (is_special(ev, keymap::Special::Enter) || (is_char(ev) && ev.utf8[0] == 'y')) {
            resolve_confirm(true);
        } else if (is_special(ev, keymap::Special::Escape)) {
            resolve_confirm(false);
        }
        return false;
    }

    if (is_special(ev, keymap::Special::Escape)) {
        switch (_mode) {
        case Mode::List:
            if (_from_search) {
                // Подсказка обещает «Esc — полный список», и после пустого
                // поиска это единственный способ вернуться: выход в чат здесь
                // читался бы как потеря архива.
                _query[0] = '\0';
                reload();
                _redraw_all = true;
                return false;
            }
            return true;  // выйти из экрана
        case Mode::Search:
            _mode = Mode::List;
            if (_from_search) {
                // Искали — Esc возвращает полный список, как обещает подсказка.
                _query[0] = '\0';
                reload();
            }
            _redraw_all = true;
            return false;
        case Mode::View:
            _mode = Mode::List;
            _list_valid = false;
            _redraw_all = true;
            return false;
        case Mode::Edit: request_leave_edit(); return false;
        case Mode::Confirm: return false;
        }
        return false;
    }

    // --- Поиск ---
    if (_mode == Mode::Search) {
        if (is_special(ev, keymap::Special::Enter)) {
            if (_query[0] == '\0') {
                // Пустой запрос — не поиск, а возврат к полному списку.
                _mode = Mode::List;
                reload();
            } else {
                run_search();
            }
            _redraw_all = true;
            return false;
        }
        if (is_special(ev, keymap::Special::Backspace)) {
            const size_t n = strlen(_query);
            if (n > 0) {
                const size_t from = textedit::prev_char(_query, n);
                _query[from] = '\0';
            }
            _redraw_all = true;
            return false;
        }
        if (is_special(ev, keymap::Special::Space)) {
            const size_t n = strlen(_query);
            if (n + 2 < sizeof(_query)) {
                _query[n] = ' ';
                _query[n + 1] = '\0';
            }
            _redraw_all = true;
            return false;
        }
        if (is_char(ev)) {
            const size_t n = strlen(_query);
            const size_t add = strlen(ev.utf8);
            if (n + add < sizeof(_query)) memcpy(_query + n, ev.utf8, add + 1);
            _redraw_all = true;
            return false;
        }
        return false;
    }

    // --- Редактор ---
    if (_mode == Mode::Edit) {
        if (is_ctrl(ev, "s", "ы")) {
            save_edit();
            return false;
        }
        if (is_ctrl(ev, "z", "я")) {
            if (textedit::undo(_ed)) after_edit(true);
            return false;
        }

        bool changed = false;

        if (is_char(ev)) {
            if (!textedit::insert_char(_ed, ev.utf8)) {
                // Буфер полон — это предел документа, а не ошибка ввода.
                fail("Документ достиг предела 64 КБ");
                return false;
            }
            after_edit(true);
            return false;
        }

        switch (ev.special) {
        case keymap::Special::Space: changed = textedit::insert_char(_ed, " "); break;
        case keymap::Special::Enter: changed = textedit::insert_char(_ed, "\n"); break;
        // Табуляция вставляется пробелами: раскладка считает символы, и
        // «настоящий» \t дал бы каретке и тексту разные системы координат.
        case keymap::Special::Tab: changed = textedit::insert(_ed, "    ", 4); break;
        case keymap::Special::Backspace:
            // Ctrl+Backspace стирает до начала строки.
            changed = ev.ctrl ? textedit::delete_to_line_start(_ed) : textedit::backspace(_ed);
            break;
        case keymap::Special::Delete: changed = textedit::delete_forward(_ed); break;
        case keymap::Special::Left:
            textedit::move_left(_ed);
            _desired_col = 0;
            break;
        case keymap::Special::Right:
            textedit::move_right(_ed);
            _desired_col = 0;
            break;
        case keymap::Special::Up:
        case keymap::Special::Down: {
            // Столбец запоминаем между нажатиями: иначе, пройдя короткую строку,
            // каретка не вернулась бы на прежнее место.
            if (_desired_col == 0) {
                size_t r = 0;
                textlayout::caret_row_col(_ed.data, _ed.len, _ed.caret, chars_per_line(), &r, &_desired_col);
            }
            _ed.caret = textlayout::move_vertical(_ed.data, _ed.len, _ed.caret, chars_per_line(),
                is_special(ev, keymap::Special::Up) ? -1 : 1, _desired_col);
            break;
        }
        default: return false;
        }

        after_edit(changed);
        return false;
    }

    // --- Просмотр ---
    if (_mode == Mode::View) {
        if (is_special(ev, keymap::Special::Enter) || is_ctrl(ev, "e", "у")) {
            begin_edit();
            return false;
        }
        if (is_special(ev, keymap::Special::Up)) {
            if (_scroll > 0) {
                --_scroll;
                _redraw_text = true;
            }
            return false;
        }
        if (is_special(ev, keymap::Special::Down)) {
            const int max_scroll = _rows > kTextRows ? _rows - kTextRows : 0;
            if (_scroll < max_scroll) {
                ++_scroll;
                _redraw_text = true;
            }
            return false;
        }
        return false;
    }

    // --- Список ---
    // Ctrl+N — новая заметка (Р-050). «т» — та же клавиша в ЙЦУКЕН.
    if (is_ctrl(ev, "n", "т")) {
        new_note();
        return false;
    }

    // «/» — поиск. В ЙЦУКЕН на этой клавише «.» (символ «ю»), поэтому
    // принимаем оба: подсказка обещает поиск по «/», а набрать его в русской
    // раскладке нельзя.
    if (is_char(ev) && (ev.utf8[0] == '/' || strcmp(ev.utf8, "ю") == 0)) {
        _mode = Mode::Search;
        _query[0] = '\0';
        _error[0] = '\0';
        _notice_ok = false;
        _redraw_all = true;
        return false;
    }

    if (is_special(ev, keymap::Special::Up)) {
        if (_selected > 0) {
            --_selected;
            // _list_valid НЕ сбрасываем: пока окно стоит, достаточно
            // перерисовать две строки (прежнюю и новую). Если выбор ушёл за
            // окно, draw() сам увидит, что первая строка сдвинулась, и
            // перерисует список целиком.
            clamp_scroll();
        }
        return false;
    }
    if (is_special(ev, keymap::Special::Down)) {
        if (_selected + 1 < static_cast<int>(_count)) {
            ++_selected;
            clamp_scroll();
        }
        return false;
    }
    if (is_special(ev, keymap::Special::Enter) || is_special(ev, keymap::Special::Space)) {
        open_selected();
        return false;
    }
    // ← → — страницы списка (v1.1.0). Стрелки вверх/вниз двигают выбор, а
    // страницы нужны, когда документов больше, чем влезает в список: до этого
    // остаток архива был недостижим вообще.
    if (is_special(ev, keymap::Special::Right)) {
        if (has_next_page()) {
            next_page();
        } else if (_from_search) {
            // Совпадений больше, чем влезает в список, а листать их нечем:
            // говорим об этом прямо, иначе «→ ничего не делает» читалось бы
            // как поломка.
            if (_total > _count) {
                notice("Совпадений больше, чем влезает — уточните запрос");
            }
        } else {
            notice("Это последняя страница архива");
        }
        return false;
    }
    if (is_special(ev, keymap::Special::Left)) {
        if (has_prev_page()) {
            prev_page();
        } else if (!_from_search) {
            notice("Это первая страница архива");
        }
        return false;
    }
    if (is_special(ev, keymap::Special::Delete)) {
        if (_count > 0) {
            _confirm = ConfirmKind::Delete;
            _mode = Mode::Confirm;
            _redraw_footer = true;
        }
        return false;
    }
    return false;
}

bool NotesScreen::make_inbox_path(char* out, size_t out_len)
{
    char ts[32];
    if (_settings == nullptr || !hw::clock_sync::now_iso(_settings->config(), ts, sizeof(ts))) return false;
    return paths::inbox_path(ts, "note", out, out_len);
}

void NotesScreen::new_note()
{
    if (_docs == nullptr || _text == nullptr || _ed.data == nullptr) {
        fail("Не хватило памяти под документ");
        return;
    }

    char rel[paths::kMaxPathLen];
    if (!make_inbox_path(rel, sizeof(rel))) {
        // Без времени в имени две заметки подряд получили бы одно имя, и вторая
        // молча затёрла бы первую. Лучше отказать, чем потерять текст.
        fail("Часы не выставлены: заметку не назвать. Ctrl+W — сеть для NTP");
        return;
    }

    // Проверяем, что такого файла ещё нет: создание через N+1 секунду могло
    // совпасть с именем, а write() перезаписал бы чужую заметку.
    if (_docs->exists(rel)) {
        fail("Файл с таким временем уже есть — повторите через секунду");
        return;
    }

    textedit::load(_ed, _text, kMaxDocBytes + 1, "", 0);

    snprintf(_rel, sizeof(_rel), "%s", rel);
    _meta = doc::Meta{};

    // Заголовок и дата создадутся при сохранении: сейчас тело пустое, и
    // придумывать название за человека незачем.
    _dirty = false;
    _scroll = 0;
    _desired_col = 0;
    _mode = Mode::Edit;
    _from_search = false;
    _error[0] = '\0';
    _notice_ok = false;

    after_edit(false);
    _scroll = 0;
    _redraw_all = true;

    notice("Новая заметка · напишите название первой строкой, Ctrl+S — сохранить");
}

void NotesScreen::run_search()
{
    if (_docs == nullptr) return;

    // Результаты кладём в тот же массив: сниппет становится заголовком строки.
    // Читать заголовки найденных файлов значило бы ещё N открытий SD ради
    // того, что и так видно в сниппете.
    static docapi::Hit hits[docapi::kMaxListedDocs];
    const size_t found = _docs->search(_query, docapi::kMaxListedDocs, hits, docapi::kMaxListedDocs);

    _count = 0;
    for (size_t i = 0; i < found && _count < kMaxListedDocs; ++i) {
        docapi::Entry& e = _items[_count++];
        e = docapi::Entry{};
        snprintf(e.path, sizeof(e.path), "%s", hits[i].path);
        snprintf(e.title, sizeof(e.title), "%s", hits[i].snippet);
    }

    _selected = 0;
    _first = 0;
    _from_search = true;
    _list_valid = false;
    _mode = Mode::List;

    // Поиск отдаёт не больше kMaxListedDocs и о страницах не знает: он читает
    // весь архив и обрезает результат. Поэтому offset обнуляем — иначе «N из M»
    // прибавило бы смещение полного списка к результатам поиска.
    //
    // Всего совпадений берём из total_found(), а не из _count: search() считает
    // их ВСЕ, включая не влезшие в страницу. Иначе «Документы: 60 из 60» врало
    // бы про архив, где совпадений 137, и человек считал бы, что остальных нет.
    _page_offset = 0;
    const size_t matches = _docs->total_found();
    _total = matches > _count ? matches : _count;

    Serial.printf("Архив: поиск «%s» — совпадений %u, показано %u\n", _query, static_cast<unsigned>(_total),
        static_cast<unsigned>(_count));

    if (_count == 0) {
        notice("Ничего не найдено · Esc — полный список");
    } else if (_total > _count) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Найдено: %u, показано %u · Esc — полный список", static_cast<unsigned>(found),
            static_cast<unsigned>(_count));
        notice(msg);
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "Найдено: %u · Esc — полный список", static_cast<unsigned>(_count));
        notice(msg);
    }
    _redraw_all = true;
}

// --- Подтверждения ----------------------------------------------------------

void NotesScreen::resolve_confirm(bool confirmed)
{
    const ConfirmKind kind = _confirm;
    _confirm = ConfirmKind::None;

    if (kind == ConfirmKind::DiscardEdits) {
        if (confirmed) {
            // Перечитываем файл, а не просто снимаем флаг: в буфере лежит
            // ПРАВЛЕНЫЙ текст, и просмотр показал бы несохранённое — то есть
            // «выйти без сохранения» ничего бы не отменило.
            _dirty = false;
            _mode = Mode::View;
            if (!open_doc(_rel)) {
                // Файл успели удалить или карта отвалилась — в список, там
                // причина уже показана.
                _mode = Mode::List;
                _list_valid = false;
            }
            _redraw_all = true;
        } else {
            _mode = Mode::Edit;
            _redraw_all = true;
        }
        return;
    }

    if (kind == ConfirmKind::Delete) {
        confirm_delete(confirmed);
        return;
    }

    _mode = Mode::List;
    _redraw_footer = true;
}

void NotesScreen::confirm_delete(bool confirmed)
{
    if (!confirmed) {
        _mode = Mode::List;
        _redraw_all = true;
        return;
    }

    if (_docs == nullptr || _count == 0) {
        _mode = Mode::List;
        return;
    }

    char rel[paths::kMaxPathLen];
    snprintf(rel, sizeof(rel), "%s", _items[_selected].path);

    char err[200];
    if (!_docs->remove(rel, err, sizeof(err))) {
        _mode = Mode::List;
        fail(err[0] != '\0' ? err : "Не удалось удалить документ");
        return;
    }

    // Индекс пересобираем сразу: иначе INDEX.md перечислял бы удалённый файл.
    char idx_err[96];
    if (!_docs->rebuild_index(idx_err, sizeof(idx_err))) {
        Serial.printf("Заметки: индекс не пересобрался — %s\n", idx_err);
    }

    // Открытый документ удалили — выходим из просмотра, иначе редактор правил бы
    // текст, которого больше нет на карте.
    if (strcmp(_rel, rel) == 0) {
        _rel[0] = '\0';
        _meta = doc::Meta{};
    }

    reload();
    _mode = Mode::List;

    char msg[128];
    snprintf(msg, sizeof(msg), "Удалён %s", rel);
    notice(msg);
    _redraw_all = true;
}

// --- Касания ----------------------------------------------------------------

bool NotesScreen::onTouch(int x, int y)
{
    (void)x;
    if (_mode != Mode::List) return false;
    if (y < kListY || y >= kListY + kVisibleRows * kRowH) return false;

    const int row = _first + (y - kListY) / kRowH;
    if (row < 0 || row >= static_cast<int>(_count)) return false;

    if (row == _selected) {
        open_selected();
        return true;
    }

    _selected = row;
    clamp_scroll();
    return false;
}

// --- Отрисовка --------------------------------------------------------------

void NotesScreen::draw()
{
    if (_need_clear) {
        // Экран мог показывать чат: стираем целиком один раз, иначе в щелях
        // между областями останется «Вопрос…» из его поля ввода.
        M5.Display.fillScreen(TFT_BLACK);
        _need_clear = false;
    }

    // Флаги снимаются ОДНИМ местом в конце. Ветки ниже не должны возвращаться
    // раньше: флаг, выставленный в другом режиме (каретка — в правке, текст — в
    // просмотре), иначе пережил бы смену режима и оставил бы dirty() вечно
    // истинным — цикл перерисовывал бы экран без изменений.
    if (_mode == Mode::List || _mode == Mode::Search) {
        if (_redraw_all) {
            // Полоса состояния и ошибки: в списке статуса нет, но на этом месте
            // осталась бы строка «строка 3, столбец 5» из просмотра, из
            // которого мы сюда вернулись.
            M5.Display.fillRect(0, kStatusY, M5.Display.width(), ui::kHintY - 8 - kStatusY, TFT_BLACK);
            draw_header();
            draw_list();
            draw_footer();
            draw_hints();
        } else {
            // Окно сдвинулось или список перечитался — рисуем заново:
            // документов немного, а перерисовка пятнадцати строк стоит десятки
            // миллисекунд.
            if (!_list_valid || _drawn_first != _first) {
                draw_header();
                draw_list();
            } else if (_drawn_selected != _selected) {
                draw_row(_drawn_selected - _first, true);
                draw_row(_selected - _first, true);
                _drawn_selected = _selected;
            }

            if (_redraw_footer) draw_footer();
        }
    } else if (_mode == Mode::View) {
        if (_redraw_all) {
            draw_header();
            draw_text(false);
            draw_status();
            draw_hints();
        } else if (_redraw_text) {
            draw_text(false);
            draw_status();
        } else if (_redraw_footer) {
            // Просмотр — единственный режим, где полоса состояния не рисуется
            // заодно с текстом, так что сообщение об ошибке надо вывести явно.
            draw_status();
        }
    } else if (_mode == Mode::Edit) {
        if (_redraw_all) {
            draw_header();
            draw_text(true);
            draw_status();
            draw_hints();
        } else {
            if (_redraw_text) {
                draw_text(true);
                draw_status();
            } else if (_redraw_caret) {
                // Переехала только каретка: две вертикальные черты вместо
                // двадцати четырёх строк текста.
                draw_caret();
            }
            if (_redraw_footer) draw_status();
        }
    } else {
        // Подтверждение рисуется в нижней полосе поверх прежнего содержимого.
        // Подсказки перерисовываем тоже: в подтверждении, открытом из
        // редактора, они продолжали бы обещать Ctrl+S, который здесь ничего не
        // делает.
        draw_footer();
        draw_hints();
    }

    _redraw_all = false;
    _redraw_text = false;
    _redraw_caret = false;
    _redraw_footer = false;
}

void NotesScreen::draw_header()
{
    // До начала списка без зазора: между ними оставалась бы полоска прежнего
    // экрана, заметная на тёмном фоне.
    M5.Display.fillRect(0, 0, M5.Display.width(), kListY, TFT_BLACK);

    static char buf[192];

    // Правее этой границы — трей. Все правые подписи шапки кончаются здесь,
    // иначе они наезжали бы на значок батареи.
    const int right_edge = ui::tray_left_edge(M5.Display.width());

    if (_mode == Mode::View || _mode == Mode::Edit) {
        const char* title = _meta.title[0] != '\0' ? _meta.title : _rel;
        const int room = right_edge - kMargin - 260;
        ui::text(ui::fit_width(title, room, buf, sizeof(buf)), kMargin, kHeaderY, TFT_WHITE);

        // Путь — справа: по заголовку не видно, в какой папке документ, а при
        // правке легко забыть, что правишь.
        // Путь в списке относительный и без ведущего «/», поэтому разделитель
        // ставим сами: склейка подряд дала бы «/noteshome/x.md».
        static char path[160];
        snprintf(path, sizeof(path), "%s/%s", cfg::kNotesDir, _rel);
        ui::text_right(ui::fit_width(path, 240, buf, sizeof(buf)), right_edge, kHeaderY, kColorFaint);
        ui::draw_tray(M5.Display.width());
        return;
    }

    if (_mode == Mode::Search) {
        snprintf(buf, sizeof(buf), "Поиск: %s", _query);
        ui::text(ui::fit_width(buf, right_edge - kMargin, _scratch, sizeof(_scratch)), kMargin, kHeaderY, TFT_WHITE);
        ui::draw_tray(M5.Display.width());
        return;
    }

    if (_from_search) {
        // «Найдено: 60 из 137» — иначе человек считал, что нашлось ровно 60,
        // хотя список обрезан пределом.
        listpage::label("Найдено", _total, _count, buf, sizeof(buf));
    } else {
        // «Документы: 60 из 137» и номер страницы, если она не первая.
        char* p = buf;
        size_t left = sizeof(buf);
        const size_t written = listpage::label("Документы", _total, shown_count(), p, left);
        if (written > 0 && _page_offset > 0) {
            p += written;
            left -= written;
            snprintf(p, left, " · с %u", static_cast<unsigned>(_page_offset + 1));
        }
    }
    ui::text(buf, kMargin, kHeaderY, TFT_WHITE);

    if (_docs != nullptr && !_docs->ready()) {
        ui::text_right("нет SD-карты", right_edge, kHeaderY, kColorBad);
    }

    ui::draw_tray(M5.Display.width());
}

void NotesScreen::draw_list()
{
    const int list_h = kVisibleRows * kRowH;

    // Пока вводится запрос, прежний список к нему не относится. Показывать его
    // под строкой «Поиск: филь» значило бы выдавать старые результаты за
    // найденные по этому запросу.
    if (_mode == Mode::Search) {
        M5.Display.fillRect(0, kListY, M5.Display.width(), kStatusY - kListY, TFT_BLACK);
        ui::text("Введите запрос и нажмите Enter.", kMargin, kListY + 20, kColorDim);
        ui::text("Ищем по заголовкам и тексту документов.", kMargin, kListY + 48, kColorFaint);
        _drawn_selected = _selected;
        _drawn_first = _first;
        _list_valid = true;
        return;
    }

    if (_count == 0) {
        M5.Display.fillRect(0, kListY, M5.Display.width(), kStatusY - kListY, TFT_BLACK);

        if (_docs != nullptr && !_docs->ready()) {
            ui::text("SD-карта не видна — архив пуст.", kMargin, kListY + 20, kColorBad);
            ui::text("Документы появятся, когда карта смонтируется.", kMargin, kListY + 48, kColorFaint);
        } else if (_from_search) {
            ui::text("Ничего не найдено.", kMargin, kListY + 20, kColorDim);
            ui::text("Esc — вернуться к полному списку.", kMargin, kListY + 48, kColorFaint);
        } else {
            ui::text("Архив пуст.", kMargin, kListY + 20, kColorDim);
            ui::text("Попросите агента в чате найти и сохранить информацию.", kMargin, kListY + 48, kColorFaint);
        }

        _drawn_selected = _selected;
        _drawn_first = _first;
        _list_valid = true;
        return;
    }

    int rows = 0;
    for (int i = 0; i < kVisibleRows; ++i) {
        if (_first + i >= static_cast<int>(_count)) break;
        draw_row(i, true);
        ++rows;
    }

    // Список стал короче — хвост области надо почистить, иначе останутся
    // строки прежнего списка.
    // Список стал короче — чистим остаток области списка, но не полосу
    // состояния: там может висеть уведомление, и стирать его при каждом
    // сдвиге выбора нельзя.
    if (rows < kVisibleRows) {
        const int from = kListY + rows * kRowH;
        M5.Display.fillRect(0, from, M5.Display.width(), kStatusY - from, TFT_BLACK);
    }

    draw_scrollbar();
    _drawn_selected = _selected;
    _drawn_first = _first;
    _list_valid = true;
}

void NotesScreen::draw_row(int row, bool fill_bg)
{
    if (row < 0 || row >= kVisibleRows) return;

    const int idx = _first + row;
    if (idx < 0 || idx >= static_cast<int>(_count)) return;

    const docapi::Entry& item = _items[idx];
    const bool sel = (idx == _selected);
    const int y = kListY + row * kRowH;

    if (fill_bg) M5.Display.fillRect(0, y, scrollbar_x(), kRowH, TFT_BLACK);

    if (sel) M5.Display.fillRect(kMargin - 6, y, M5.Display.width() - 2 * kMargin + 12, kRowH - 2, kColorSelected);

    ui::text(sel ? ">" : " ", kMargin, y, sel ? TFT_YELLOW : kColorFaint);

    // Дата обновления — фиксированный столбец, чтобы строки не «плясали».
    // В результатах поиска даты нет: там сниппет важнее.
    if (!_from_search) {
        ui::text(item.updated[0] != '\0' ? item.updated : "—", kMargin + 18, y,
            item.updated[0] != '\0' ? kColorDim : kColorFaint);
    }

    const int title_x = _from_search ? kMargin + 18 : kMargin + 130;
    const char* title = item.title[0] != '\0' ? item.title : item.path;
    const int room = M5.Display.width() - kMargin - title_x;
    static char buf[192];
    ui::text(ui::fit_width(title, room, buf, sizeof(buf)), title_x, y, TFT_WHITE);
}

void NotesScreen::draw_scrollbar()
{
    const int x = scrollbar_x();
    M5.Display.fillRect(x, kListY, kScrollW, kVisibleRows * kRowH, TFT_BLACK);
    if (_count <= static_cast<size_t>(kVisibleRows)) return;

    const int track_h = kVisibleRows * kRowH;
    M5.Display.fillRect(x, kListY, kScrollW, track_h, kColorFrame);

    int thumb_h = static_cast<int>(static_cast<long>(track_h) * kVisibleRows / static_cast<long>(_count));
    if (thumb_h < 20) thumb_h = 20;

    const int max_first = static_cast<int>(_count) - kVisibleRows;
    const int travel = track_h - thumb_h;
    const int thumb_y = kListY + (max_first > 0 ? travel * _first / max_first : 0);
    M5.Display.fillRect(x, thumb_y, kScrollW, thumb_h, kColorDim);
}

void NotesScreen::draw_text(bool with_caret)
{
    // Окно перерисовывается целиком, значит на экране каретки нет: забываем,
    // где она была, иначе draw_caret() попытается стереть её со старого места
    // и нарисует поверх текста кусок прежней строки.
    _drawn_caret_row = -1;
    _drawn_caret_col = -1;

    // Чистим до строки состояния, а не до последней строки сетки: между ними
    // 8 пикселей, и в них оставались бы обрезки строк списка, из которого
    // документ открыли.
    M5.Display.fillRect(0, kTextY, M5.Display.width(), kStatusY - kTextY, TFT_BLACK);

    static char line[512];
    for (int i = 0; i < kTextRows; ++i) {
        const int row = _scroll + i;
        if (row >= _rows) break;

        if (_row_offsets == nullptr) break;
        const size_t from = _row_offsets[row];
        const size_t to = row + 1 <= _rows ? _row_offsets[row + 1] : _ed.len;
        const size_t len = to > from ? to - from : 0;
        expand_line(_ed.data + from, len, line, sizeof(line));

        ui::text_plain(line, kMargin, kTextY + i * kLineH, with_caret ? TFT_WHITE : kColorDim);
    }

    // Каретку рисуем после текста: она поверх строки.
    if (with_caret) draw_caret();
}

void NotesScreen::draw_caret()
{
    const int gw = glyph_w();

    // Стереть прежнюю: черта стоит на месте вставки, и без этого она осталась
    // бы висеть на старом месте — выглядело бы как две каретки.
    if (_drawn_caret_row >= 0 && _drawn_caret_col >= 0) {
        const int old_y = kTextY + (_drawn_caret_row - _scroll) * kLineH;
        if (old_y >= kTextY && old_y < kTextY + kTextRows * kLineH) {
            // Перерисовываем строку под прежним курсором, а не заливаем фон:
            // заливка стёрла бы букву, которая там стоит.
            const int row = _drawn_caret_row;
            if (_row_offsets != nullptr && row >= 0 && row < _rows) {
                const size_t from = _row_offsets[row];
                const size_t to = row + 1 <= _rows ? _row_offsets[row + 1] : _ed.len;
                const size_t len = to > from ? to - from : 0;
                char line[512];
                expand_line(_ed.data + from, len, line, sizeof(line));
                M5.Display.fillRect(kMargin, old_y, M5.Display.width() - 2 * kMargin, kLineH, TFT_BLACK);
                ui::text_plain(line, kMargin, old_y, TFT_WHITE);
            }
        }
    }

    size_t caret_row = 0;
    size_t caret_col = 0;
    textlayout::caret_row_col(_ed.data, _ed.len, _ed.caret, chars_per_line(), &caret_row, &caret_col);

    const int y = kTextY + (static_cast<int>(caret_row) - _scroll) * kLineH;
    if (y >= kTextY && y < kTextY + kTextRows * kLineH) {
        const int x = kMargin + static_cast<int>(caret_col) * gw;
        M5.Display.fillRect(x, y, 2, kLineH, kColorCaret);
        _drawn_caret_row = static_cast<int>(caret_row);
        _drawn_caret_col = static_cast<int>(caret_col);
    } else {
        _drawn_caret_row = -1;
        _drawn_caret_col = -1;
    }
}

void NotesScreen::draw_status()
{
    M5.Display.fillRect(0, kStatusY, M5.Display.width(), ui::kHintY - 8 - kStatusY, TFT_BLACK);

    static char buf[176];

    if (_mode == Mode::View) {
        snprintf(buf, sizeof(buf), "%d строк · Enter — править · ↑↓ — прокрутка", _rows);
        ui::text(buf, kMargin, kStatusY, kColorFaint);
    } else {
        size_t row = 0;
        size_t col = 0;
        textlayout::caret_row_col(_ed.data, _ed.len, _ed.caret, chars_per_line(), &row, &col);
        snprintf(buf, sizeof(buf), "строка %u, столбец %u · %s", static_cast<unsigned>(row + 1),
            static_cast<unsigned>(col + 1), _dirty ? "не сохранено" : "сохранено");
        ui::text(buf, kMargin, kStatusY, _dirty ? kColorBad : kColorFaint);
    }

    // Сообщение об ошибке сохранения обязано быть видно и из редактора: без
    // этого Ctrl+S на полном буфере молча не срабатывал бы, и пользователь
    // считал бы текст сохранённым.
    if (_error[0] != '\0') {
        ui::text(ui::fit_width(_error, M5.Display.width() - 2 * kMargin, _scratch, sizeof(_scratch)), kMargin, kErrorY,
            _notice_ok ? kColorDim : kColorBad);
    }
}

void NotesScreen::draw_footer()
{
    M5.Display.fillRect(0, kErrorY, M5.Display.width(), ui::kHintY - 8 - kErrorY, TFT_BLACK);

    static char buf[192];

    if (_mode == Mode::Confirm) {
        // Две строки, а не одна: заголовок документа бывает длинным, и в одной
        // строке он вытеснял подсказку клавиш — пользователь видел вопрос без
        // ответа на него.
        const char* keys = "";
        if (_confirm == ConfirmKind::Delete && _count > 0) {
            snprintf(buf, sizeof(buf), "Удалить «%s»?",
                _items[_selected].title[0] != '\0' ? _items[_selected].title : _items[_selected].path);
            keys = "Enter — удалить · Esc — отмена";
        } else if (_confirm == ConfirmKind::DiscardEdits) {
            snprintf(buf, sizeof(buf), "Есть несохранённые правки. Выйти без сохранения?");
            keys = "Enter — выйти · Esc — остаться";
        } else {
            buf[0] = '\0';
        }
        ui::text(ui::fit_width(buf, M5.Display.width() - 2 * kMargin, _scratch, sizeof(_scratch)), kMargin, kErrorY,
            kColorBad);
        ui::text(keys, kMargin, kErrorY + 24, kColorDim);
        return;
    }

    if (_error[0] != '\0') {
        ui::text(ui::fit_width(_error, M5.Display.width() - 2 * kMargin, _scratch, sizeof(_scratch)), kMargin, kErrorY,
            _notice_ok ? kColorDim : kColorBad);
    }
}

void NotesScreen::draw_hints()
{
    M5.Display.fillRect(0, ui::kHintY - 8, M5.Display.width(), 34, TFT_BLACK);

    // В подтверждении клавиши написаны в самом вопросе, а подсказки прежнего
    // режима к нему не относятся.
    if (_mode == Mode::Confirm) return;

    if (_mode == Mode::Edit) {
        ui::text("Enter — новая строка · Ctrl+S — сохранить · Ctrl+Z — отмена · Esc — выйти", kMargin, ui::kHintY,
            kColorFaint);
        return;
    }
    if (_mode == Mode::Search) {
        ui::text("Введите запрос · Enter — искать · Esc — отмена", kMargin, ui::kHintY, kColorFaint);
        return;
    }
    if (_mode == Mode::View) {
        ui::text("Enter — править · ↑↓ — прокрутка · Esc — к списку", kMargin, ui::kHintY, kColorFaint);
        return;
    }
    // Подсказка о страницах — только когда они есть: на архиве из пяти
    // документов «← →» занимали бы место, ничего не делая.
    if (!_from_search && (_total > _count || _page_offset > 0)) {
        const size_t more = _total > shown_count() ? _total - shown_count() : 0;
        if (more > 0) {
            char hint[64];
            if (listpage::more_hint(more, hint, sizeof(hint)) > 0) {
                char line[160];
                snprintf(
                    line, sizeof(line), "↑↓ — выбор · Enter — открыть · %s · ← назад · / — поиск · Esc — в чат", hint);
                ui::text(line, kMargin, ui::kHintY, kColorFaint);
                return;
            }
        }
        ui::text("↑↓ — выбор · Enter — открыть · ← → — страницы · / — поиск · Esc — в чат", kMargin, ui::kHintY,
            kColorFaint);
        return;
    }

    ui::text("↑↓ — выбор · Enter — открыть · Ctrl+N — новая · / — поиск · Del — удалить · Esc — в чат", kMargin,
        ui::kHintY, kColorFaint);
}

void NotesScreen::fail(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text != nullptr ? text : "");
    _notice_ok = false;
    _redraw_footer = true;
}

void NotesScreen::notice(const char* text)
{
    // В подтверждении нижнюю полосу занимает вопрос, и подсказка от main его
    // бы не перебила (draw_footer в этом режиме _error не показывает) — то
    // есть сообщение просто пропало бы. Молча ничего не делаем.
    if (_mode == Mode::Confirm) return;

    snprintf(_error, sizeof(_error), "%s", text != nullptr ? text : "");
    _notice_ok = true;
    _redraw_footer = true;
}

}  // namespace ui
