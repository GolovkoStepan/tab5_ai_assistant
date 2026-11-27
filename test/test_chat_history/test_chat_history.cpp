/*
 * Тесты обрезки истории.
 *
 * Проверяется не «сколько осталось», а корректность запроса после обрезки:
 * сервер отклоняет историю, которая начинается с результата инструмента или
 * где `tool` оторван от своего вызова. Такие запросы падают с 400 уже после
 * отправки, и на устройстве это выглядит как «диалог перестал работать».
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/chat_history.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

history::Message msg(api::Role role, const char* content)
{
    history::Message m;
    m.role = role;
    m.content = content;
    m.content_len = strlen(content);
    return m;
}

/// Сообщение ассистента с вызовами инструментов: текст пуст (так и приходит
/// ход с инструментом), а `tool_calls` занимает байты в теле запроса.
history::Message msg_with_calls(const char* calls)
{
    history::Message m;
    m.role = api::Role::Assistant;
    m.content = "";
    m.content_len = 0;
    m.tool_calls = calls;
    m.tool_calls_len = strlen(calls);
    return m;
}

}  // namespace

/// Всё влезает — ничего не режем.
void test_no_trim_when_fits()
{
    const history::Message msgs[] = {
        msg(api::Role::System, "sys"),
        msg(api::Role::User, "привет"),
        msg(api::Role::Assistant, "здравствуйте"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 1000);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(3, r.end);
    TEST_ASSERT_EQUAL_UINT32(3, r.kept);
    TEST_ASSERT_EQUAL_UINT32(0, r.dropped);
    TEST_ASSERT_TRUE(r.kept_system);
}

/// Системное сообщение сохраняется даже при нулевом бюджете: без него модель
/// ведёт себя иначе, и это выглядит как «ассистент сломался».
void test_system_always_kept()
{
    const history::Message msgs[] = {
        msg(api::Role::System, "ты помощник"),
        msg(api::Role::User, "вопрос"),
        msg(api::Role::Assistant, "ответ"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 1);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_TRUE(r.kept_system);
    TEST_ASSERT_TRUE(r.kept >= 2);  // системное + хотя бы вопрос
}

/// Режем с начала: свежие сообщения важнее старых.
void test_trims_oldest_first()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "1111111111"),
        msg(api::Role::Assistant, "2222222222"),
        msg(api::Role::User, "3333333333"),
        msg(api::Role::Assistant, "4444444444"),
    };

    const history::TrimResult r = history::trim(msgs, 4, 20);

    TEST_ASSERT_EQUAL_UINT32(2, r.begin);
    TEST_ASSERT_EQUAL_UINT32(2, r.dropped);
    TEST_ASSERT_EQUAL_UINT32(2, r.kept);
    TEST_ASSERT_EQUAL_STRING("3333333333", msgs[r.begin].content);
}

/// Последний вопрос пользователя не выбрасывается никогда: иначе модель
/// получает ответ без вопроса.
void test_last_user_question_always_kept()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "давний вопрос"),
        msg(api::Role::Assistant, "давний ответ"),
        msg(api::Role::User, "свежий вопрос, который длиннее бюджета"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 5);

    TEST_ASSERT_EQUAL_UINT32(2, r.begin);
    TEST_ASSERT_TRUE(r.end > r.begin);
    TEST_ASSERT_EQUAL_STRING("свежий вопрос, который длиннее бюджета", msgs[r.begin].content);
}

/// Самый опасный случай: начало истории попадает на результат инструмента.
/// Такой запрос сервер отклонит с 400, потому что вызова в истории уже нет.
void test_never_starts_with_tool()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "найди документ"),
        msg(api::Role::Assistant, "сейчас поищу"),
        msg(api::Role::Tool, "результат поиска"),
        msg(api::Role::Assistant, "вот что нашёл"),
        msg(api::Role::User, "спасибо"),
    };

    const history::TrimResult r = history::trim(msgs, 5, 30);

    TEST_ASSERT_TRUE(r.begin < 5);
    TEST_ASSERT_TRUE(msgs[r.begin].role != api::Role::Tool);
}

/// Осиротевшая цепочка инструментов обрезает историю справа: продолжать её
/// нельзя, вызова в отправляемой части нет.
void test_orphan_tool_chain_cuts_right_side()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "вопрос"),
        msg(api::Role::Tool, "tool без вызова"),
        msg(api::Role::Assistant, "после вызова"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 1000);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(1, r.end);
    TEST_ASSERT_EQUAL_UINT32(1, r.kept);
}

/// Пара «вызов ассистента → результат инструмента» допустима целиком.
void test_tool_after_assistant_is_valid()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "найди"),
        msg(api::Role::Assistant, "ищу"),
        msg(api::Role::Tool, "нашёл"),
        msg(api::Role::Assistant, "вот"),
    };

    const history::TrimResult r = history::trim(msgs, 4, 1000);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(4, r.end);
    TEST_ASSERT_EQUAL_UINT32(4, r.kept);
}

/// Пустая история — не падаем и не разыменовываем nullptr.
void test_empty_history()
{
    const history::TrimResult r = history::trim(nullptr, 0, 100);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(0, r.kept);
}

/// Если в истории только системное сообщение — остаётся оно.
void test_only_system_message()
{
    const history::Message msgs[] = {msg(api::Role::System, "sys")};

    const history::TrimResult r = history::trim(msgs, 1, 100);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(1, r.kept);
    TEST_ASSERT_TRUE(r.kept_system);
}

/// Сумма байт считается только по указанным сообщениям.
void test_bytes_counted_for_kept_only()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "12345"),
        msg(api::Role::Assistant, "123"),
    };

    TEST_ASSERT_EQUAL_UINT32(8, history::total_bytes(msgs, 2));
    TEST_ASSERT_EQUAL_UINT32(3, history::total_bytes(msgs, 2, 1));
}

/// Системное сообщение в бюджете не участвует: оно сохраняется всегда, и
/// вычитать его из бюджета значило бы незаслуженно отрезать живой диалог.
void test_system_not_counted_in_budget()
{
    const history::Message msgs[] = {
        msg(api::Role::System, "длинная инструкция длинная инструкция длинная инструкция длинная инструкция"),
        msg(api::Role::User, "abcd"),
        msg(api::Role::Assistant, "efgh"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 8);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(3, r.kept);
}

/// Первое сообщение диалога больше бюджета — всё равно отправляем: пустой
/// запрос хуже длинного, а сервер сам скажет, что не влезло.
void test_single_oversized_message_still_sent()
{
    const history::Message msgs[] = {msg(api::Role::User, "очень длинный вопрос")};

    const history::TrimResult r = history::trim(msgs, 1, 4);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(1, r.kept);
}

/// Вызовы инструментов входят в бюджет: они уходят в тело запроса, и раньше их
/// пропуск делал обрезку бесполезной на длинном треде с записями заметок.
///
/// Здесь текст одного сообщения — 2 байта, а вызовов 100. Бюджет 50: по тексту
/// всё влезло бы, по факту — нет, и старое сообщение с вызовами должно уйти.
void test_tool_calls_counted_in_budget()
{
    // Ровно 100 байт вызовов: массив нулей дал бы strlen() == 0, и проверка
    // прошла бы вхолостую — поэтому заполняем явно.
    char calls[101];
    memset(calls, 'x', 100);
    calls[100] = '\0';

    const history::Message msgs[] = {
        msg(api::Role::User, "ab"),
        msg_with_calls(calls),
        msg(api::Role::User, "cd"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 50);

    // Сообщение с вызовами одно занимает 100 байт — бюджет 50 не вмещает и его,
    // но свежие сообщения важнее: остаётся последнее, оно тоже больше бюджета.
    TEST_ASSERT_EQUAL_UINT32(2, r.begin);
    TEST_ASSERT_EQUAL_UINT32(1, r.kept);
}

/// Если вызовы влезают вместе с текстом — ничего не режем. Проверка, что
/// изменился именно подсчёт, а не всё поведение обрезки.
void test_tool_calls_fit_keeps_history()
{
    const history::Message msgs[] = {
        msg(api::Role::User, "вопрос"),
        msg_with_calls("{\"a\":1}"),
        msg(api::Role::User, "ещё"),
    };

    const history::TrimResult r = history::trim(msgs, 3, 100);

    TEST_ASSERT_EQUAL_UINT32(0, r.begin);
    TEST_ASSERT_EQUAL_UINT32(3, r.kept);
}

/// Воспроизведение дефекта Р-047 на хосте: старый подсчёт (только текст)
/// выбирал историю, которая в тело запроса не влезает, новый — влезает.
///
/// Это не рассуждение, а проверка: обе истории одинаковы по тексту и вызовам,
/// разница только в `tool_calls_len`. Первая — как считал старый код (0),
/// вторая — как считает новый. Дальше обе идут в настоящий `api::build_request`
/// с буфером тела прошивки (128 КБ).
void test_old_accounting_overflows_real_body()
{
    // Буфер тела как в прошивке (kBodyCap в ui/chat_screen.cpp) и бюджет
    // истории как в прошивке (kHistoryBudgetBytes).
    constexpr size_t kBody = 128 * 1024;
    constexpr size_t kBudget = 48 * 1024;

    // Аргументы write_doc с текстом заметки: 34 КБ на вызов — это не выдумка,
    // столько даёт восемь вызовов по 4 КБ (kMaxArgsLen в util/tool_calls.h).
    static char calls[34001];
    memset(calls, 'x', 34000);
    calls[34000] = '\0';

    // Текст сообщений: 3 КБ у вопроса и 3 КБ у результата инструмента.
    static char text[3001];
    memset(text, 'y', 3000);
    text[3000] = '\0';

    constexpr size_t kTriples = 10;
    constexpr size_t kCount = kTriples * 3;

    static history::Message old_msgs[kCount];
    static history::Message new_msgs[kCount];
    const size_t text_len = strlen(text);

    for (size_t i = 0; i < kCount; ++i) {
        history::Message m;
        const size_t slot = i % 3;
        if (slot == 1) {
            // Ответ модели с вызовами: текста нет, вызовы есть (как на плате).
            m.role = api::Role::Assistant;
            m.content = "";
            m.content_len = 0;
            m.tool_calls = calls;
            m.tool_calls_len = strlen(calls);
        } else {
            m.role = api::Role::User;
            m.content = text;
            m.content_len = text_len;
        }

        new_msgs[i] = m;
        // Старый учёт: вызовы в бюджет не входили.
        old_msgs[i] = m;
        old_msgs[i].tool_calls_len = 0;
    }

    const history::TrimResult old_trim = history::trim(old_msgs, kCount, kBudget);
    const history::TrimResult new_trim = history::trim(new_msgs, kCount, kBudget);

    // Старый учёт оставил заметно больше сообщений — в этом и была ошибка.
    TEST_ASSERT_GREATER_THAN_UINT32(new_trim.kept, old_trim.kept);

    static char body[kBody];
    api::RequestOptions opt;
    opt.model = "m";

    auto build = [&](const history::Message* src, const history::TrimResult& t) {
        static api::OutMessage out[kCount + 1];
        size_t n = 0;
        for (size_t i = t.begin; i < t.end; ++i) {
            out[n].role = src[i].role;
            out[n].content = src[i].content;
            out[n].tool_call_id = nullptr;
            out[n].name = nullptr;
            out[n].tool_calls_json = src[i].tool_calls;
            ++n;
        }
        return api::build_request(out, n, opt, body, sizeof(body));
    };

    // Старый выбор в буфер прошивки не влезает — это и был отказ хода
    // «История не собралась».
    TEST_ASSERT_EQUAL_UINT32(0, build(old_msgs, old_trim));

    // Новый — влезает, и заметно ниже предела.
    const size_t new_len = build(new_msgs, new_trim);
    TEST_ASSERT_GREATER_THAN_UINT32(0, new_len);
    TEST_ASSERT_LESS_THAN_UINT32(kBody, new_len);
}

/// total_bytes считает и текст, и вызовы — им пользуется статистика.
void test_total_bytes_includes_calls()
{
    const history::Message msgs[] = {
        msg(api::Role::Assistant, "12"),
        msg_with_calls("12345"),
    };

    TEST_ASSERT_EQUAL_UINT32(7, history::total_bytes(msgs, 2));
    TEST_ASSERT_EQUAL_UINT32(5, history::total_bytes(msgs, 2, 1));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_trim_when_fits);
    RUN_TEST(test_system_always_kept);
    RUN_TEST(test_trims_oldest_first);
    RUN_TEST(test_last_user_question_always_kept);
    RUN_TEST(test_never_starts_with_tool);
    RUN_TEST(test_orphan_tool_chain_cuts_right_side);
    RUN_TEST(test_tool_after_assistant_is_valid);
    RUN_TEST(test_empty_history);
    RUN_TEST(test_only_system_message);
    RUN_TEST(test_bytes_counted_for_kept_only);
    RUN_TEST(test_system_not_counted_in_budget);
    RUN_TEST(test_single_oversized_message_still_sent);
    RUN_TEST(test_tool_calls_counted_in_budget);
    RUN_TEST(test_tool_calls_fit_keeps_history);
    RUN_TEST(test_old_accounting_overflows_real_body);
    RUN_TEST(test_total_bytes_includes_calls);
    return UNITY_END();
}
