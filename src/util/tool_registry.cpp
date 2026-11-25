// Реестр инструментов: схемы, аргументы, вызов. Без Arduino — тесты на хосте.

#include "util/tool_registry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

namespace tools {
namespace {

/// Префикс ошибки. Модель отличает «вызов не удался» от «в архиве так написано»
/// только по нему — это часть контракта, а не оформление.
constexpr const char* kErrorPrefix = "Ошибка: ";

}  // namespace

Registry::Registry()
{
    _count = 0;
}

bool Registry::add(const char* name, const char* schema, Handler handler)
{
    if (name == nullptr || schema == nullptr || handler == nullptr) return false;
    if (_count >= kMaxTools) return false;
    if (find(name) != nullptr) return false;

    _tools[_count].name = name;
    _tools[_count].schema = schema;
    _tools[_count].handler = handler;
    ++_count;
    return true;
}

const Tool* Registry::find(const char* name) const
{
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < _count; ++i) {
        if (strcmp(_tools[i].name, name) == 0) return &_tools[i];
    }
    return nullptr;
}

bool Registry::build_tools_json(char* out, size_t out_len) const
{
    if (out == nullptr || out_len == 0) return false;

    size_t n = 0;
    auto put = [&](const char* s) -> bool {
        const size_t len = strlen(s);
        if (n + len + 1 > out_len) return false;
        memcpy(out + n, s, len);
        n += len;
        out[n] = '\0';
        return true;
    };

    // Пустой список — тоже валидный JSON. Но пустым его отдавать нельзя:
    // модель с пустым tools иногда печатает DSML (Р-025). Пусть вызывающий
    // решает, передавать ли поле вовсе.
    if (!put("[")) return false;
    for (size_t i = 0; i < _count; ++i) {
        if (i > 0 && !put(", ")) return false;
        if (!put("{\"type\": \"function\", \"function\": ")) return false;
        if (!put(_tools[i].schema)) return false;
        if (!put("}")) return false;
    }
    return put("]");
}

bool Registry::invoke(const char* name, const char* args_json, size_t args_len, Context& ctx)
{
    if (ctx.out == nullptr || ctx.out_cap == 0) return false;
    ctx.out[0] = '\0';
    ctx.deferred = false;
    ctx.needs_confirm = false;
    ctx.confirm_path[0] = '\0';

    const Tool* t = find(name);
    if (t == nullptr) {
        // Модель может придумать имя. Это не повод рвать цикл: скажем ей, что
        // такого инструмента нет, и перечислим доступные.
        size_t used = set_resultf(ctx, "%sинструмента «%s» нет. Доступны:", kErrorPrefix, name != nullptr ? name : "");
        for (size_t i = 0; i < _count; ++i) {
            const size_t need = strlen(_tools[i].name) + 2;
            if (used + need + 1 > ctx.out_cap) break;
            ctx.out[used++] = ' ';
            const size_t n = strlen(_tools[i].name);
            memcpy(ctx.out + used, _tools[i].name, n);
            used += n;
            ctx.out[used] = '\0';
        }
        return false;
    }

    if (args_json == nullptr || args_len == 0) {
        set_resultf(ctx, "%sу инструмента «%s» нет аргументов. Нужен JSON-объект", kErrorPrefix, name);
        return false;
    }

    // Проверяем, что это вообще JSON-объект. Поля проверяет сам обработчик:
    // только он знает, какие из них обязательны и какой пример подсказать.
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, args_json, args_len);
    if (err) {
        set_resultf(ctx, "%sаргументы не разобрались как JSON: %s. Пример: {\"path\": \"home/note.md\"}", kErrorPrefix,
            err.c_str());
        return false;
    }
    if (!doc.is<JsonObjectConst>()) {
        set_resultf(ctx, "%sаргументы должны быть JSON-объектом вида {\"path\": \"home/note.md\"}", kErrorPrefix);
        return false;
    }

    return t->handler(args_json, args_len, ctx);
}

bool arg_string(const char* args_json, size_t args_len, const char* key, char* out, size_t out_len)
{
    if (args_json == nullptr || key == nullptr || out == nullptr || out_len == 0) return false;
    out[0] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, args_json, args_len)) return false;

    JsonVariantConst v = doc[key];
    if (!v.is<const char*>()) return false;

    const char* s = v.as<const char*>();
    if (s == nullptr) return false;
    const size_t n = strlen(s);
    if (n + 1 > out_len) return false;
    memcpy(out, s, n + 1);
    return true;
}

int arg_int(const char* args_json, size_t args_len, const char* key, int fallback)
{
    if (args_json == nullptr || key == nullptr) return fallback;
    JsonDocument doc;
    if (deserializeJson(doc, args_json, args_len)) return fallback;
    JsonVariantConst v = doc[key];
    if (!v.is<int>()) return fallback;
    return v.as<int>();
}

size_t set_result(Context& ctx, const char* text)
{
    if (ctx.out == nullptr || ctx.out_cap == 0) return 0;
    ctx.out[0] = '\0';
    if (text == nullptr) return 0;

    const size_t n = strlen(text);
    const size_t cap = ctx.out_cap - 1;
    const size_t take = n < cap ? n : cap;
    memcpy(ctx.out, text, take);
    ctx.out[take] = '\0';
    return take;
}

size_t set_resultf(Context& ctx, const char* fmt, ...)
{
    if (ctx.out == nullptr || ctx.out_cap == 0) return 0;
    ctx.out[0] = '\0';

    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(ctx.out, ctx.out_cap, fmt, ap);
    va_end(ap);

    if (n < 0) {
        ctx.out[0] = '\0';
        return 0;
    }
    if (static_cast<size_t>(n) >= ctx.out_cap) {
        // Усечение по границе UTF-8: резать букву пополам нельзя — модель
        // получит битый UTF-8 в аргументах следующего вызова.
        size_t cut = ctx.out_cap - 1;
        while (cut > 0 && (static_cast<unsigned char>(ctx.out[cut]) & 0xC0) == 0x80)
            --cut;
        ctx.out[cut] = '\0';
        return cut;
    }
    return static_cast<size_t>(n);
}

size_t set_error(Context& ctx, const char* text)
{
    if (ctx.out == nullptr || ctx.out_cap == 0) return 0;
    ctx.out[0] = '\0';
    if (text == nullptr) return 0;

    const size_t cap = ctx.out_cap - 1;
    const size_t prefix = strlen(kErrorPrefix);
    if (prefix >= cap) return 0;
    memcpy(ctx.out, kErrorPrefix, prefix);

    const size_t n = strlen(text);
    const size_t take = n < cap - prefix ? n : cap - prefix;
    memcpy(ctx.out + prefix, text, take);
    ctx.out[prefix + take] = '\0';
    return prefix + take;
}

}  // namespace tools
