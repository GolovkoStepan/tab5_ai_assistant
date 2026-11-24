// Обрезка истории: правила, из-за которых «просто выбросить старое» не годится.
//
// Главная ловушка — сообщение `tool`: в API оно обязано ссылаться на
// инструмент, который модель вызвала раньше. Оторвать его от вызова значит
// получить 400 от сервера, причём не на клиенте, а после отправки истории —
// то есть «диалог сломался сам по себе».
//
// Отсюда правила поверх бюджета:
//   1. Системное сообщение сохраняется всегда и в бюджет не входит.
//   2. История не начинается с результата инструмента.
//   3. Последний вопрос пользователя остаётся всегда — иначе история
//      превращается в ответ без вопроса.

#include "util/chat_history.h"

namespace history {
namespace {

/// Индекс, на котором заканчивается допустимая история.
///
/// Цепочка `tool` допустима, только если перед ней шло сообщение ассистента
/// (в реальном диалоге — его вызов инструмента). Первая же цепочка без такого
/// предшественника означает, что вызов остался в отрезанной части, и всё, что
/// после неё, отправить нельзя.
size_t find_end(const Message* msgs, size_t start, size_t count)
{
    api::Role last_non_tool = api::Role::User;

    for (size_t i = start; i < count; ++i) {
        if (msgs[i].role == api::Role::Tool) {
            if (last_non_tool != api::Role::Assistant) return i;
            continue;
        }
        last_non_tool = msgs[i].role;
    }
    return count;
}

}  // namespace

size_t total_bytes(const Message* msgs, size_t count, size_t from)
{
    size_t sum = 0;
    for (size_t i = from; i < count; ++i) {
        sum += msgs[i].content_len;
    }
    return sum;
}

TrimResult trim(const Message* msgs, size_t count, size_t budget)
{
    TrimResult result;

    if (msgs == nullptr || count == 0) return result;

    // Системное сообщение — только если оно действительно первое.
    const bool has_system = msgs[0].role == api::Role::System;
    const size_t first = has_system ? 1 : 0;

    // Идём с конца, набирая бюджет: свежие сообщения важнее старых. Хотя бы
    // одно сообщение берём всегда, даже если оно одно больше бюджета: пустой
    // запрос бессмыслен, а сервер сам скажет, что промпт не влез.
    size_t start = count;
    size_t bytes = 0;
    for (size_t i = count; i > first; --i) {
        const size_t b = msgs[i - 1].content_len;
        if (bytes + b > budget && start != count) break;
        bytes += b;
        start = i - 1;
    }

    // Результат инструмента без вызова в начале отправить нельзя.
    while (start < count && msgs[start].role == api::Role::Tool)
        ++start;

    // Последний вопрос пользователя остаётся всегда: ответ без вопроса модель
    // поймёт превратно, а пользователь — как «опять сломалось».
    size_t last_user = count;
    for (size_t i = count; i > first; --i) {
        if (msgs[i - 1].role == api::Role::User) {
            last_user = i - 1;
            break;
        }
    }
    if (last_user < count && start > last_user) start = last_user;

    const size_t end = find_end(msgs, start, count);

    // Системное сообщение уходит первым, поэтому при его наличии отправляемый
    // кусок начинается с нуля.
    result.begin = has_system ? 0 : start;
    result.end = end;
    result.kept = (end > start ? end - start : 0) + (has_system ? 1 : 0);
    result.dropped = start > first ? start - first : 0;
    result.bytes = total_bytes(msgs, end, start);
    result.kept_system = has_system;
    return result;
}

}  // namespace history
