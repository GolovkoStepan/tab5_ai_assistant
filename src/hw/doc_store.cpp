// Архив документов на SD: список, чтение, запись, поиск, индекс.
//
// Все пути на карте — от точки монтирования (`/notes/...`), см. грабли в
// docs/ARCHITECTURE.md §4.5: полный путь `/sdcard/notes/...` превратился бы в
// `/sdcard/sdcard/notes/...` и fopen вернул бы ENOENT.

#include "hw/doc_store.h"

#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "util/doc_search.h"

namespace hw {
namespace {

/// Буфер чтения один на весь класс: 64 КБ на стеке задачи цикла не помещаются,
/// а выделять и освобождать его на каждый документ — дырки в куче.
char* alloc_psram(size_t size)
{
    char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (p == nullptr) p = static_cast<char*>(malloc(size));
    return p;
}

/// Взять из даты `ГГГГ-ММ-ДД...` только дату для списка.
void date_only(const char* iso, char* out, size_t out_len)
{
    if (out_len == 0) return;
    out[0] = '\0';
    if (iso == nullptr || strlen(iso) < 10) return;
    // 10 символов «2026-11-23» плюс ноль.
    if (out_len < 11) return;
    memcpy(out, iso, 10);
    out[10] = '\0';
}

}  // namespace

void DocStore::make_path(const char* rel, char* out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", cfg::kNotesDir, rel);
}

bool DocStore::begin()
{
    // Каталог архива создаём один раз. mkdir на существующем каталоге просто
    // вернёт false — это не ошибка.
    if (!SD_MMC.exists(cfg::kNotesDir)) {
        if (!SD_MMC.mkdir(cfg::kNotesDir)) return false;
    }

    if (_buf == nullptr) _buf = alloc_psram(kMaxDocBytes + 1);
    if (_body == nullptr) _body = alloc_psram(kMaxDocBytes + 1);
    _ready = _buf != nullptr && _body != nullptr && ensure_entries();
    return _ready;
}

bool DocStore::ensure_entries()
{
    // 60 записей по ~250 байт — 15 КБ. На стеке задачи цикла это не помещается
    // (там ~8 КБ), поэтому массив живёт в PSRAM.
    if (_entries == nullptr) {
        _entries = static_cast<docapi::Entry*>(static_cast<void*>(alloc_psram(kMaxListedDocs * sizeof(docapi::Entry))));
    }
    return _entries != nullptr;
}

bool DocStore::ensure_folder(const char* rel_folder)
{
    if (rel_folder == nullptr || rel_folder[0] == '\0') return true;

    // Идём по сегментам: mkdir не создаёт родителей.
    char path[paths::kMaxPathLen];
    snprintf(path, sizeof(path), "%s", cfg::kNotesDir);
    size_t used = strlen(path);

    const char* p = rel_folder;
    while (*p != '\0') {
        const char* slash = strchr(p, '/');
        const size_t seg = (slash != nullptr) ? static_cast<size_t>(slash - p) : strlen(p);
        if (seg == 0) break;

        if (used + 1 + seg + 1 > sizeof(path)) return false;
        path[used++] = '/';
        memcpy(path + used, p, seg);
        used += seg;
        path[used] = '\0';

        // exists() на каталоге возвращает true, поэтому mkdir только когда нет.
        if (!SD_MMC.exists(path)) {
            if (!SD_MMC.mkdir(path)) return false;
        }

        if (slash == nullptr) break;
        p = slash + 1;
    }
    return true;
}

bool DocStore::read_file(
    const char* path, doc::Meta& meta, char* body, size_t body_cap, size_t* body_len, size_t* file_bytes)
{
    if (body != nullptr && body_len != nullptr) {
        body[0] = '\0';
        *body_len = 0;
    }

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    const size_t size = f.size();
    if (file_bytes != nullptr) *file_bytes = size;
    if (size > kMaxDocBytes) {
        f.close();
        return false;
    }

    const size_t got = f.read(reinterpret_cast<uint8_t*>(_buf), size);
    f.close();
    if (got != size) return false;
    _buf[got] = '\0';

    const char* body_ptr = nullptr;
    size_t body_size = 0;
    // Документ без фронтматтера тоже читаем: показать текст важнее, чем
    // отказать из-за отсутствия заголовка. Вызывающий видит, что meta.title
    // пуст.
    doc::parse(_buf, got, meta, &body_ptr, &body_size);

    if (body != nullptr && body_len != nullptr) {
        if (body_size + 1 > body_cap) {
            body_size = body_cap > 0 ? body_cap - 1 : 0;
        }
        if (body_size > 0) memcpy(body, body_ptr, body_size);
        body[body_size] = '\0';
        *body_len = body_size;
    }
    return true;
}

bool DocStore::read_meta(const char* rel, doc::Meta& meta, size_t* file_bytes)
{
    if (!_ready) return false;
    if (paths::check(rel) != paths::Result::Ok) return false;

    char path[paths::kMaxPathLen + 16];
    make_path(rel, path, sizeof(path));

    // Размер нужен списку, и он узнаётся в том же открытии: список читает
    // заголовки десятков файлов, и второе открытие на каждый удваивало бы
    // работу с SD.
    size_t len = 0;
    return read_file(path, meta, nullptr, 0, &len, file_bytes);
}

void DocStore::walk(const char* rel_folder, docapi::Entry* out, size_t cap, size_t depth)
{
    // Глубже kMaxDepth не ходим: paths::check всё равно не пустит такой путь,
    // а каталог с мусором мог бы оказаться сколь угодно глубоким.
    if (depth > paths::kMaxDepth) return;

    char dir_path[paths::kMaxPathLen + 16];
    if (rel_folder[0] == '\0') {
        snprintf(dir_path, sizeof(dir_path), "%s", cfg::kNotesDir);
    } else {
        make_path(rel_folder, dir_path, sizeof(dir_path));
    }

    File dir = SD_MMC.open(dir_path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
    }

    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        // Имя копируем ДО close(): File::close() отпускает реализацию, и name()
        // после него возвращает nullptr — на этом уже обжигались в ChatStore.
        char name[128];
        snprintf(name, sizeof(name), "%s", entry.name() != nullptr ? entry.name() : "");
        const bool is_dir = entry.isDirectory();
        const size_t size = is_dir ? 0 : entry.size();
        entry.close();
        if (name[0] == '\0') continue;

        // name() может вернуть полный путь или только имя.
        const char* slash = strrchr(name, '/');
        const char* base = slash != nullptr ? slash + 1 : name;
        if (base[0] == '\0') continue;

        char rel[paths::kMaxPathLen];
        if (rel_folder[0] == '\0') {
            snprintf(rel, sizeof(rel), "%s", base);
        } else {
            snprintf(rel, sizeof(rel), "%s/%s", rel_folder, base);
        }

        if (is_dir) {
            walk(rel, out, cap, depth + 1);
            continue;
        }

        const size_t len = strlen(base);
        if (len < 4 || strcmp(base + len - 3, ".md") != 0) continue;
        // Временный файл атомарной записи — не документ.
        if (len > 7 && strcmp(base + len - 7, ".md.tmp") == 0) continue;

        // Индекс — служебный файл, а не документ: иначе он попадал бы в
        // собственный список, в поиск и в «архив пуст» на пустом архиве.
        if (strcmp(rel, "INDEX.md") == 0) continue;

        // Путь из каталога может не пройти проверку (кириллица, слишком
        // глубокая вложенность) — такой файл показывать нельзя: его нельзя
        // ни открыть инструментом, ни перезаписать.
        if (paths::check(rel) != paths::Result::Ok) continue;

        _total_found += 1;
        if (_total_found > cap) continue;

        docapi::Entry& e = out[_total_found - 1];
        e = docapi::Entry{};
        snprintf(e.path, sizeof(e.path), "%s", rel);
        e.bytes = size;

        doc::Meta meta;
        size_t bytes = 0;
        if (read_meta(rel, meta, &bytes)) {
            snprintf(e.title, sizeof(e.title), "%s", meta.title);
            date_only(meta.updated[0] != '\0' ? meta.updated : meta.created, e.updated, sizeof(e.updated));
        }
    }
    dir.close();
}

