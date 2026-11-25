// Редактирование текста с кареткой: реализация чистых операций.
//
// Почему всё на байтовых смещениях, а не на индексах символов. Буфер лежит на
// карте как UTF-8, и любая операция — это memcpy или memmove по байтам.
// Пересчёт «символ → байт» на каждый ввод стоил бы прохода по всему тексту, а
// заметка бывает на 60 КБ. Единственное место, где счёт идёт в символах, —
// столбец каретки при движении по строкам, и там проход в пределах строки.

#include "util/text_edit.h"

#include <string.h>

namespace textedit {
namespace {

/// Записать обратную операцию. История кольцевая не нужна: она короткая, и
/// сдвиг на 32 элемента по 48 байт — это 1.5 КБ, дешевле любой арифметики с
/// головой и хвостом.
void push_undo(Editor& ed, const UndoOp& op)
{
    if (ed.undo_count == kMaxUndo) {
        // История полна — старейшая операция уходит. Так редактор продолжает
        // работать после сотни нажатий, а не перестаёт отменять вовсе.
        memmove(&ed.undo[0], &ed.undo[1], (kMaxUndo - 1) * sizeof(UndoOp));
        --ed.undo_count;
    }
    ed.undo[ed.undo_count++] = op;
}

}  // namespace

size_t char_len(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    // Битая последовательность: считаем одним байтом, иначе зациклимся.
    return 1;
}

size_t prev_char(const char* s, size_t pos)
{
    if (s == nullptr || pos == 0) return 0;
    --pos;
    // Отступаем назад, пока не встанем на ведущий байт. Максимум 3 шага:
    // UTF-8 не длиннее 4 байт.
    size_t guard = 0;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80 && guard < 3) {
        --pos;
        ++guard;
    }
    return pos;
}

size_t next_char(const char* s, size_t pos, size_t len)
{
    if (s == nullptr || pos >= len) return len;
    const size_t step = char_len(static_cast<unsigned char>(s[pos]));
    const size_t next = pos + step;
    return next < len ? next : len;
}

void clamp_caret(Editor& ed)
{
    // Буфер мог не привязаться (load вернул false) — тогда каретке некуда
    // вставать, и без этой проверки первое же нажатие стрелки читало бы
    // nullptr.
    if (ed.data == nullptr) {
        ed.caret = 0;
        ed.len = 0;
        return;
    }
    if (ed.caret > ed.len) ed.caret = ed.len;
    // Каретка между байтами многобайтного символа сдвигает его половину —
    // встаём на начало символа.
    while (ed.caret > 0 && (static_cast<unsigned char>(ed.data[ed.caret]) & 0xC0) == 0x80)
        --ed.caret;
}

bool load(Editor& ed, char* buf, size_t cap, const char* text, size_t text_len)
{
    ed.data = buf;
    ed.cap = cap;
    ed.len = 0;
    ed.caret = 0;
    ed.undo_count = 0;

    if (buf == nullptr || cap == 0) return false;
    if (text == nullptr) text_len = 0;
    if (text_len + 1 > cap) {
        buf[0] = '\0';
        return false;
    }

    // Текст может уже лежать в самом буфере: open_doc() читает тело прямо в
    // _text и передаёт его сюда. memcpy с перекрытием — неопределённое
    // поведение, поэтому копируем только если указатели разные.
    if (text_len > 0) {
        if (text != buf) memmove(buf, text, text_len);
    }
    buf[text_len] = '\0';
    ed.len = text_len;
    return true;
}

bool insert(Editor& ed, const char* text, size_t text_len)
{
    if (ed.data == nullptr || text == nullptr || text_len == 0) return false;
    if (ed.len + text_len + 1 > ed.cap) return false;

    clamp_caret(ed);
    memmove(ed.data + ed.caret + text_len, ed.data + ed.caret, ed.len - ed.caret);
    memcpy(ed.data + ed.caret, text, text_len);
    ed.len += text_len;
    ed.data[ed.len] = '\0';

    UndoOp op;
    op.kind = UndoOp::Kind::Insert;
    op.pos = ed.caret;
    op.len = text_len;
    push_undo(ed, op);

    ed.caret += text_len;
    return true;
}

bool insert_char(Editor& ed, const char* utf8)
{
    if (utf8 == nullptr || utf8[0] == '\0') return false;
    return insert(ed, utf8, strlen(utf8));
}

