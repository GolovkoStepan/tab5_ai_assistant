// История введённых вопросов — без железа.
//
// Подробности и обоснование решений — в include/util/input_history.h.

#include "util/input_history.h"

#include <string.h>

namespace inputhist {

namespace {

/// Слот записи с заданным возрастом (0 — самая свежая).
///
/// Кольцо: самая свежая запись в slots[head], предыдущая — на один слот назад.
/// Отдельного индекса «старейшей» нет намеренно: он выводится из head и count,
/// а два независимых индекса кольца — это ровно то место, где они расходятся.
size_t slot_of(const History& h, size_t age)
{
    return (static_cast<size_t>(h.head) + kMaxEntries - age) % kMaxEntries;
}

char* draft_of(History& h)
{
    return h.arena + kMaxEntries * kEntryCap;
}

}  // namespace

bool init(History& h, char* arena, size_t arena_bytes)
{
    if (arena == nullptr || arena_bytes < kArenaBytes) {
        h = History{};
        return false;
    }

    h.arena = arena;
    h.count = 0;
    h.head = 0;
    h.cursor = 0;
    h.draft_len = 0;
    h.draft_saved = false;
    // len[] не обнуляем: записи недоступны, пока count == 0, а 20 значений по
    // 4 байта — это не та память, ради которой стоит проходить по арене.
    return true;
}

void clear(History& h)
{
    h.count = 0;
    h.head = 0;
    h.cursor = 0;
    h.draft_len = 0;
    h.draft_saved = false;
}

bool add(History& h, const char* text, size_t len)
{
    if (h.arena == nullptr || text == nullptr || len == 0) return false;

    // Не влез — не запоминаем вовсе. Обрезать нельзя: вытащенный из истории
    // покалеченный вопрос ушёл бы в модель, и ответ был бы не тот.
    if (len >= kEntryCap) return false;

    // Тот же вопрос, что и предыдущий: не плодим запись. Спрашивают одно и то
    // же дважды, но двадцать слотов на один вопрос — расточительно.
    const size_t newest = slot_of(h, 0);
    if (h.count > 0 && h.len[newest] == len && memcmp(h.arena + newest * kEntryCap, text, len) == 0) {
        // Возвращаем листание к черновику: сейчас в поле ничего нет.
        h.cursor = 0;
        h.draft_saved = false;
        h.draft_len = 0;
        return true;
    }

    h.head = static_cast<uint8_t>((h.head + 1) % kMaxEntries);
    char* slot = h.arena + static_cast<size_t>(h.head) * kEntryCap;
    memcpy(slot, text, len);
    slot[len] = '\0';
    h.len[h.head] = len;

    if (h.count < kMaxEntries) h.count = static_cast<uint8_t>(h.count + 1);

    // Вопрос отправлен, поле очищено — листание начинается заново.
    h.cursor = 0;
    h.draft_saved = false;
    h.draft_len = 0;
    return true;
}

const char* up(History& h, const char* draft, size_t draft_len, size_t* out_len)
{
    if (out_len != nullptr) *out_len = 0;
    if (h.arena == nullptr || h.count == 0) return nullptr;

    if (h.cursor == 0) {
        // Первый шаг: сохраняем черновик, чтобы Alt+↓ его вернул. Без этого
        // случайное Alt+↑ съедало бы недописанный вопрос, и вернуть его на
        // клавиатуре было бы нечем.
        if (draft != nullptr && draft_len > 0 && draft_len < kEntryCap) {
            memcpy(draft_of(h), draft, draft_len);
            draft_of(h)[draft_len] = '\0';
            h.draft_len = draft_len;
            h.draft_saved = true;
        } else {
            // Пустое поле — возвращать нечего, но и «пустой черновик» помнить
            // надо: Alt+↓ должен вернуть пустоту, а не первый же вопрос.
            h.draft_len = 0;
            h.draft_saved = true;
        }
    }

    // Дальше вверх некуда — самое старое уже на экране.
    if (h.cursor >= h.count) return nullptr;

    h.cursor = static_cast<uint8_t>(h.cursor + 1);
    const size_t slot = slot_of(h, static_cast<size_t>(h.cursor) - 1);
    if (out_len != nullptr) *out_len = h.len[slot];
    return h.arena + slot * kEntryCap;
}

const char* down(History& h, size_t* out_len)
{
    if (out_len != nullptr) *out_len = 0;
    if (h.arena == nullptr || h.cursor == 0) return nullptr;

    h.cursor = static_cast<uint8_t>(h.cursor - 1);
    if (h.cursor == 0) {
        // Вернулись к черновику. Если его не сохраняли (история листалась не
        // с первого шага — так бывает после смены треда), вернуть нечего.
        if (!h.draft_saved) return nullptr;
        if (out_len != nullptr) *out_len = h.draft_len;
        return draft_of(h);
    }

    const size_t slot = slot_of(h, static_cast<size_t>(h.cursor) - 1);
    if (out_len != nullptr) *out_len = h.len[slot];
    return h.arena + slot * kEntryCap;
}

bool at_draft(const History& h)
{
    return h.cursor == 0;
}

void to_draft(History& h)
{
    h.cursor = 0;
    h.draft_len = 0;
    h.draft_saved = false;
}

size_t size(const History& h)
{
    return h.count;
}

}  // namespace inputhist
