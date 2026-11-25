// Инструменты архива: схемы и обработчики. Без Arduino — тесты на хосте.
//
// Каждый обработчик решает три вопроса: что взять из аргументов, что вернуть
// модели, каким текстом объяснить отказ. Текст ошибки — часть контракта:
// живой прогон (Р-025) показал, что модель исправляется по подсказке с
// примером правильного значения, поэтому пример есть в каждом отказе.

#include "util/doc_tools.h"

#include <stdio.h>
#include <string.h>

#include "util/doc_api.h"
#include "util/doc_search.h"

namespace doctools {
namespace {

using docapi::Store;

/// Хранилище из контекста. nullptr — вызывающий не привязал DocStore.
Store* store_of(tools::Context& ctx)
{
    return static_cast<Store*>(ctx.store);
}

Store* require_store(tools::Context& ctx)
{
    Store* s = store_of(ctx);
    if (s == nullptr) tools::set_error(ctx, "архив недоступен");
    return s;
}

/// Прочитать обязательное строковое поле с понятной ошибкой.
///
/// Подсказка с примером — не украшение: по ней модель исправляет аргументы.
bool need_string(tools::Context& ctx, const char* args, size_t args_len, const char* key, const char* example,
    char* out, size_t out_len)
{
    if (tools::arg_string(args, args_len, key, out, out_len)) return true;

    char msg[256];
    snprintf(
        msg, sizeof(msg), "в аргументах нет поля «%s» или оно не строка. Пример: {\"%s\": \"%s\"}", key, key, example);
    tools::set_error(ctx, msg);
    return false;
}

// --- list_docs --------------------------------------------------------------

bool list_docs(const char* args, size_t args_len, tools::Context& ctx)
{
    Store* store = require_store(ctx);
    if (store == nullptr) return false;

    // folder необязателен: без него — весь архив.
    char folder[paths::kMaxPathLen];
    folder[0] = '\0';
    tools::arg_string(args, args_len, "folder", folder, sizeof(folder));

    if (folder[0] != '\0') {
        // Папка — это префикс пути, а не документ: проверяем её отдельно,
        // чтобы «home/../config» не превратился в обход корня.
        char probe[paths::kMaxPathLen];
        const int n = snprintf(probe, sizeof(probe), "%s/x.md", folder);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(probe)) {
            tools::set_error(ctx, "слишком длинное имя папки");
            return false;
        }
        const paths::Result r = paths::check(probe);
        if (r != paths::Result::Ok) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s", paths::error_text(r));
            tools::set_error(ctx, msg);
            return false;
        }
    }

    if (ctx.entries == nullptr || ctx.entries_cap == 0) {
        tools::set_error(ctx, "внутренняя ошибка: некуда положить список");
        return false;
    }

    const size_t found = store->list(folder, ctx.entries, ctx.entries_cap);
    if (found == 0) {
        // Пустой архив — НЕ ошибка: модель должна отличать «документов нет» от
        // «не смог прочитать». Возвращаем true, чтобы цикл агента не считал
        // это неудачным вызовом.
        if (folder[0] != '\0') {
            tools::set_resultf(ctx, "В папке «%s» документов нет.", folder);
        } else {
            tools::set_result(ctx, "Архив пуст. Документов пока нет.");
        }
        return true;
    }

    size_t used = tools::set_resultf(ctx, "Документы (%u):\n", static_cast<unsigned>(found));
    for (size_t i = 0; i < found; ++i) {
        const docapi::Entry& e = ctx.entries[i];
        char row[256];
        const size_t n = docsearch::format_list_row(e.path, e.title, e.updated, row, sizeof(row));
        if (n == 0) continue;
        const size_t need = strlen(row) + 2;
        if (used + need + 1 > ctx.out_cap) break;
        ctx.out[used++] = '\n';
        memcpy(ctx.out + used, row, strlen(row));
        used += strlen(row);
        ctx.out[used] = '\0';
    }
    return true;
}

// --- read_doc ---------------------------------------------------------------

