#include "util/web_search.h"

#include <stdio.h>
#include <string.h>

#include "util/api_json.h"

namespace wsearch {
namespace {

/// Системное сообщение поискового подзапроса.
///
/// Короткое намеренно: задача узкая — найти и пересказать факты. Тот же
/// системный промпт, что у диалога (архив, инструменты, стиль ответа), здесь
/// был бы вреден: инструментов в этом запросе нет, и модель начала бы о них
/// писать — за это мы платим как за обычный ответ.
const char* const kSearchSystem =
    "Ты — поисковый помощник. Ответь на вопрос по существу, кратко и по-русски. "
    "Опирайся на найденные в интернете страницы. Не используй эмодзи и markdown-разметку. "
    "Не перечисляй ссылки — их добавит устройство. Если в найденных страницах ответа нет, "
    "скажи об этом прямо.";

/// Сколько места отдаём пересказу внутри результата: ссылки важнее, а весь
/// результат — kMaxResultBytes.
constexpr size_t kAnswerPart = 1100;

/// Потолок выходных токенов пересказа. 600 токенов — это заметно больше
/// kAnswerPart, но с запасом на служебные символы: обрывать ответ на середине
/// хуже, чем получить его целиком и обрезать по своим правилам.
constexpr unsigned kMaxAnswerTokens = 600;

/// Отрезать хвостовые пробелы и переводы строк.
void strip_tail(char* text, size_t& len)
{
    while (len > 0) {
        const char c = text[len - 1];
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
        --len;
    }
    text[len] = '\0';
}

}  // namespace

const char* const kToolSchema =
    "{\"name\": \"web_search\", \"description\": \"Найти информацию в интернете. Зови, только если нужны "
    "внешние данные: свежие факты, инструкции, характеристики приборов — или ответ на вопрос, которого нет "
    "в архиве документов. Не зови на приветствия, благодарности, короткие реплики и вопросы о самой беседе. "
    "Один вызов — один поисковый запрос.\", \"parameters\": {\"type\": \"object\", \"properties\": {"
    "\"query\": {\"type\": \"string\", \"description\": \"Поисковый запрос, например: Xiaomi Mi Air Purifier 3 "
    "сброс счётчика фильтра\"}, \"max_results\": {\"type\": \"integer\", \"description\": \"Сколько источников "
    "вернуть: по умолчанию 5, максимум 10\"}}, \"required\": [\"query\"]}}";

size_t build_body(const char* query, const char* model, uint8_t max_results, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    if (query == nullptr || query[0] == '\0' || model == nullptr || model[0] == '\0') return 0;

    // Границы те же, что у плагина: 0 сервер молча заменит своим умолчанием
    // (5), больше kMaxSources нам некуда девать.
    uint8_t n = max_results == 0 ? kDefaultResults : max_results;
    if (n > web::kMaxSources) n = web::kMaxSources;

    char plugins[64];
    if (web::build_plugins_json(n, plugins, sizeof(plugins)) == 0) return 0;

    // Тело собирает тот же api::build_request, что и основной запрос: экранирование,
    // порядок полей и формат `plugins` живут в одном месте и проверяются тестами.
    api::OutMessage msgs[2];
    msgs[0].role = api::Role::System;
    msgs[0].content = kSearchSystem;
    msgs[1].role = api::Role::User;
    msgs[1].content = query;

    api::RequestOptions opt;
    opt.model = model;
    // Низкая температура: поиск — не творчество, нужен точный пересказ.
    opt.temperature_tenths = 2;
    opt.stream = true;

    // `reasoning_effort: none` — рассуждения в поисковом подзапросе не нужны,
    // но тарифицируются как выходные токены (Р-036 п. 8: до 4305 из 4787).
    opt.reasoning_effort = "none";
    opt.max_tokens = kMaxAnswerTokens;

    // `tools` нет намеренно: иначе модель внутри поиска могла бы снова
    // попросить инструмент, и получилась бы рекурсия.
    opt.plugins_json = plugins;

    return api::build_request(msgs, 2, opt, out, out_len);
}

size_t format_result(
    const char* answer, size_t answer_len, const web::List* list, char* out, size_t out_len, size_t list_from)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';

    // Источники, пришедшие ИМЕННО этим поиском. Список на экране один на ход,
    // и без этого отсечения второй поиск отчитался бы ссылками первого.
    const size_t total = list != nullptr ? list->count : 0;
    if (list_from > total) list_from = total;
    const size_t n_sources = total - list_from;