size_t DocStore::list(const char* folder, docapi::Entry* out, size_t cap)
{
    _total_found = 0;
    if (!_ready || out == nullptr || cap == 0) return 0;
    if (cap > kMaxListedDocs) cap = kMaxListedDocs;

    walk(folder != nullptr ? folder : "", out, cap, 0);
    return _total_found < cap ? _total_found : cap;
}

bool DocStore::read(
    const char* rel, doc::Meta& meta, char* body, size_t body_cap, size_t* body_len, char* err, size_t err_len)
{
    if (err != nullptr && err_len > 0) err[0] = '\0';
    if (!_ready) {
        if (err && err_len > 0) snprintf(err, err_len, "архив недоступен: нет SD-карты");
        return false;
    }

    const paths::Result r = paths::check(rel);
    if (r != paths::Result::Ok) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", paths::error_text(r));
        return false;
    }

    char path[paths::kMaxPathLen + 16];
    make_path(rel, path, sizeof(path));

    if (!read_file(path, meta, body, body_cap, body_len, nullptr)) {
        if (err && err_len > 0) snprintf(err, err_len, "документ «%s» не найден или не читается", rel);
        return false;
    }
    return true;
}

bool DocStore::write(const char* rel, const doc::Meta& meta, const char* body, bool* created, char* err, size_t err_len)
{
    if (err != nullptr && err_len > 0) err[0] = '\0';
    if (created != nullptr) *created = false;
    if (!_ready) {
        if (err && err_len > 0) snprintf(err, err_len, "архив недоступен: нет SD-карты");
        return false;
    }

    const paths::Result r = paths::check(rel);
    if (r != paths::Result::Ok) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", paths::error_text(r));
        return false;
    }

    if (meta.title[0] == '\0') {
        if (err && err_len > 0) snprintf(err, err_len, "у документа нет заголовка (title)");
        return false;
    }

    // Папку создаём до записи: иначе fopen во вложенном каталоге вернёт ошибку.
    char folder[paths::kMaxPathLen];
    paths::folder_of(rel, folder, sizeof(folder));
    if (!ensure_folder(folder)) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось создать папку «%s»", folder);
        return false;
    }

    char path[paths::kMaxPathLen + 16];
    char tmp[paths::kMaxPathLen + 24];
    make_path(rel, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    // Дата создания неизменна. Если файл уже есть, берём её оттуда ВСЕГДА, а
    // не только когда в новом фронтматтере её нет: инструмент write_doc
    // подставляет туда текущее время, и проверка «created пуст» не срабатывала
    // бы никогда — перезапись «омолаживала» документ, и он терял место в
    // хронологии. Проверено на живой карте: created менялся при каждом
    // обновлении.
    doc::Meta out_meta = meta;
    const bool exists = SD_MMC.exists(path);
    if (created != nullptr) *created = !exists;
    if (exists) {
        doc::Meta old;
        size_t bytes = 0;
        if (read_meta(rel, old, &bytes) && old.created[0] != '\0') {
            snprintf(out_meta.created, sizeof(out_meta.created), "%s", old.created);
        }
    }

    // Собираем файл в буфер _buf: так запись — одна операция, и до неё видно,
    // влез ли документ в предел.
    const size_t n = doc::build(out_meta, body, _buf, kMaxDocBytes);
    if (n == 0) {
        if (err && err_len > 0) snprintf(err, err_len, "документ слишком большой или без заголовка");
        return false;
    }

    File f = SD_MMC.open(tmp, FILE_WRITE);
    if (!f) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось создать временный файл");
        return false;
    }
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(_buf), n);
    f.close();

    if (written != n) {
        SD_MMC.remove(tmp);
        if (err && err_len > 0) snprintf(err, err_len, "запись оборвалась: документ не сохранён");
        return false;
    }

    // Rename поверх существующего файла на FAT32 не работает. Удаляем старый
    // только ПОСЛЕ успешной записи .tmp: если что-то пойдёт не так, текст
    // останется в .tmp рядом, и его можно спасти руками.
    if (exists) SD_MMC.remove(path);
    if (!SD_MMC.rename(tmp, path)) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось заменить документ (текст остался в %s)", tmp);
        return false;
    }
    return true;
}