bool read_doc(const char* args, size_t args_len, tools::Context& ctx)
{
    Store* store = require_store(ctx);
    if (store == nullptr) return false;

    char rel[paths::kMaxPathLen];
    if (!need_string(ctx, args, args_len, "path", "home/air-purifier-filter.md", rel, sizeof(rel))) return false;

    // Путь проверяем здесь, а не только в хранилище: инструмент обязан
    // отказывать одинаково при любой реализации Store, и подсказка с примером
    // должна быть в его собственном ответе.
    const paths::Result pr = paths::check(rel);
    if (pr != paths::Result::Ok) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s. Получено: «%s»", paths::error_text(pr), rel);
        tools::set_error(ctx, msg);
        return false;
    }

    doc::Meta meta;
    size_t body_len = 0;
    char err[160];
    if (!store->read(rel, meta, ctx.body, ctx.body_cap, &body_len, err, sizeof(err))) {
        // Ошибку хранилища отдаём как есть: в ней уже есть путь и причина.
        tools::set_error(ctx, err[0] != '\0' ? err : "документ не читается");
        return false;
    }

    // Заголовок — первой строкой: модель должна видеть, что это за документ.
    // Тело уже усечено хранилищем до своего предела, но общий предел ответа
    // (4 КБ) меньше, поэтому режем здесь.
    const char* title = meta.title[0] != '\0' ? meta.title : rel;
    size_t used = tools::set_resultf(ctx, "# %s\n\n", title);
    const size_t room = ctx.out_cap > used + 16 ? ctx.out_cap - used - 1 : 0;
    if (room > 0 && body_len > 0) {
        const size_t take = docsearch::truncate_utf8(ctx.body, body_len, room - 1, ctx.out + used, room);
        used += take;
    }
    (void)used;
    return true;
}

// --- search_docs ------------------------------------------------------------

bool search_docs(const char* args, size_t args_len, tools::Context& ctx)
{
    Store* store = require_store(ctx);
    if (store == nullptr) return false;

    char query[128];
    if (!need_string(ctx, args, args_len, "query", "фильтр", query, sizeof(query))) return false;

    int limit = tools::arg_int(args, args_len, "limit", 5);
    if (limit <= 0) limit = 5;
    if (limit > 10) limit = 10;

    if (ctx.hits == nullptr || ctx.hits_cap == 0) {
        tools::set_error(ctx, "внутренняя ошибка: некуда положить результаты");
        return false;
    }

    const size_t found = store->search(query, static_cast<size_t>(limit), ctx.hits, ctx.hits_cap);
    if (found == 0) {
        tools::set_error(ctx, "по запросу ничего не найдено. Попробуйте одно слово в другой форме или list_docs");
        return false;
    }

    size_t used = tools::set_resultf(ctx, "Найдено (%u):\n", static_cast<unsigned>(found));
    for (size_t i = 0; i < found; ++i) {
        const docapi::Hit& h = ctx.hits[i];
        char row[320];
        const int n = snprintf(row, sizeof(row), "\n%s\n  %s", h.path, h.snippet);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(row)) continue;
        if (used + static_cast<size_t>(n) + 1 > ctx.out_cap) break;
        memcpy(ctx.out + used, row, static_cast<size_t>(n));
        used += static_cast<size_t>(n);
        ctx.out[used] = '\0';
    }
    return true;
}

// --- write_doc --------------------------------------------------------------

