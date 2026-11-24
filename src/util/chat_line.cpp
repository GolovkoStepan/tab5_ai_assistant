// Строка треда в формате JSONL: сборка и разбор. Без Arduino.
//
// Сборка вручную, как и тело запроса: строки треда пишутся на SD по одной, и
// лишняя аллокация на каждое сообщение ни к чему. Разбор — ArduinoJson:
// недоверенный ввод с экранированием, свой парсер был бы источником дефектов.

#include "util/chat_line.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "util/settings.h"

namespace chatline {
namespace {

/// Дописывает строку в буфер.
bool put(char* out, size_t out_len, size_t* used, const char* text)
{
    const size_t n = strlen(text);
    if (*used + n + 1 > out_len) return false;
    memcpy(out + *used, text, n);
    *used += n;
    out[*used] = '\0';
    return true;
}

/// Дописывает экранированную строку в кавычках.
bool put_json_string(char* out, size_t out_len, size_t* used, const char* text)
{
    if (!put(out, out_len, used, "\"")) return false;

    const size_t room = out_len - *used;
    const size_t n = settings::json_escape(text, out + *used, room);
    if (n == 0 && text[0] != '\0') return false;
    *used += n;

    return put(out, out_len, used, "\"");
}

}  // namespace

const char* role_name(api::Role role)
{
    switch (role) {
    case api::Role::System: return "system";
    case api::Role::User: return "user";
    case api::Role::Assistant: return "assistant";
    case api::Role::Tool: return "tool";
    }
    return "user";
}

api::Role role_from_name(const char* name)
{
    if (name == nullptr) return api::Role::User;
    if (strcmp(name, "system") == 0) return api::Role::System;
    if (strcmp(name, "assistant") == 0) return api::Role::Assistant;
    if (strcmp(name, "tool") == 0) return api::Role::Tool;
    return api::Role::User;
}

size_t estimate_size(const char* content)
{
    // Кавычки, экранирование и служебные поля — с запасом в полтора раза:
    // точнее считать незачем, оценка нужна только для лимита файла.
    const size_t n = content != nullptr ? strlen(content) : 0;
    return n + n / 2 + 128;
}

size_t build(const api::Role& role, const char* content, const char* ts, const Usage& usage, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;

    size_t used = 0;
    out[0] = '\0';

    bool ok = put(out, out_len, &used, "{\"role\":") && put_json_string(out, out_len, &used, role_name(role));
    ok = ok && put(out, out_len, &used, ",\"content\":") &&
         put_json_string(out, out_len, &used, content != nullptr ? content : "");

    if (ok && ts != nullptr && ts[0] != '\0') {
        ok = put(out, out_len, &used, ",\"ts\":") && put_json_string(out, out_len, &used, ts);
    }

    if (ok && role == api::Role::Assistant && (usage.input_tokens > 0 || usage.output_tokens > 0)) {
        char tokens[64];
        snprintf(tokens, sizeof(tokens), ",\"usage\":{\"input\":%ld,\"output\":%ld}", usage.input_tokens,
            usage.output_tokens);
        ok = put(out, out_len, &used, tokens);
    }

    // Пометка неполного ответа: без неё оборванный ответ выглядит как обычный,
    // и пользователь не понимает, почему модель замолчала на полуслове.
    if (ok && role == api::Role::Assistant && usage.partial) {
        ok = put(out, out_len, &used, ",\"partial\":true");
    }

    ok = ok && put(out, out_len, &used, "}");
    return ok ? used : 0;
}

bool parse(char* line, size_t len, Entry& out, char* content_out, size_t content_cap)
{
    if (line == nullptr || len == 0 || content_out == nullptr || content_cap == 0) return false;

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, line, len);
    if (err != DeserializationError::Ok) return false;

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull()) return false;

    // Служебная строка метаданных треда (`{"type":"meta",...}`) — не сообщение.
    JsonVariant type = root["type"];
    if (!type.isNull()) return false;

    const char* role = root["role"].as<const char*>();
    if (role == nullptr) return false;

    out = Entry{};
    out.role = role_from_name(role);

    // Текст копируем в буфер вызывающего: пул документа умирает на выходе, и
    // указатель внутрь него был бы висячим. ArduinoJson 7 zero-copy не делает,
    // даже когда разбирает изменяемый буфер.
    const char* content = root["content"] | "";
    const size_t content_len = strlen(content);
    if (content_len + 1 > content_cap) return false;
    memcpy(content_out, content, content_len + 1);
    out.content = content_out;
    out.content_len = content_len;

    const char* ts = root["ts"].as<const char*>();
    if (ts != nullptr) snprintf(out.ts, sizeof(out.ts), "%s", ts);

    JsonObject usage = root["usage"];
    if (!usage.isNull()) {
        out.input_tokens = usage["input"].as<long>();
        out.output_tokens = usage["output"].as<long>();
    }

    out.partial = root["partial"] | false;
    return true;
}

}  // namespace chatline
