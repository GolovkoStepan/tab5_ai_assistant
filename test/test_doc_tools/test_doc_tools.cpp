/*
 * Тесты инструментов архива — на подставном хранилище.
 *
 * Настоящий DocStore требует SD-карту, поэтому здесь FakeStore: он живёт в
 * памяти и умеет то же, что настоящий, но без файлов. Так проверяется именно
 * логика инструментов: что вернуть модели, какую ошибку написать, что делает
 * write_doc с кириллическим путём.
 *
 * Почему это важно: ошибки здесь видит только модель. Если инструмент вернёт
 * невнятное «не удалось», модель начнёт переспрашивать или выдумывать — а
 * пользователь увидит бессмысленный диалог.
 */

#include <string.h>

#include <ArduinoJson.h>
#include <unity.h>

#include "util/doc_tools.h"
#include "util/tool_registry.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

/// Хранилище в памяти: до 8 документов, 8 КБ на тело.
class FakeStore : public docapi::Store {
public:
    struct Doc {
        char path[paths::kMaxPathLen]{};
        doc::Meta meta;
        char body[2048]{};
        size_t body_len{0};
        bool used{false};
    };

    Doc docs[8]{};
    size_t count{0};
    bool fail_read{false};
    bool fail_write{false};
    bool fail_list{false};

    Doc* find(const char* rel)
    {
        for (size_t i = 0; i < count; ++i) {
            if (docs[i].used && strcmp(docs[i].path, rel) == 0) return &docs[i];
        }
        return nullptr;
    }

    bool add(const char* rel, const char* title, const char* body)
    {
        if (count >= 8) return false;
        Doc& d = docs[count++];
        d = Doc{};
        d.used = true;
        snprintf(d.path, sizeof(d.path), "%s", rel);
        snprintf(d.meta.title, sizeof(d.meta.title), "%s", title);
        snprintf(d.meta.updated, sizeof(d.meta.updated), "2026-11-23");
        snprintf(d.body, sizeof(d.body), "%s", body);
        d.body_len = strlen(d.body);
        return true;
    }

    size_t list(const char* folder, docapi::Entry* out, size_t cap) override
    {
        if (fail_list) return 0;
        size_t n = 0;
        const size_t flen = folder != nullptr ? strlen(folder) : 0;
        for (size_t i = 0; i < count && n < cap; ++i) {
            if (!docs[i].used) continue;
            // Папка — префикс пути: «home» включает «home/x.md», но не «home2/x.md».
            if (flen > 0 && strncmp(docs[i].path, folder, flen) != 0) continue;
            out[n] = docapi::Entry{};
            snprintf(out[n].path, sizeof(out[n].path), "%s", docs[i].path);
            snprintf(out[n].title, sizeof(out[n].title), "%s", docs[i].meta.title);
            memcpy(out[n].updated, docs[i].meta.updated, 11);
            ++n;
        }
        return n;
    }

    bool read(const char* rel, doc::Meta& meta, char* body, size_t body_cap, size_t* body_len, char* err,
        size_t err_len) override
    {
        if (fail_read) {
            if (err && err_len > 0) snprintf(err, err_len, "документ «%s» не найден или не читается", rel);
            return false;
        }
        Doc* d = find(rel);
        if (d == nullptr) {
            if (err && err_len > 0) snprintf(err, err_len, "документ «%s» не найден или не читается", rel);
            return false;
        }
        meta = d->meta;
        if (body != nullptr && body_len != nullptr && body_cap > 0) {
            size_t n = d->body_len < body_cap - 1 ? d->body_len : body_cap - 1;
            memcpy(body, d->body, n);
            body[n] = '\0';
            *body_len = n;
        }
        return true;
    }

    bool read_meta(const char* rel, doc::Meta& meta, size_t* file_bytes) override
    {
        Doc* d = find(rel);
        if (d == nullptr) return false;
        meta = d->meta;
        if (file_bytes != nullptr) *file_bytes = d->body_len;
        return true;
    }

    bool write(
        const char* rel, const doc::Meta& meta, const char* body, bool* created, char* err, size_t err_len) override
    {
        if (fail_write) {
            if (err && err_len > 0) snprintf(err, err_len, "не удалось создать временный файл");
            return false;
        }
        Doc* d = find(rel);
        if (created != nullptr) *created = d == nullptr;
        if (d == nullptr) {
            if (count >= 8) {
                if (err && err_len > 0) snprintf(err, err_len, "архив переполнен");
                return false;
            }
            d = &docs[count++];
            *d = Doc{};
            d->used = true;
            snprintf(d->path, sizeof(d->path), "%s", rel);
        }
        d->meta = meta;
        snprintf(d->body, sizeof(d->body), "%s", body != nullptr ? body : "");
        d->body_len = strlen(d->body);
        return true;
    }