bool write_doc(const char* args, size_t args_len, tools::Context& ctx)
{
    Store* store = require_store(ctx);
    if (store == nullptr) return false;

    char raw_path[128];
    char title[128];
    if (!need_string(ctx, args, args_len, "path", "home/air-purifier-filter.md", raw_path, sizeof(raw_path))) {
        return false;
    }
    if (!need_string(ctx, args, args_len, "title", "Сброс ошибки очистителя", title, sizeof(title))) return false;

    // Путь нормализуем: модель может написать «./home/x», с большой буквы или
    // без расширения. Нормализация безопасна — она не выводит за пределы
    // архива, а проверка внутри.
    char rel[paths::kMaxPathLen];
    const paths::Result r = paths::normalize(raw_path, rel, sizeof(rel));
    if (r != paths::Result::Ok) {
        // Главный отказ: в нём должен быть пример, иначе модель повторит тот
        // же путь. Валидация пути уже пишет пример — используем его.
        char msg[320];
        snprintf(msg, sizeof(msg), "%s. Получено: «%s»", paths::error_text(r), raw_path);
        tools::set_error(ctx, msg);
        return false;
    }

    doc::Meta meta;
    snprintf(meta.title, sizeof(meta.title), "%s", title);
    const bool has_url = tools::arg_string(args, args_len, "source_url", meta.source_url, sizeof(meta.source_url));
    const bool has_tags = tools::arg_string(args, args_len, "tags", meta.tags, sizeof(meta.tags));

    // Обновление существующего документа: недостающие поля берём из старого
    // файла. Модель, дописывая абзац, обычно присылает только path, title и
    // content — и теги с источником исчезали бы из фронтматтера навсегда.
    // Проверено на живой карте: после второй записи в файле остались только
    // title и даты.
    //
    // Стереть поле намеренно всё равно можно: пустая строка в аргументе — это
    // «очистить», и она отличается от отсутствия поля.
    // read_meta на несуществующем документе просто вернёт false — отдельная
    // проверка exists() означала бы второе открытие файла ради того же ответа.
    if (!has_url || !has_tags) {
        doc::Meta old;
        size_t old_bytes = 0;
        if (store->read_meta(rel, old, &old_bytes)) {
            if (!has_url) snprintf(meta.source_url, sizeof(meta.source_url), "%s", old.source_url);
            if (!has_tags) snprintf(meta.tags, sizeof(meta.tags), "%s", old.tags);
        }
    }

    // Время ставим сами: модель не знает часов устройства, а выдуманная дата
    // хуже отсутствующей. created перезапишется в DocStore::write из старого
    // файла — здесь он нужен только для нового документа.
    if (ctx.now_iso != nullptr && ctx.now_iso[0] != '\0') {
        snprintf(meta.updated, sizeof(meta.updated), "%s", ctx.now_iso);
        snprintf(meta.created, sizeof(meta.created), "%s", ctx.now_iso);
    }

    // Тело — прямо в буфер вызова: документ пишется целиком, а 64 КБ в
    // контексте уже выделены. Отдельная копия была бы лишними 64 КБ.
    if (ctx.body == nullptr || ctx.body_cap == 0) {
        tools::set_error(ctx, "внутренняя ошибка: нет буфера под текст документа");
        return false;
    }
    if (!tools::arg_string(args, args_len, "content", ctx.body, ctx.body_cap)) {
        tools::set_error(
            ctx, "в аргументах нет поля «content» или оно не строка. Пример: {\"content\": \"# Заметка\\n\\nТекст\"}");
        return false;
    }

    bool created = false;
    char err[200];
    if (!store->write(rel, meta, ctx.body, &created, err, sizeof(err))) {
        tools::set_error(ctx, err[0] != '\0' ? err : "не удалось сохранить документ");
        return false;
    }

    tools::set_resultf(ctx, created ? "Создан документ %s" : "Обновлён документ %s", rel);
    return true;
}

// --- delete_doc -------------------------------------------------------------

