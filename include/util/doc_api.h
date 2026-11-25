#pragma once

// Интерфейс архива документов — без файловой системы.
//
// Зачем интерфейс, а не прямой вызов hw::DocStore. Инструменты агента
// (list_docs, read_doc, write_doc…) — это логика: что вернуть модели, как
// обрезать результат, какую ошибку написать. Проверять её надо на хосте, где
// нет SD_MMC, и с подставным хранилищем, где легко устроить «файла нет»,
// «путь с кириллицей», «слишком большой документ». Настоящая реализация
// (hw::DocStore) наследует этот интерфейс и добавляет только работу с картой.
//
// Виртуальные функции здесь уместны: вызовов десятки за диалог, а не тысячи
// за кадр. Экономить на vtable ценой невозможности тестировать — плохой обмен.

#include <stddef.h>
#include <stdint.h>

#include "util/doc_path.h"
#include "util/front_matter.h"

namespace docapi {

/// Сколько документов отдаёт list() за раз.
constexpr size_t kMaxListedDocs = 60;

/// Одна строка списка документов.
struct Entry {
    char path[paths::kMaxPathLen]{};
    char title[128]{};
    /// Дата обновления `ГГГГ-ММ-ДД` — только она нужна в списке.
    char updated[16]{};
    size_t bytes{0};
};

/// Один результат поиска.
struct Hit {
    char path[paths::kMaxPathLen]{};
    /// Сниппет вокруг совпадения, одна строка.
    char snippet[192]{};
};

class Store {
public:
    virtual ~Store() = default;

    /// Собрать список документов.
    ///
    /// @param folder подкаталог (`home`, `home/kitchen`) или пустая строка для
    ///               всего архива
    /// @return сколько заполнено
    virtual size_t list(const char* folder, Entry* out, size_t cap) = 0;

    /// Прочитать документ: метаданные и тело.
    ///
    /// Документ без фронтматтера тоже читается — meta.title пуст, тело есть.
    ///
    /// @return false, если файла нет, путь невалиден или документ слишком велик
    virtual bool read(
        const char* rel, doc::Meta& meta, char* body, size_t body_cap, size_t* body_len, char* err, size_t err_len) = 0;

    /// Прочитать только метаданные. Нужно списку: читать 64 КБ ради заголовка
    /// незачем.
    virtual bool read_meta(const char* rel, doc::Meta& meta, size_t* file_bytes) = 0;

    /// Записать документ (создать или перезаписать).
    ///
    /// @param created [выход] true, если файла раньше не было
    virtual bool write(
        const char* rel, const doc::Meta& meta, const char* body, bool* created, char* err, size_t err_len) = 0;

    /// Удалить документ.
    virtual bool remove(const char* rel, char* err, size_t err_len) = 0;

    /// Найти документы по подстроке без учёта регистра.
    ///
    /// @param limit сколько вернуть (0 — сколько влезет)
    virtual size_t search(const char* query, size_t limit, Hit* out, size_t cap) = 0;

    /// Есть ли документ по такому пути.
    virtual bool exists(const char* rel) = 0;
};

}  // namespace docapi