    bool remove(const char* rel, char* err, size_t err_len) override
    {
        Doc* d = find(rel);
        if (d == nullptr) {
            if (err && err_len > 0) snprintf(err, err_len, "документ «%s» не найден", rel);
            return false;
        }
        d->used = false;
        return true;
    }

    size_t search(const char* query, size_t limit, docapi::Hit* out, size_t cap) override
    {
        size_t n = 0;
        const size_t qlen = strlen(query);
        for (size_t i = 0; i < count && n < limit && n < cap; ++i) {
            if (!docs[i].used) continue;
            const char* where = docsearch_placeholder(docs[i].body, docs[i].body_len, query, qlen);
            if (where == nullptr) continue;
            out[n] = docapi::Hit{};
            snprintf(out[n].path, sizeof(out[n].path), "%s", docs[i].path);
            snprintf(out[n].snippet, sizeof(out[n].snippet), "%s", query);
            ++n;
        }
        return n;
    }

    bool exists(const char* rel) override { return find(rel) != nullptr; }

private:
    /// Простейший поиск: для тестов инструментов хватает латиницы и точного
    /// совпадения — сам поиск проверяется в test_doc_search.
    static const char* docsearch_placeholder(const char* hay, size_t len, const char* needle, size_t needle_len)
    {
        if (needle_len == 0 || len < needle_len) return nullptr;
        for (size_t i = 0; i + needle_len <= len; ++i) {
            if (memcmp(hay + i, needle, needle_len) == 0) return hay + i;
        }
        return nullptr;
    }
};

/// Контекст со всеми буферами: обработчики не выделяют память сами.
struct Fixture {
    tools::Registry reg;
    FakeStore store;
    char out[tools::kMaxResultBytes + 1]{};
    char body[4096]{};
    docapi::Entry entries[docapi::kMaxListedDocs]{};
    docapi::Hit hits[16]{};
    tools::Context ctx;

    Fixture()
    {
        doctools::register_all(reg);
        ctx.out = out;
        ctx.out_cap = sizeof(out);
        ctx.store = &store;
        ctx.body = body;
        ctx.body_cap = sizeof(body);
        ctx.entries = entries;
        ctx.entries_cap = docapi::kMaxListedDocs;
        ctx.hits = hits;
        ctx.hits_cap = 16;
        ctx.now_iso = "2026-11-25T10:00:00+03:00";
    }

    bool call(const char* name, const char* args) { return reg.invoke(name, args, strlen(args), ctx); }
};

}  // namespace

// --- Реестр ----------------------------------------------------------------

void test_registry_has_all_tools()
{
    Fixture f;
    TEST_ASSERT_EQUAL_UINT32(5, f.reg.count());
    for (const char* name : {"list_docs", "read_doc", "search_docs", "write_doc", "delete_doc"}) {
        TEST_ASSERT_NOT_NULL(f.reg.find(name));
    }
}

void test_registry_rejects_duplicate_name()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.reg.add("read_doc", "{}", [](const char*, size_t, tools::Context&) { return true; }));
}

void test_registry_builds_valid_json()
{
    Fixture f;
    char json[8192];
    TEST_ASSERT_TRUE(f.reg.build_tools_json(json, sizeof(json)));

    // Схемы идут в запрос как есть, поэтому набор обязан быть валидным JSON.
    // Проверяем через ArduinoJson — он же разберёт это на устройстве.
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json));
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT32(5, arr.size());
    for (JsonObjectConst item : arr) {
        TEST_ASSERT_EQUAL_STRING("function", item["type"] | "");
        TEST_ASSERT_TRUE((item["function"]["name"] | "")[0] != '\0');
    }
}

void test_registry_reports_unknown_tool()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("no_such_tool", "{}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "инструмента «no_such_tool» нет"));
    // Список доступных — чтобы модель могла исправиться, а не гадать.
    TEST_ASSERT_NOT_NULL(strstr(f.out, "read_doc"));
}

void test_registry_rejects_broken_json()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("read_doc", "{\"path\": "));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "не разобрались как JSON"));
}

