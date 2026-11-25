#pragma once

// Одна строка треда в формате JSONL: сборка и разбор.
//
// Формат выбран в ARCHITECTURE (5.2) за устойчивость: при обрыве записи
// теряется максимум последняя строка, а сам файл остаётся читаемым. Одна
// строка — одно сообщение, поля `role`, `content`, `ts`, плюс `usage` у ответа
// модели.
//
// Чистые функции без Arduino: SD и файловые операции — в hw/chat_store, здесь
// только текст.

#include <stddef.h>
#include <stdint.h>

#include "util/api_json.h"

namespace chatline {

/// Длина поля времени в ISO-8601: «2026-11-24T12:00:00+03:00» — 25 символов.
constexpr size_t kMaxTs = 32;

/// Длина идентификатора вызова инструмента. У RouterAI он вида `call_…`,
/// но формат не документирован — держим запас.
constexpr size_t kToolCallIdLen = 64;

/// Длина имени инструмента.
constexpr size_t kToolNameLen = 32;

/// Разобранная строка треда.
///
/// Текст НЕ хранится внутри: он указывает на буфер, который передали в parse().
/// Так сделано потому, что ArduinoJson 7 не поддерживает zero-copy — он
/// копирует строки в пул документа, а пул умирает вместе с документом. Ссылка
/// внутрь этого пула была бы висячей (на этом уже обжигались: тесты падали с
/// пустым content).
struct Entry {
    api::Role role{api::Role::User};
    /// Текст сообщения: указывает в буфер, переданный в parse().
    const char* content{nullptr};
    /// Длина текста в байтах.
    size_t content_len{0};
    /// Время сообщения в ISO-8601.
    char ts[kMaxTs]{};
    /// Токены ответа, 0 если не записаны.
    long input_tokens{0};
    long output_tokens{0};
    /// Ответ оборван (пользователь нажал Esc или пропала сеть): при загрузке
    /// такой ответ помечается, чтобы не выглядеть нормальным.
    bool partial{false};
    /// Для role == Tool: вызов, на который это сообщение отвечает.
    char tool_call_id[kToolCallIdLen]{};
    /// Для role == Tool: имя инструмента.
    char name[kToolNameLen]{};
    /// Для role == Assistant: массив вызовов как есть, из ответа модели.
    ///
    /// Хранить обязательно, даже если своими силами мы его уже выполнили:
    /// без сообщения ассистента с вызовами сервер не понимает, на что
    /// отвечают следующие сообщения role=tool. Указатель — в буфер,
    /// переданный в parse().
    const char* tool_calls{nullptr};
    size_t tool_calls_len{0};
};

/// Что писать в строку ответа помимо текста.
struct Usage {
    long input_tokens{0};
    long output_tokens{0};
    bool partial{false};
};

/// Собирает строку JSONL без завершающего перевода строки.
///
/// @param role    роль сообщения
/// @param content текст (может быть пустым, но не nullptr)
/// @param ts      время ISO-8601; nullptr — поле не писать
/// @param usage   счётчики для ответа модели; для других ролей не нужны
/// @return число байт без завершающего нуля, 0 если буфер мал
/// @param tool_call_id для role == Tool: идентификатор вызова
/// @param name         для role == Tool: имя инструмента
/// @param tool_calls   для role == Assistant: массив вызовов как есть
size_t build(const api::Role& role, const char* content, const char* ts, const Usage& usage, char* out, size_t out_len,
    const char* tool_call_id = nullptr, const char* name = nullptr, const char* tool_calls = nullptr);

/// Разбирает строку JSONL.
///
/// @param line        строка файла (без перевода строки)
/// @param len         её длина
/// @param out         результат; out.content будет указывать в content_out
/// @param content_out буфер, куда копируется текст сообщения
/// @param content_cap его размер
/// @return false, если строка битая, это служебная строка метаданных или текст
///         не помещается в content_out. Обрезать текст молча нельзя: обрезанный
///         ответ модели выглядит как законченный.
/// @param calls_out куда копировать `tool_calls` (может быть nullptr — тогда
///                  поле игнорируется)
/// @param calls_cap его размер; если массив не влез, строка считается битой
bool parse(char* line, size_t len, Entry& out, char* content_out, size_t content_cap, char* calls_out = nullptr,
    size_t calls_cap = 0);

/// Имя роли в JSON.
const char* role_name(api::Role role);

/// Роль по имени. Неизвестная роль — User: лучше показать сообщение не тем
/// цветом, чем потерять его.
api::Role role_from_name(const char* name);

/// Сколько байт займёт строка сообщения с запасом — для проверки лимита файла
/// перед записью.
size_t estimate_size(const char* content);

}  // namespace chatline
