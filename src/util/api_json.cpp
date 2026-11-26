// Сборка запроса и разбор ответа API. Без Arduino — тестируется на хосте.
//
// Тело запроса собираем вручную, а не через библиотеку JSON: так нет ни одной
// лишней аллокации (тело с историей легко достигает десятков килобайт, и его
// место — в PSRAM, а не в куче), а экранирование уже есть и покрыто тестами в
// util/settings. Разбор ответа, наоборот, идёт через ArduinoJson: там
// недоверенный ввод с вложенными объектами и \uXXXX-последовательностями, и
// свой парсер был бы источником дефектов.

#include "util/api_json.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "util/settings.h"

namespace api {
namespace {

const char* role_name(Role role)
{
    switch (role) {
    case Role::System: return "system";
    case Role::User: return "user";
    case Role::Assistant: return "assistant";
    case Role::Tool: return "tool";
    }
    return "user";
}

/// Дописывает строку в буфер, не выходя за его границы.
/// @return false, если места не хватило (буфер будет помечен как переполненный)
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
    // json_escape возвращает 0 и на пустой строке, и на переполнении, поэтому
    // пустую строку проверяем отдельно.
    if (n == 0 && text[0] != '\0') return false;
    *used += n;

    return put(out, out_len, used, "\"");
}

/// Копирует текст в буфер, обрезая по его размеру. Возвращает длину копии.
size_t copy_text(const char* src, char* dst, size_t dst_len)
{
    if (src == nullptr || dst == nullptr || dst_len == 0) return 0;
    const size_t n = strlen(src);
    const size_t copy = (n < dst_len - 1) ? n : dst_len - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    return copy;
}

/// Заполняет счётчики из блока usage.
///
/// RouterAI отдаёт input_tokens/output_tokens, OpenAI-совместимые клиенты —
/// prompt_tokens/completion_tokens. Читаем оба: иначе на одном из них счётчик
/// молча показывал бы нули.
void fill_usage(JsonVariant usage, Chunk& out)
{
    if (usage.isNull()) return;

    long in = usage["input_tokens"].as<long>();
    if (in == 0) in = usage["prompt_tokens"].as<long>();
    long outp = usage["output_tokens"].as<long>();
    if (outp == 0) outp = usage["completion_tokens"].as<long>();

    // Токены рассуждений тарифицируются как выходные, и при reasoning_effort
    // они бывают больше самого ответа: 48 против 12 в живом прогоне.
    long reasoning_tokens = usage["completion_tokens_details"]["reasoning_tokens"].as<long>();
    if (reasoning_tokens == 0) reasoning_tokens = usage["output_tokens_details"]["reasoning_tokens"].as<long>();

    if (in > 0 || outp > 0) {
        out.has_usage = true;
        out.input_tokens = in;
        out.output_tokens = outp;
        out.reasoning_tokens = reasoning_tokens;
    }
}

}  // namespace

size_t build_request(const OutMessage* msgs, size_t count, const RequestOptions& opt, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0 || (msgs == nullptr && count > 0) || opt.model == nullptr) return 0;

    size_t used = 0;
    out[0] = '\0';

    char number[32];
    snprintf(number, sizeof(number), "%.1f", opt.temperature_tenths / 10.0);

    bool ok = put(out, out_len, &used, "{\"model\":") && put_json_string(out, out_len, &used, opt.model) &&
              put(out, out_len, &used, ",\"messages\":[");

    for (size_t i = 0; ok && i < count; ++i) {
        if (i > 0) ok = put(out, out_len, &used, ",");

        ok = ok && put(out, out_len, &used, "{\"role\":") &&
             put_json_string(out, out_len, &used, role_name(msgs[i].role));

        // Ассистент с вызовами: content бывает null, а tool_calls обязателен.
        // Сервер сам content не требует, но пустая строка и null для него не
        // одно и то же — с null сообщение читается как «вызов без текста».
        if (ok && msgs[i].tool_calls_json != nullptr) {
            ok = put(out, out_len, &used, ",\"content\":null,\"tool_calls\":") &&
                 put(out, out_len, &used, msgs[i].tool_calls_json);
        } else {
            ok = ok && put(out, out_len, &used, ",\"content\":") &&
                 put_json_string(out, out_len, &used, msgs[i].content ? msgs[i].content : "");
        }

        // tool_call_id и name обязательны для role=tool: без них модель не
        // сопоставит результат с вызовом.
        if (ok && msgs[i].tool_call_id != nullptr) {
            ok = put(out, out_len, &used, ",\"tool_call_id\":") &&
                 put_json_string(out, out_len, &used, msgs[i].tool_call_id);
        }
        if (ok && msgs[i].name != nullptr) {
            ok = put(out, out_len, &used, ",\"name\":") && put_json_string(out, out_len, &used, msgs[i].name);
        }

        ok = ok && put(out, out_len, &used, "}");
    }

    ok = ok && put(out, out_len, &used, "]");

    if (ok) {
        ok = put(out, out_len, &used, ",\"stream\":") && put(out, out_len, &used, opt.stream ? "true" : "false") &&
             put(out, out_len, &used, ",\"temperature\":") && put(out, out_len, &used, number);
    }

    if (ok && opt.reasoning_effort != nullptr && opt.reasoning_effort[0] != '\0') {
        ok = put(out, out_len, &used, ",\"reasoning_effort\":") &&
             put_json_string(out, out_len, &used, opt.reasoning_effort);
    }

    if (ok && opt.max_tokens > 0) {
        snprintf(number, sizeof(number), "%u", opt.max_tokens);
        ok = put(out, out_len, &used, ",\"max_tokens\":") && put(out, out_len, &used, number);
    }

    // tools — массив как есть, собранный tools::Registry. Не проверяем его
    // здесь: схему описывает реестр, а builder только переносит.
    if (ok && opt.tools_json != nullptr && opt.tools_json[0] != '\0') {
        ok = put(out, out_len, &used, ",\"tools\":") && put(out, out_len, &used, opt.tools_json);
    }

    // plugins — веб-поиск. Живёт рядом с tools: они совместимы (Р-036).
    if (ok && opt.plugins_json != nullptr && opt.plugins_json[0] != '\0') {
        ok = put(out, out_len, &used, ",\"plugins\":") && put(out, out_len, &used, opt.plugins_json);
    }

    ok = ok && put(out, out_len, &used, "}");

    out[used] = '\0';
    return ok ? used : 0;
}