void test_registry_rejects_non_object()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("read_doc", "[1,2,3]"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "должны быть JSON-объектом"));
}

// --- list_docs -------------------------------------------------------------

void test_list_docs_returns_rows()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс ошибки", "текст");
    f.store.add("home/filter.md", "Замена фильтра", "текст");

    TEST_ASSERT_TRUE(f.call("list_docs", "{}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Документы (2)"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/air.md — Сброс ошибки (2026-11-23)"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/filter.md — Замена фильтра"));
}

void test_list_docs_filters_by_folder()
{
    Fixture f;
    f.store.add("home/kitchen/kettle.md", "Чайник", "текст");
    f.store.add("car/tyres.md", "Шины", "текст");

    TEST_ASSERT_TRUE(f.call("list_docs", "{\"folder\": \"home\"}"));
    TEST_ASSERT_NULL(strstr(f.out, "car/tyres.md"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/kitchen/kettle.md"));
}

void test_list_docs_empty_archive_is_not_error_text()
{
    Fixture f;
    // Пустой архив — полезный результат, а не сбой: цикл агента не должен
    // считать вызов неудачным.
    TEST_ASSERT_TRUE(f.call("list_docs", "{}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Архив пуст"));
    TEST_ASSERT_NULL(strstr(f.out, "Ошибка"));
}

void test_list_docs_rejects_traversal_folder()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс", "текст");

    // Папка — тоже путь от модели: «..» здесь так же опасен, как в файле.
    TEST_ASSERT_FALSE(f.call("list_docs", "{\"folder\": \"home/../../..\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Ошибка"));
}

// --- read_doc --------------------------------------------------------------

void test_read_doc_returns_title_and_body()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс ошибки", "1. Выключить.\n2. Зажать кнопку.");

    TEST_ASSERT_TRUE(f.call("read_doc", "{\"path\": \"home/air.md\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "# Сброс ошибки"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Зажать кнопку"));
}

void test_read_doc_missing_has_actionable_error()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("read_doc", "{\"path\": \"home/none.md\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Ошибка:"));
    // Модели надо сказать, что делать: без подсказки она повторяет тот же вызов.
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/none.md"));
}

void test_read_doc_requires_path()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("read_doc", "{}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "«path»"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/air-purifier-filter.md"));
}

