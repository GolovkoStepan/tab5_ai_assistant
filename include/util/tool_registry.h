#pragma once

// Реестр инструментов агента: схемы, разбор аргументов, вызов обработчика.
//
// Что здесь и почему именно так.
//
// 1. **Схемы — строки, а не структуры.** Схему надо отдать в JSON запроса как
//    есть. Собирать её каждый раз из C++-описания значило бы тратить память и
//    получить расхождение между тем, что мы описали модели, и тем, что
//    проверяем. Одна строка — один источник правды.
//
// 2. **Валидатор пишем сами.** Живой прогон (Р-025) показал: сервер принимает
//    схему без `parameters` и вызывает инструмент с `{}`. То есть аргументы
//    проверяет не сервер, а мы. Ошибку возвращаем модели ТЕКСТОМ с примером
//    правильного значения — по такому тексту модель исправляется (проверено).
//
// 3. **Результат — текст, а не JSON.** Модели проще читать, и токенов уходит
//    меньше. Ошибка — тоже текст, но с префиксом «Ошибка:», чтобы модель
//    понимала, что вызов не удался, а не что в архиве так написано.
//
// 4. **Никаких исключений.** Arduino собирается с -fno-exceptions, а обработчик
//    работает с SD, где отказ — норма. Поэтому обработчик возвращает bool и
//    пишет причину в буфер.
//
// Модуль чистый: ни Arduino, ни SD. Инструменты над файлами — в отдельном
// слое, который получает DocStore. Так реестр тестируется на хосте.

#include <stddef.h>
#include <stdint.h>

#include "util/doc_api.h"

namespace tools {

/// Предел числа инструментов. Пять архивных плюс будущий `web_search`.
constexpr size_t kMaxTools = 8;

/// Максимальная длина текста результата, который уходит модели.
/// 4 КБ — из docs/ARCHITECTURE.md §4.4. Больше не нужно: модель всё равно
/// пересказывает, а контекст стоит денег и вытесняет историю диалога.
constexpr size_t kMaxResultBytes = 4096;

/// Контекст вызова: всё, что нужно обработчику и чего он не должен искать сам.
///
/// Буферы (результат, тело документа, массивы списка и поиска) выдаёт
/// вызывающий: обработчик не выделяет память. На плате это принципиально —
/// внутри `tick()` куча не должна шевелиться, иначе посреди диалога
/// начинается фрагментация.
struct Context {
    /// Буфер результата. Пишет обработчик, читает цикл агента.
    char* out{nullptr};
    size_t out_cap{0};
    /// Указатель на хранилище документов (docapi::Store) — как void*, чтобы
    /// реестр не зависел от реализации и собирался в env:native.
    void* store{nullptr};
    /// Буфер тела документа для read_doc/write_doc.
    char* body{nullptr};
    size_t body_cap{0};
    /// Массив под список документов (list_docs).
    docapi::Entry* entries{nullptr};
    size_t entries_cap{0};
    /// Массив под результаты поиска (search_docs).
    docapi::Hit* hits{nullptr};
    size_t hits_cap{0};
    /// Текущее время в ISO-8601 — его ставит вызывающий, а не модель.
    const char* now_iso{nullptr};
    /// Пользователь подтвердил необратимое действие (удаление).
    bool confirmed{false};
    /// Требуется подтверждение пользователя (delete_doc выставляет этот флаг,
    /// а решение принимает экран, не обработчик).
    bool needs_confirm{false};
    /// Что именно подтверждать — путь.
    char confirm_path[96]{};
    /// Вызов отклонён: обработчик не выполнил действие, а попросил
    /// подтверждения. Цикл агента не считает это шагом.
    bool deferred{false};
};

/// Обработчик инструмента.
///
/// @param args_json аргументы как JSON-объект (гарантированно валидный JSON,
///                  но не гарантированно с нужными полями)
/// @return true, если в out записан полезный результат; false — в out ошибка
using Handler = bool (*)(const char* args_json, size_t args_len, Context& ctx);

/// Описание одного инструмента.
struct Tool {
    const char* name{nullptr};
    /// JSON-схема параметров — как есть, в запрос.
    const char* schema{nullptr};
    Handler handler{nullptr};
};

class Registry {
public:
    Registry();

    /// Добавить инструмент. @return false, если мест нет или имя занято
    bool add(const char* name, const char* schema, Handler handler);

    /// Схемы всех инструментов одной строкой: `[{"type":"function",...}, ...]`.
    ///
    /// @return false, если не влезло в буфер
    bool build_tools_json(char* out, size_t out_len) const;

    /// Найти инструмент по имени. nullptr — такого нет.
    const Tool* find(const char* name) const;

    /// Вызвать инструмент по имени.
    ///
    /// @return true, если результат полезный (в ctx.out); false — ошибка
    ///         (тоже в ctx.out, текстом)
    bool invoke(const char* name, const char* args_json, size_t args_len, Context& ctx);

    size_t count() const { return _count; }
    const Tool& at(size_t i) const { return _tools[i]; }

private:
    Tool _tools[kMaxTools]{};
    size_t _count{0};
};

// --- Помощники для обработчиков --------------------------------------------

/// Достать строковое поле из JSON-аргументов.
///
/// @param args_json аргументы
/// @param args_len  их длина
/// @param key       имя поля
/// @param out       буфер
/// @param out_len   его размер
/// @return false, если поля нет, оно не строка или не влезло
bool arg_string(const char* args_json, size_t args_len, const char* key, char* out, size_t out_len);

/// Достать целое поле с умолчанием.
int arg_int(const char* args_json, size_t args_len, const char* key, int fallback);

/// Записать текст результата с усечением до kMaxResultBytes.
///
/// @return длина записанного (без нуля)
size_t set_result(Context& ctx, const char* text);

/// Записать результат по формату printf.
size_t set_resultf(Context& ctx, const char* fmt, ...);

/// Записать ошибку: префикс «Ошибка:» добавляется здесь, чтобы обработчики
/// не забывали его ставить.
size_t set_error(Context& ctx, const char* text);

}  // namespace tools