bool DocStore::remove(const char* rel, char* err, size_t err_len)
{
    if (err != nullptr && err_len > 0) err[0] = '\0';
    if (!_ready) {
        if (err && err_len > 0) snprintf(err, err_len, "архив недоступен: нет SD-карты");
        return false;
    }

    const paths::Result r = paths::check(rel);
    if (r != paths::Result::Ok) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", paths::error_text(r));
        return false;
    }

    char path[paths::kMaxPathLen + 16];
    make_path(rel, path, sizeof(path));

    if (!SD_MMC.exists(path)) {
        if (err && err_len > 0) snprintf(err, err_len, "документ «%s» не найден", rel);
        return false;
    }
    if (!SD_MMC.remove(path)) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось удалить документ «%s»", rel);
        return false;
    }
    return true;
}

size_t DocStore::search(const char* query, size_t limit, docapi::Hit* out, size_t cap)
{
    if (!_ready || query == nullptr || query[0] == '\0' || out == nullptr || cap == 0) return 0;
    if (limit == 0 || limit > cap) limit = cap;

    // Собираем все документы и ищем в каждом. Для архива на десятки файлов
    // этого достаточно, а индекс с обратным словником — отдельная работа,
    // которая оправдается на сотнях.
    if (!ensure_entries()) return 0;
    docapi::Entry* entries = _entries;
    const size_t found = list("", entries, kMaxListedDocs);
    if (found == 0) return 0;

    const size_t qlen = strlen(query);
    size_t hits = 0;

    for (size_t i = 0; i < found && hits < limit; ++i) {
        doc::Meta meta;
        size_t body_len = 0;
        if (!read(entries[i].path, meta, _body, kMaxDocBytes, &body_len, nullptr, 0)) continue;

        // Сначала заголовок: если слово в заголовке — это более релевантный
        // результат, чем совпадение в середине текста.
        const char* where = docsearch::find_ci(meta.title, strlen(meta.title), query, qlen);
        if (where != nullptr) {
            docapi::Hit& h = out[hits++];
            h = docapi::Hit{};
            snprintf(h.path, sizeof(h.path), "%s", entries[i].path);
            docsearch::snippet(meta.title, strlen(meta.title), static_cast<size_t>(where - meta.title), qlen, 40,
                h.snippet, sizeof(h.snippet));
            continue;
        }

        where = docsearch::find_ci(_body, body_len, query, qlen);
        if (where == nullptr) continue;

        docapi::Hit& h = out[hits++];
        h = docapi::Hit{};
        snprintf(h.path, sizeof(h.path), "%s", entries[i].path);
        docsearch::snippet(_body, body_len, static_cast<size_t>(where - _body), qlen, 48, h.snippet, sizeof(h.snippet));
    }
    return hits;
}

