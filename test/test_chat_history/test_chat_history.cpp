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
    return UNITY_END();
}