void test_read_doc_rejects_bad_path()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс", "текст");
    // Кириллица в пути: модель это предлагает, а FAT32 на ней спотыкается.
    TEST_ASSERT_FALSE(f.call("read_doc", "{\"path\": \"Архив/Заметка.md\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "недопустимый символ"));
}

// --- search_docs -----------------------------------------------------------

void test_search_docs_finds_and_formats()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс ошибки", "Замените фильтр раз в полгода");
    f.store.add("car/tyres.md", "Шины", "Давление 2.2 бара");

    TEST_ASSERT_TRUE(f.call("search_docs", "{\"query\": \"фильтр\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Найдено (1)"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/air.md"));
}

void test_search_docs_empty_query_has_hint()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("search_docs", "{}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "«query»"));
}

void test_search_docs_nothing_found_suggests_alternative()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс", "текст");
    TEST_ASSERT_FALSE(f.call("search_docs", "{\"query\": \"вертолёт\"}"));
    // Не найдено — не повод сдаваться: подсказываем другой запрос или список.
    TEST_ASSERT_NOT_NULL(strstr(f.out, "list_docs"));
}

// --- write_doc -------------------------------------------------------------

void test_write_doc_creates()
{
    Fixture f;
    TEST_ASSERT_TRUE(f.call("write_doc",
        "{\"path\": \"home/air-filter.md\", \"title\": \"Замена фильтра\", "
        "\"content\": \"# Замена\\n\\nРаз в полгода.\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Создан документ home/air-filter.md"));

    const FakeStore::Doc* d = f.store.find("home/air-filter.md");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("Замена фильтра", d->meta.title);
    TEST_ASSERT_NOT_NULL(strstr(d->body, "Раз в полгода"));
    // Время ставим сами: модель часов устройства не знает.
    TEST_ASSERT_EQUAL_STRING("2026-11-25T10:00:00+03:00", d->meta.updated);
}

void test_write_doc_updates_existing()
{
    Fixture f;
    f.store.add("home/air.md", "Старый заголовок", "старый текст");

    TEST_ASSERT_TRUE(f.call(
        "write_doc", "{\"path\": \"home/air.md\", \"title\": \"Новый заголовок\", \"content\": \"новый текст\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Обновлён документ home/air.md"));
    TEST_ASSERT_EQUAL_STRING("Новый заголовок", f.store.find("home/air.md")->meta.title);
}

void test_write_doc_normalizes_path()
{
    Fixture f;
    // Модель иногда пишет «./home/X.MD» — это не повод отказывать.
    TEST_ASSERT_TRUE(
        f.call("write_doc", "{\"path\": \"./Home/Air-Filter.MD\", \"title\": \"Замена\", \"content\": \"текст\"}"));
    TEST_ASSERT_NOT_NULL(f.store.find("home/air-filter.md"));
    TEST_ASSERT_NULL(f.store.find("./Home/Air-Filter.MD"));
}

void test_write_doc_rejects_cyrillic_path_with_example()
{
    Fixture f;
    // Проверено живым прогоном (Р-025): модель предлагает кириллический путь,
    // если не подсказать. В ошибке обязан быть пример — по нему она
    // исправляется.
    TEST_ASSERT_FALSE(
        f.call("write_doc", "{\"path\": \"Архив/Замена фильтра.md\", \"title\": \"Замена\", \"content\": \"текст\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Ошибка:"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "home/"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Архив/Замена фильтра.md"));
}

void test_write_doc_rejects_traversal()
{
    Fixture f;
    TEST_ASSERT_FALSE(
        f.call("write_doc", "{\"path\": \"../config.md\", \"title\": \"Взлом\", \"content\": \"текст\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "«..»"));
    TEST_ASSERT_EQUAL_UINT32(0, f.store.count);
}

void test_write_doc_requires_title()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("write_doc", "{\"path\": \"home/x.md\", \"content\": \"текст\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "«title»"));
}

void test_write_doc_requires_content()
{
    Fixture f;
    TEST_ASSERT_FALSE(f.call("write_doc", "{\"path\": \"home/x.md\", \"title\": \"Заголовок\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "«content»"));
}

void test_write_doc_update_keeps_omitted_fields()
{
    // Модель, дописывая абзац, обычно присылает только path, title и content.
    // Если бы инструмент брал поля «как есть», теги и источник исчезали бы из
    // фронтматтера навсегда — проверено на живой карте.
    Fixture f;
    TEST_ASSERT_TRUE(f.call("write_doc",
        "{\"path\": \"home/x.md\", \"title\": \"Заголовок\", \"content\": \"первый\", "
        "\"source_url\": \"https://example.com/a\", \"tags\": \"дом, техника\"}"));

    // Обновление без необязательных полей.
    TEST_ASSERT_TRUE(
        f.call("write_doc", "{\"path\": \"home/x.md\", \"title\": \"Заголовок\", \"content\": \"второй\"}"));
    const FakeStore::Doc* d = f.store.find("home/x.md");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("https://example.com/a", d->meta.source_url);
    TEST_ASSERT_EQUAL_STRING("дом, техника", d->meta.tags);

    // Явно пустое поле — это «стереть», и оно должно отличаться от отсутствия.
    TEST_ASSERT_TRUE(f.call(
        "write_doc", "{\"path\": \"home/x.md\", \"title\": \"Заголовок\", \"content\": \"третий\", \"tags\": \"\"}"));
    d = f.store.find("home/x.md");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("", d->meta.tags);
}

void test_write_doc_passes_optional_fields()
{
    Fixture f;
    TEST_ASSERT_TRUE(f.call("write_doc",
        "{\"path\": \"home/x.md\", \"title\": \"Заголовок\", \"content\": \"текст\", "
        "\"source_url\": \"https://example.com/a\", \"tags\": \"дом, техника\"}"));
    const FakeStore::Doc* d = f.store.find("home/x.md");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("https://example.com/a", d->meta.source_url);
    TEST_ASSERT_EQUAL_STRING("дом, техника", d->meta.tags);
}

void test_write_doc_reports_storage_error()
{
    Fixture f;
    f.store.fail_write = true;
    TEST_ASSERT_FALSE(f.call("write_doc", "{\"path\": \"home/x.md\", \"title\": \"T\", \"content\": \"c\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "временный файл"));
}

// --- delete_doc ------------------------------------------------------------

void test_delete_doc_asks_confirmation_first()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс", "текст");

    // Удаление необратимо, поэтому сначала подтверждение. Обработчик не
    // показывает диалог: это работа экрана.
    TEST_ASSERT_FALSE(f.call("delete_doc", "{\"path\": \"home/air.md\"}"));
    TEST_ASSERT_TRUE(f.ctx.needs_confirm);
    TEST_ASSERT_TRUE(f.ctx.deferred);
    TEST_ASSERT_EQUAL_STRING("home/air.md", f.ctx.confirm_path);
    TEST_ASSERT_NOT_NULL(f.store.find("home/air.md"));
}

void test_delete_doc_removes_after_confirmation()
{
    Fixture f;
    f.store.add("home/air.md", "Сброс", "текст");
    f.ctx.confirmed = true;

    TEST_ASSERT_TRUE(f.call("delete_doc", "{\"path\": \"home/air.md\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Удалён документ home/air.md"));
    TEST_ASSERT_NULL(f.store.find("home/air.md"));
    TEST_ASSERT_FALSE(f.ctx.needs_confirm);
}

void test_delete_doc_missing_file()
{
    Fixture f;
    f.ctx.confirmed = true;
    TEST_ASSERT_FALSE(f.call("delete_doc", "{\"path\": \"home/none.md\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "не найден"));
}

void test_delete_doc_rejects_traversal()
{
    Fixture f;
    f.ctx.confirmed = true;
    TEST_ASSERT_FALSE(f.call("delete_doc", "{\"path\": \"../config.md\"}"));
    TEST_ASSERT_NOT_NULL(strstr(f.out, "Ошибка:"));
}

// --- Схемы -----------------------------------------------------------------

void test_schemas_are_valid_json_with_required()
{
    Fixture f;
    for (size_t i = 0; i < f.reg.count(); ++i) {
        const tools::Tool& t = f.reg.at(i);
        TEST_ASSERT_NOT_NULL(t.name);
        TEST_ASSERT_NOT_NULL(t.schema);

        // Схема — валидный JSON-объект с именем и parameters.
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, t.schema);
        TEST_ASSERT_EQUAL_STRING("Ok", err.c_str());
        TEST_ASSERT_TRUE(doc["parameters"].is<JsonObjectConst>());
        TEST_ASSERT_TRUE(doc["parameters"]["type"] == "object");
        // Имя в схеме обязано совпадать с именем в реестре: иначе модель
        // вызовет «одно», а обработчик ищется по другому.
        TEST_ASSERT_EQUAL_STRING(t.name, doc["name"] | "");
        TEST_ASSERT_TRUE(doc["description"].is<const char*>());
    }
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_registry_has_all_tools);
    RUN_TEST(test_registry_rejects_duplicate_name);
    RUN_TEST(test_registry_builds_valid_json);
    RUN_TEST(test_registry_reports_unknown_tool);
    RUN_TEST(test_registry_rejects_broken_json);
    RUN_TEST(test_registry_rejects_non_object);

    RUN_TEST(test_list_docs_returns_rows);
    RUN_TEST(test_list_docs_filters_by_folder);
    RUN_TEST(test_list_docs_empty_archive_is_not_error_text);
    RUN_TEST(test_list_docs_rejects_traversal_folder);

    RUN_TEST(test_read_doc_returns_title_and_body);
    RUN_TEST(test_read_doc_missing_has_actionable_error);
    RUN_TEST(test_read_doc_requires_path);
    RUN_TEST(test_read_doc_rejects_bad_path);

    RUN_TEST(test_search_docs_finds_and_formats);
    RUN_TEST(test_search_docs_empty_query_has_hint);
    RUN_TEST(test_search_docs_nothing_found_suggests_alternative);

    RUN_TEST(test_write_doc_creates);
    RUN_TEST(test_write_doc_updates_existing);
    RUN_TEST(test_write_doc_normalizes_path);
    RUN_TEST(test_write_doc_rejects_cyrillic_path_with_example);
    RUN_TEST(test_write_doc_rejects_traversal);
    RUN_TEST(test_write_doc_requires_title);
    RUN_TEST(test_write_doc_requires_content);
    RUN_TEST(test_write_doc_update_keeps_omitted_fields);
    RUN_TEST(test_write_doc_passes_optional_fields);
    RUN_TEST(test_write_doc_reports_storage_error);

    RUN_TEST(test_delete_doc_asks_confirmation_first);
    RUN_TEST(test_delete_doc_removes_after_confirmation);
    RUN_TEST(test_delete_doc_missing_file);
    RUN_TEST(test_delete_doc_rejects_traversal);

    RUN_TEST(test_schemas_are_valid_json_with_required);

    return UNITY_END();
}