bool DocStore::exists(const char* rel)
{
    if (!_ready) return false;
    if (paths::check(rel) != paths::Result::Ok) return false;

    char path[paths::kMaxPathLen + 16];
    make_path(rel, path, sizeof(path));
    return SD_MMC.exists(path);
}

bool DocStore::rebuild_index(char* err, size_t err_len)
{
    if (err != nullptr && err_len > 0) err[0] = '\0';
    if (!_ready) {
        if (err && err_len > 0) snprintf(err, err_len, "архив недоступен: нет SD-карты");
        return false;
    }

    if (!ensure_entries()) return false;
    docapi::Entry* entries = _entries;
    const size_t found = list("", entries, kMaxListedDocs);

    // Собираем индекс в _buf: документ маленький (строка на файл), а запись
    // одной операцией не оставляет на карте половину индекса.
    size_t n = 0;
    auto put = [&](const char* s) -> bool {
        const size_t len = strlen(s);
        if (n + len + 1 > kMaxDocBytes) return false;
        memcpy(_buf + n, s, len);
        n += len;
        _buf[n] = '\0';
        return true;
    };

    if (!put("# Архив документов\n\n")) return false;
    if (found == 0) {
        if (!put("Пока пусто.\n")) return false;
    } else {
        if (!put("| Документ | Заголовок | Обновлён |\n|---|---|---|\n")) return false;
        for (size_t i = 0; i < found; ++i) {
            const char* title = entries[i].title[0] != '\0' ? entries[i].title : entries[i].path;
            const char* date = entries[i].updated[0] != '\0' ? entries[i].updated : "—";
            char row[512];
            snprintf(row, sizeof(row), "| [%s](%s) | %s | %s |\n", entries[i].path, entries[i].path, title, date);
            if (!put(row)) return false;
        }
    }

    char indexPath[64];
    snprintf(indexPath, sizeof(indexPath), "%s/INDEX.md", cfg::kNotesDir);
    File f = SD_MMC.open(indexPath, FILE_WRITE);
    if (!f) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось записать INDEX.md");
        return false;
    }
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(_buf), n);
    f.close();
    if (written != n) {
        if (err && err_len > 0) snprintf(err, err_len, "запись INDEX.md оборвалась");
        return false;
    }
    return true;
}

}  // namespace hw
