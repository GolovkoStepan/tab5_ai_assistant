#pragma once

// Инструменты архива над DocStore: list_docs, read_doc, search_docs,
// write_doc, delete_doc.
//
// Схемы описаны по-русски: модель у нас русскоязычная, и живой прогон
// подтвердил, что русские описания работают не хуже английских (Р-025).
// В описании пути прямо сказано «латиница, цифры, дефис» — модель это
// соблюдает, и валидация почти никогда не срабатывает. Но она обязательна:
// на «сохрани заметку» модель уверенно предложила
// «Архив/Замена фильтра очистителя воздуха.md».
//
// Модуль зависит от DocStore, но не от SD напрямую: у него интерфейс через
// void*, как в util/tool_registry.h. Поэтому обработчики можно проверить на
// хосте подменой хранилища.

#include <stddef.h>

#include "util/tool_registry.h"

namespace doctools {

/// Зарегистрировать все инструменты архива в реестре.
///
/// @return false, если реестр переполнен (маловероятно: инструментов пять)
bool register_all(tools::Registry& reg);

/// Схемы — отдельно, чтобы тесты могли проверить, что они валидный JSON
/// и содержат обязательные поля.
namespace schema {
extern const char* kListDocs;
extern const char* kReadDoc;
extern const char* kSearchDocs;
extern const char* kWriteDoc;
extern const char* kDeleteDoc;
}  // namespace schema

}  // namespace doctools