bool parse_chunk(const char* json, size_t len, Chunk& out, char* text_out, size_t text_cap, char* reasoning_out,
    size_t reasoning_cap)
{
    if (json == nullptr || len == 0) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull()) return false;

    if (!root["choices"].isNull() && !root["choices"].is<JsonArray>()) return false;

    JsonArray choices = root["choices"];
    if (!choices.isNull() && choices.size() > 0) {
        JsonObject choice = choices[0];

        // finish_reason приходит в последнем содержательном чанке; null в
        // промежуточных — это не ошибка и не повод считать ответ оборванным.
        JsonVariant finish = choice["finish_reason"];
        if (!finish.isNull()) {
            const char* reason = finish.as<const char*>();
            if (reason != nullptr && reason[0] != '\0') {
                snprintf(out.finish_reason, sizeof(out.finish_reason), "%s", reason);
                out.has_finish = true;
            }
        }

        // Текст дельты. Путь choices[0].delta.content — стандарт для стриминга.
        out.text_len = copy_text(choice["delta"]["content"].as<const char*>(), text_out, text_cap);
        out.has_text = out.text_len > 0;

        // Рассуждения. Проверено на живом RouterAI: при reasoning_effort,
        // отличном от "none", они идут отдельным полем delta.reasoning и
        // занимают больше времени, чем сам ответ.
        out.reasoning_len = copy_text(choice["delta"]["reasoning"].as<const char*>(), reasoning_out, reasoning_cap);
        out.has_reasoning = out.reasoning_len > 0;
    }

    fill_usage(root["usage"], out);
    return true;
}

bool parse_full_response(const char* json, size_t len, Chunk& out, char* text_out, size_t text_cap, char* reasoning_out,
    size_t reasoning_cap)
{
    if (json == nullptr || len == 0) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull()) return false;

    JsonArray choices = root["choices"];
    if (choices.isNull() || choices.size() == 0) return false;

    JsonObject choice = choices[0];

    // В нестриминговом ответе текст лежит в message, а не в delta.
    const char* text = choice["message"]["content"].as<const char*>();
    if (text != nullptr && text[0] != '\0' && text_out != nullptr && text_cap > 0) {
        const size_t n = strlen(text);
        const size_t copy = (n < text_cap - 1) ? n : text_cap - 1;
        memcpy(text_out, text, copy);
        text_out[copy] = '\0';
        out.text_len = copy;
        out.has_text = copy > 0;
    }

    // Рассуждения в нестриминговом ответе — отдельным полем message.reasoning,
    // а не внутри content. Если их не отделить, они попадут в чат как ответ.
    const char* reasoning = choice["message"]["reasoning"].as<const char*>();
    if (reasoning != nullptr && reasoning[0] != '\0' && reasoning_out != nullptr && reasoning_cap > 0) {
        const size_t n = strlen(reasoning);
        const size_t copy = (n < reasoning_cap - 1) ? n : reasoning_cap - 1;
        memcpy(reasoning_out, reasoning, copy);
        reasoning_out[copy] = '\0';
        out.reasoning_len = copy;
        out.has_reasoning = copy > 0;
    }

    const char* reason = choice["finish_reason"].as<const char*>();
    if (reason != nullptr && reason[0] != '\0') {
        snprintf(out.finish_reason, sizeof(out.finish_reason), "%s", reason);
        out.has_finish = true;
    }

    fill_usage(root["usage"], out);
    return true;
}