bool delete_doc(const char* args, size_t args_len, tools::Context& ctx)
{
    Store* store = require_store(ctx);
    if (store == nullptr) return false;

    char rel[paths::kMaxPathLen];
    if (!need_string(ctx, args, args_len, "path", "home/air-purifier-filter.md", rel, sizeof(rel))) return false;

    if (paths::check(rel) != paths::Result::Ok) {
        // Удалять по невалидному пути нельзя: normalize здесь не годится —
        // удаление не должно «догадываться», что имелось в виду.
        char msg[320];
        snprintf(msg, sizeof(msg), "%s. Получено: «%s»", paths::error_text(paths::check(rel)), rel);
        tools::set_error(ctx, msg);
        return false;
    }

    // Удаление — необратимое действие, поэтому сначала спрашиваем
    // пользователя. Обработчик не показывает диалог: это работа экрана.
    // Решение приходит следующим вызовом с уже подтверждённым флагом.
    if (!ctx.confirmed) {
        if (store->exists(rel)) {
            ctx.needs_confirm = true;
            ctx.deferred = true;
            snprintf(ctx.confirm_path, sizeof(ctx.confirm_path), "%s", rel);
            tools::set_resultf(ctx, "Нужно подтверждение: удалить %s?", rel);
            return false;
        }
        tools::set_error(ctx, "документ не найден, удалять нечего");
        return false;
    }

    char err[200];
    if (!store->remove(rel, err, sizeof(err))) {
        tools::set_error(ctx, err[0] != '\0' ? err : "не удалось удалить документ");
        return false;
    }

    tools::set_resultf(ctx, "Удалён документ %s", rel);
    return true;
}

}  // namespace

namespace schema {

// Схемы — компактные строки. В описании пути прямо сказано, какие символы
// разрешены: модель это соблюдает (проверено), и грубый фильтр имён не нужен.
const char* kListDocs =
    "{\"name\": \"list_docs\", \"description\": "
    "\"Показать документы архива. Без folder — все, с folder — только из этой папки.\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {"
    "\"folder\": {\"type\": \"string\", \"description\": "
    "\"Папка латиницей, например home или home/kitchen. Пусто — весь архив.\"}}, "
    "\"required\": []}}";

const char* kReadDoc =
    "{\"name\": \"read_doc\", \"description\": \"Прочитать документ архива целиком.\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {"
    "\"path\": {\"type\": \"string\", \"description\": "
    "\"Путь латиницей, цифрами, дефисом и слэшем, например home/air-purifier-filter.md\"}}, "
    "\"required\": [\"path\"]}}";

const char* kSearchDocs =
    "{\"name\": \"search_docs\", \"description\": \"Найти документы по слову или фразе, без учёта регистра.\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {"
    "\"query\": {\"type\": \"string\", \"description\": \"Что искать, например фильтр\"}, "
    "\"limit\": {\"type\": \"integer\", \"description\": \"Сколько результатов вернуть, по умолчанию 5\"}}, "
    "\"required\": [\"query\"]}}";

const char* kWriteDoc =
    "{\"name\": \"write_doc\", \"description\": "
    "\"Сохранить документ в архив. Если файл есть — обновить его.\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {"
    "\"path\": {\"type\": \"string\", \"description\": "
    "\"ASCII-слаг: латиница, цифры, дефис, подчёркивание, слэш; расширение .md, например "
    "home/air-purifier-filter.md\"}, "
    "\"title\": {\"type\": \"string\", \"description\": \"Заголовок по-русски для списка\"}, "
    "\"content\": {\"type\": \"string\", \"description\": \"Текст документа в markdown\"}, "
    "\"source_url\": {\"type\": \"string\", \"description\": \"Ссылка на источник, если есть\"}, "
    "\"tags\": {\"type\": \"string\", \"description\": \"Теги через запятую, например бытовая-техника, очиститель\"}}, "
    "\"required\": [\"path\", \"title\", \"content\"]}}";

const char* kDeleteDoc =
    "{\"name\": \"delete_doc\", \"description\": "
    "\"Удалить документ. Пользователь подтвердит удаление на экране устройства.\", "
    "\"parameters\": {\"type\": \"object\", \"properties\": {"
    "\"path\": {\"type\": \"string\", \"description\": "
    "\"Путь латиницей, например home/air-purifier-filter.md\"}}, "
    "\"required\": [\"path\"]}}";

}  // namespace schema

bool register_all(tools::Registry& reg)
{
    return reg.add("list_docs", schema::kListDocs, list_docs) && reg.add("read_doc", schema::kReadDoc, read_doc) &&
           reg.add("search_docs", schema::kSearchDocs, search_docs) &&
           reg.add("write_doc", schema::kWriteDoc, write_doc) && reg.add("delete_doc", schema::kDeleteDoc, delete_doc);
}

}  // namespace doctools