    const bool has_answer = answer != nullptr && answer_len > 0;
    if (!has_answer && n_sources == 0) {
        // Ни текста, ни ссылок. Модель должна узнать об этом явно, иначе
        // начнёт отвечать по памяти так, будто поиск что-то дал.
        static const char kNone[] = "Поиск ничего не нашёл. Отвечай по памяти и скажи, что данные не проверены.";
        const size_t n = sizeof(kNone) - 1;
        if (n + 1 > out_len) return 0;
        memcpy(out, kNone, n + 1);
        return n;
    }

    size_t used = 0;

    if (has_answer) {
        // Границы применяем ДО проверки UTF-8: если сначала отрезать по
        // kAnswerPart, а потом по размеру буфера, второй отказ разрежет
        // кириллицу пополам, и модель получит битый глиф.
        size_t take = answer_len < kAnswerPart ? answer_len : kAnswerPart;
        const size_t room = out_len - 1;
        if (take > room) take = room;

        // Обрезка по границе UTF-8: байт-продолжение (10xxxxxx) в конце
        // означает разрезанный символ — отступаем к его началу.
        while (take > 0 && (static_cast<unsigned char>(answer[take]) & 0xC0) == 0x80)
            --take;

        memcpy(out, answer, take);
        used = take;
        out[used] = '\0';
        strip_tail(out, used);
    }

    if (n_sources > 0) {
        // Список отдельной строкой после пересказа. Если пересказа не было,
        // пустая строка перед «Источники:» не нужна.
        const int head = snprintf(out + used, out_len - used, "%sИсточники:", used > 0 ? "\n\n" : "");
        if (head > 0 && static_cast<size_t>(head) < out_len - used) {
            used += static_cast<size_t>(head);

            for (size_t i = 0; i < n_sources; ++i) {
                const web::Source& src = list->items[list_from + i];
                // Модели нужен полный адрес, а не «домен — заголовок»: в
                // отличие от ленты чата, здесь ссылка — рабочий материал для
                // поля `source_url` заметки.
                const char* title = src.title[0] != '\0' ? src.title : src.url;
                const int need =
                    snprintf(out + used, out_len - used, "\n%u. %s — %s", static_cast<unsigned>(i + 1), src.url, title);
                if (need <= 0 || static_cast<size_t>(need) >= out_len - used) {
                    out[used] = '\0';
                    break;
                }
                used += static_cast<size_t>(need);
            }
        }
    }

    return used;
}

namespace {

/// Обязательное строковое поле с примером в отказе: по такому тексту модель
/// исправляет аргументы (проверено, Р-025).
bool need_query(tools::Context& ctx, const char* args, size_t args_len, char* out, size_t out_len)
{
    if (!tools::arg_string(args, args_len, "query", out, out_len)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "в аргументах нет поля «query» или оно не строка. Пример: {\"query\": \"сброс фильтра "
            "очистителя Xiaomi\"}");
        tools::set_error(ctx, msg);
        return false;
    }

    // Модель иногда присылает запрос с ведущими или хвостовыми пробелами —
    // поисковик на такое отвечает хуже, а нам это ничего не стоит.
    size_t start = 0;
    while (out[start] == ' ' || out[start] == '\n' || out[start] == '\r' || out[start] == '\t')
        ++start;
    if (start > 0) {
        const size_t n = strlen(out + start);
        memmove(out, out + start, n + 1);
    }

    size_t end = strlen(out);
    while (end > 0) {
        const char c = out[end - 1];
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
        out[--end] = '\0';
    }

    if (out[0] == '\0') {
        tools::set_error(ctx, "поле «query» пустое. Пример: {\"query\": \"сброс фильтра очистителя Xiaomi\"}");
        return false;
    }
    return true;
}

}  // namespace

bool web_search_tool(const char* args, size_t args_len, tools::Context& ctx)
{
    char query[kMaxQueryLen + 1];
    if (!need_query(ctx, args, args_len, query, sizeof(query))) return false;

    // Обработчик не ходит в сеть: он синхронный, а поиск — запрос на 20-30 с.
    // Он только складывает запрос в контекст и просит экран выполнить поиск.
    // Так же устроено подтверждение удаления (`needs_confirm`).
    snprintf(ctx.search_query, sizeof(ctx.search_query), "%s", query);

    const int asked = tools::arg_int(args, args_len, "max_results", 0);
    // 0 — модель число не назвала. Тогда берём настройку устройства в
    // begin_search(): иначе поле «Источников в ответе» в Ctrl+A молча не
    // работало бы, и это выглядело бы сломанной настройкой.
    ctx.search_max_results = static_cast<uint8_t>(asked > 0 ? (asked > 10 ? 10 : asked) : 0);

    ctx.needs_search = true;
    ctx.deferred = true;

    tools::set_resultf(ctx, "Ищу в интернете: %s", query);
    return false;
}

bool register_tool(tools::Registry& reg)
{
    return reg.add("web_search", kToolSchema, web_search_tool);
}

}  // namespace wsearch