bool backspace(Editor& ed)
{
    if (ed.data == nullptr || ed.caret == 0) return false;

    clamp_caret(ed);
    const size_t from = prev_char(ed.data, ed.caret);
    const size_t n = ed.caret - from;

    UndoOp op;
    op.kind = UndoOp::Kind::Erase;
    op.pos = from;
    op.len = n;
    // Сам текст нужен, чтобы вернуть его при отмене. Символ не длиннее 4 байт,
    // kMaxUndoText — с запасом.
    op.text_len = n <= kMaxUndoText ? n : kMaxUndoText;
    memcpy(op.text, ed.data + from, op.text_len);
    push_undo(ed, op);

    memmove(ed.data + from, ed.data + ed.caret, ed.len - ed.caret);
    ed.len -= n;
    ed.data[ed.len] = '\0';
    ed.caret = from;
    return true;
}

bool delete_forward(Editor& ed)
{
    if (ed.data == nullptr) return false;
    clamp_caret(ed);
    if (ed.caret >= ed.len) return false;

    const size_t to = next_char(ed.data, ed.caret, ed.len);
    const size_t n = to - ed.caret;

    UndoOp op;
    op.kind = UndoOp::Kind::Erase;
    op.pos = ed.caret;
    op.len = n;
    op.text_len = n <= kMaxUndoText ? n : kMaxUndoText;
    memcpy(op.text, ed.data + ed.caret, op.text_len);
    push_undo(ed, op);

    memmove(ed.data + ed.caret, ed.data + to, ed.len - to);
    ed.len -= n;
    ed.data[ed.len] = '\0';
    return true;
}

bool delete_to_line_start(Editor& ed)
{
    if (ed.data == nullptr) return false;
    clamp_caret(ed);
    const size_t from = line_start(ed);
    if (from >= ed.caret) return false;

    const size_t n = ed.caret - from;

    // Обратная операция хранит только kMaxUndoText байт, а стирать можем
    // целую строку. Отмена такого удаления вернула бы обрывок — хуже, чем
    // отсутствие отмены, потому что пользователь считает текст целым.
    // Поэтому длинное удаление идёт без записи в историю.
    if (n <= kMaxUndoText) {
        UndoOp op;
        op.kind = UndoOp::Kind::Erase;
        op.pos = from;
        op.len = n;
        op.text_len = n;
        memcpy(op.text, ed.data + from, n);
        push_undo(ed, op);
    } else {
        ed.undo_count = 0;
    }

    memmove(ed.data + from, ed.data + ed.caret, ed.len - ed.caret);
    ed.len -= n;
    ed.data[ed.len] = '\0';
    ed.caret = from;
    return true;
}

void move_left(Editor& ed)
{
    clamp_caret(ed);
    ed.caret = prev_char(ed.data, ed.caret);
}

void move_right(Editor& ed)
{
    clamp_caret(ed);
    ed.caret = next_char(ed.data, ed.caret, ed.len);
}

size_t line_start(const Editor& ed)
{
    if (ed.data == nullptr) return 0;
    size_t p = ed.caret < ed.len ? ed.caret : ed.len;
    while (p > 0 && ed.data[p - 1] != '\n')
        --p;
    return p;
}

size_t line_end(const Editor& ed)
{
    if (ed.data == nullptr) return 0;
    size_t p = ed.caret < ed.len ? ed.caret : ed.len;
    while (p < ed.len && ed.data[p] != '\n')
        ++p;
    return p;
}

bool undo(Editor& ed)
{
    if (ed.data == nullptr || ed.undo_count == 0) return false;

    const UndoOp& op = ed.undo[ed.undo_count - 1];

    if (op.kind == UndoOp::Kind::Insert) {
        // Была вставка len байт в pos — стираем их.
        if (op.pos + op.len > ed.len) return false;
        memmove(ed.data + op.pos, ed.data + op.pos + op.len, ed.len - op.pos - op.len);
        ed.len -= op.len;
        ed.data[ed.len] = '\0';
        ed.caret = op.pos;
    } else {
        // Было стирание text_len байт из pos — возвращаем их.
        if (ed.len + op.text_len + 1 > ed.cap) return false;
        memmove(ed.data + op.pos + op.text_len, ed.data + op.pos, ed.len - op.pos);
        memcpy(ed.data + op.pos, op.text, op.text_len);
        ed.len += op.text_len;
        ed.data[ed.len] = '\0';
        ed.caret = op.pos + op.text_len;
    }

    --ed.undo_count;
    return true;
}

void clear_undo(Editor& ed)
{
    ed.undo_count = 0;
}

}  // namespace textedit