bool parse_error(const char* json, size_t len, int http_code, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return false;
    out[0] = '\0';

    if (json == nullptr || len == 0) {
        snprintf(out, out_len, "%s", http_error_text(http_code));
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
        // Тело не JSON (страница ошибки прокси): показываем HTTP-код, а не
        // кусок HTML на экране.
        snprintf(out, out_len, "%s", http_error_text(http_code));
        return false;
    }

    JsonVariant error = doc["error"];

    // Лимит трат по ключу: RouterAI отвечает 429 с текстом
    // «API key weekly spending limit exceeded: 100,22 руб. / 100,00 руб.».
    // Показывать его как есть бессмысленно — английский, и непонятно, что
    // делать. Но суммы из него полезны: человеку нужно знать, сколько
    // потрачено и каков потолок.
    if (quota_exhausted(http_code, json, len)) {
        const char* raw = nullptr;
        if (!error.isNull()) {
            raw = error.is<const char*>() ? error.as<const char*>() : error["message"].as<const char*>();
        }
        if (raw == nullptr) raw = doc["message"].as<const char*>();

        // Достаём числа «потрачено / потолок» из хвоста сообщения.
        double spent = 0;
        double limit = 0;
        bool have_numbers = false;
        if (raw != nullptr) {
            const char* colon = strrchr(raw, ':');
            if (colon != nullptr) {
                // Числа записаны с запятой как десятичным разделителем.
                char tail[128];
                snprintf(tail, sizeof(tail), "%s", colon + 1);
                for (char* p = tail; *p != '\0'; ++p) {
                    if (*p == ',') *p = '.';
                }
                have_numbers = sscanf(tail, " %lf %*s / %lf", &spent, &limit) == 2;
            }
        }
        if (have_numbers) {
            // В русском десятичный разделитель — запятая. Сервер присылает
            // «100,22 руб. / 100,00 руб.», и в таком же виде человеку привычнее.
            // Меняем только точки между цифрами: точки в конце предложений
            // трогать нельзя.
            snprintf(out, out_len, "Лимит трат по ключу исчерпан: %.2f из %.2f руб. Пополните баланс.", spent, limit);
            for (char* p = out + 1; *p != '\0'; ++p) {
                const bool digits_around = (p[-1] >= '0' && p[-1] <= '9') && (p[1] >= '0' && p[1] <= '9');
                if (*p == '.' && digits_around) *p = ',';
            }
        } else {
            snprintf(out, out_len, "Лимит трат по ключу исчерпан. Пополните баланс.");
        }
        return true;
    }

    if (error.isNull()) {
        const char* message = doc["message"].as<const char*>();
        if (message != nullptr && message[0] != '\0') {
            snprintf(out, out_len, "%s", message);
            return true;
        }
        snprintf(out, out_len, "%s", http_error_text(http_code));
        return false;
    }

    if (error.is<const char*>()) {
        const char* text = error.as<const char*>();
        if (text != nullptr && text[0] != '\0') {
            snprintf(out, out_len, "%s", text);
            return true;
        }
    } else {
        // {"error":{"message":"…","code":…}}
        const char* message = error["message"].as<const char*>();
        if (message != nullptr && message[0] != '\0') {
            snprintf(out, out_len, "%s", message);
            return true;
        }
    }

    snprintf(out, out_len, "%s", http_error_text(http_code));
    return false;
}

const char* http_error_text(int http_code)
{
    switch (http_code) {
    case 400: return "Запрос отклонён сервером";
    case 401: return "Неверный API-ключ";
    case 402: return "Закончился баланс";
    case 403: return "Доступ запрещён";
    case 404: return "Модель не найдена";
    case 413: return "Слишком длинный запрос";
    case 429: return "Слишком много запросов";
    case 500:
    case 502:
    case 503:
    case 504: return "Сервис недоступен";
    default: return "Ошибка сети";
    }
}

bool quota_exhausted(int http_code, const char* json, size_t len)
{
    // Лимит приходит с 429, но на всякий случай смотрим и 402: у разных
    // провайдеров «кончились деньги» кодируется по-разному.
    if (http_code != 429 && http_code != 402) return false;

    if (json == nullptr || len == 0) return http_code == 402;

    // Ищем по тексту, а не по полю `code`: в теле 429 поле `code` равно 429 —
    // это тот же HTTP-код, а не признак лимита.
    //
    // Подстроки подобраны по тем формулировкам, которые встречаются у RouterAI
    // и OpenAI-совместимых шлюзов. Регистр не важен: сервер пишет и
    // `spending limit`, и `Spending Limit`.
    static const char* const kMarkers[] = {
        "spending limit",
        "quota",
        "insufficient",
        "exceeded your current",
        "billing",
        "закончился баланс",
        "лимит",
    };

    char lower[512];
    const size_t copy = len < sizeof(lower) - 1 ? len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy; ++i) {
        const char c = json[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    lower[copy] = '\0';

    for (const char* marker : kMarkers) {
        if (strstr(lower, marker) != nullptr) return true;
    }
    return false;
}

}  // namespace api
