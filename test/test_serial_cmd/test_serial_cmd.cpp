#include <unity.h>

#include <string.h>

#include "util/serial_cmd.h"

namespace {

/// Разобрать и вернуть текст вопроса (или пустую строку).
void parse_text(const char* line, char* out, size_t out_len, serialcmd::Kind expected)
{
    const char* text = nullptr;
    size_t len = 0;
    const serialcmd::Kind kind = serialcmd::parse(line, strlen(line), &text, &len);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(kind));
    if (expected == serialcmd::Kind::Ask) {
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_LESS_THAN(out_len, len + 1);
        memcpy(out, text, len);
        out[len] = '\0';
    }
}

}  // namespace

/// Основная форма: `ask <вопрос>`.
void test_ask_basic()
{
    char out[64];
    parse_text("ask Как сбросить фильтр?", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("Как сбросить фильтр?", out);
}

/// Регистр команды не важен, а вопрос — важен: «ASK» не должен портить текст.
void test_ask_case_insensitive_command()
{
    char out[64];
    parse_text("ASK Привет", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("Привет", out);

    parse_text("AsK Привет", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("Привет", out);
}

/// Короткая форма `> вопрос`.
void test_short_form()
{
    char out[64];
    parse_text("> 2 + 2?", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("2 + 2?", out);
}

/// Пробелы между командой и текстом не входят в вопрос.
void test_leading_spaces_trimmed()
{
    char out[64];
    parse_text("ask     много пробелов", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("много пробелов", out);
}

/// Пробелы ВНУТРИ вопроса сохраняются: модель видит ровно то, что написали.
void test_inner_spaces_kept()
{
    char out[64];
    parse_text("ask 2  +  2", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("2  +  2", out);
}

/// Хвостовой `\r` от терминала не должен попадать в вопрос.
void test_trailing_cr_trimmed()
{
    char out[64];
    parse_text("ask Вопрос\r", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("Вопрос", out);

    parse_text("ask Вопрос \t ", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("Вопрос", out);
}

/// Пустая строка и строка из одних пробелов — молчание, а не команда.
void test_empty_is_none()
{
    char out[8];
    parse_text("", out, sizeof(out), serialcmd::Kind::None);
    parse_text("   ", out, sizeof(out), serialcmd::Kind::None);
    parse_text("\r\n", out, sizeof(out), serialcmd::Kind::None);
}

/// Голый текст НЕ считается вопросом: Serial — это ещё и лог, и случайная
/// строка не должна улетать в модель за деньги.
void test_bare_text_is_unknown()
{
    char out[8];
    parse_text("Как сбросить фильтр?", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("boot ok", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// «ask» без текста — подсказка, а не пустой запрос к модели.
void test_ask_without_text_is_help()
{
    char out[8];
    parse_text("ask", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("ask    ", out, sizeof(out), serialcmd::Kind::Help);
    parse_text(">", out, sizeof(out), serialcmd::Kind::Help);
    parse_text(">   ", out, sizeof(out), serialcmd::Kind::Help);
}

/// Подсказка по команде `help` и `?`.
void test_help()
{
    char out[8];
    parse_text("help", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("HELP", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("?", out, sizeof(out), serialcmd::Kind::Help);
}

/// Слово, начинающееся с «help», но не «help» — неизвестная команда.
void test_help_prefix_is_not_help()
{
    char out[8];
    parse_text("helper", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("helpful thing", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// Команда `thread` — печать треда, бесплатная диагностика.
void test_thread()
{
    char out[8];
    parse_text("thread", out, sizeof(out), serialcmd::Kind::Thread);
    parse_text("THREAD", out, sizeof(out), serialcmd::Kind::Thread);
    // Префикс — уже не команда.
    parse_text("threads", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// `wifi off` / `wifi on` — выключатель сети для проверки «без интернета».
void test_wifi_switch()
{
    char out[8];
    parse_text("wifi off", out, sizeof(out), serialcmd::Kind::WifiOff);
    parse_text("WIFI OFF", out, sizeof(out), serialcmd::Kind::WifiOff);
    parse_text("  wifi   off  ", out, sizeof(out), serialcmd::Kind::WifiOff);
    parse_text("wifi on", out, sizeof(out), serialcmd::Kind::WifiOn);
    parse_text("WiFi On", out, sizeof(out), serialcmd::Kind::WifiOn);
}

/// `wifi` без понятного аргумента — подсказка, а не догадка.
///
/// Молчаливое «наверное, имели в виду off» опаснее: человек решил бы, что
/// сеть выключена, и не понял бы, почему плата продолжает ходить в API.
void test_wifi_without_argument_is_help()
{
    char out[8];
    parse_text("wifi", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("wifi   ", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("wifi off please", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("wifi sideways", out, sizeof(out), serialcmd::Kind::Help);
}

/// Слово, начинающееся с «wifi», но не команда.
void test_wifi_prefix_is_not_command()
{
    char out[8];
    parse_text("wifioff", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("wifi_status", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// Подсказка знает про новые команды: иначе о них не узнать с устройства.
void test_help_mentions_wifi()
{
    const char* text = serialcmd::help_text();
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_NOT_NULL(strstr(text, "wifi off"));
    TEST_ASSERT_NOT_NULL(strstr(text, "wifi on"));
    TEST_ASSERT_NOT_NULL(strstr(text, "ask"));
    TEST_ASSERT_NOT_NULL(strstr(text, "thread"));
    TEST_ASSERT_NOT_NULL(strstr(text, "api"));
}

/// `api <адрес>` — подмена адреса API для проверки на фальшивом сервере.
void test_api_address()
{
    auto parse_api = [](const char* line, char* out, size_t out_len) {
        const char* text = nullptr;
        size_t len = 0;
        const serialcmd::Kind kind = serialcmd::parse(line, strlen(line), &text, &len);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(serialcmd::Kind::Api), static_cast<int>(kind));
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_LESS_THAN(out_len, len + 1);
        memcpy(out, text, len);
        out[len] = '\0';
    };

    char out[128];
    parse_api("api http://192.168.1.141:8765/api/v1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.141:8765/api/v1", out);

    // Регистр команды не важен, адрес — важен.
    parse_api("API HTTP://host/api/v1", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("HTTP://host/api/v1", out);
}

/// `api` без адреса — подсказка: молчаливое «понял» оставило бы старый адрес.
void test_api_without_address_is_help()
{
    char out[8];
    parse_text("api", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("api   ", out, sizeof(out), serialcmd::Kind::Help);
}

/// Слово, начинающееся с «api», но не команда.
void test_api_prefix_is_not_api()
{
    char out[8];
    parse_text("api_key", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("apikey", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// `tz <зона>` — смена таймзоны рядом с `api`: обе правят config.json.
void test_tz_zone()
{
    auto parse_tz = [](const char* line, char* out, size_t out_len) {
        const char* text = nullptr;
        size_t text_len = 0;
        const serialcmd::Kind kind = serialcmd::parse(line, strlen(line), &text, &text_len);
        TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Kind::Tz), static_cast<int>(kind));
        const size_t n = text_len < out_len - 1 ? text_len : out_len - 1;
        memcpy(out, text, n);
        out[n] = '\0';
    };

    char out[32];
    parse_tz("tz YEKT-5", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("YEKT-5", out);

    // Регистр команды — любой, значение сохраняется как написано.
    parse_tz("TZ MSK-3", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("MSK-3", out);

    // Дробное смещение: в Индии и Непале пояс не кратен часу.
    parse_tz("tz IST-5:30", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("IST-5:30", out);
}

/// `tz` без аргумента — подсказка: молчаливое «понял» оставило бы старый пояс.
void test_tz_without_argument_is_help()
{
    char out[8];
    parse_text("tz", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("tz   ", out, sizeof(out), serialcmd::Kind::Help);
}

/// Слово, начинающееся с «tz», но не команда (`tzdata`, `tzset`).
void test_tz_prefix_is_not_tz()
{
    char out[8];
    parse_text("tzdata", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("tzset", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// Подсказка должна упоминать `tz`: иначе о команде никто не узнает.
void test_help_mentions_tz()
{
    const char* text = serialcmd::help_text();
    TEST_ASSERT_NOT_NULL(strstr(text, "tz"));
}

/// Слово, начинающееся с «ask», но без пробела — не команда.
void test_ask_prefix_is_not_ask()
{
    char out[8];
    parse_text("asked", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("asker question", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// nullptr и нулевая длина не должны ничего ломать.
void test_null_line()
{
    const char* text = nullptr;
    size_t len = 99;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(serialcmd::Kind::None), static_cast<int>(serialcmd::parse(nullptr, 0, &text, &len)));
    TEST_ASSERT_NULL(text);
    TEST_ASSERT_EQUAL_UINT(0, len);
}

/// Вопрос целиком на кириллице: длина считается в байтах, ничего не теряется.
void test_cyrillic_whole_question()
{
    char out[128];
    parse_text("ask Почему очиститель просит заменить фильтр?", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("Почему очиститель просит заменить фильтр?", out);
}

/// Однобуквенный вопрос — не подсказка и не пустая команда.
void test_single_char_question()
{
    char out[8];
    parse_text("ask 1", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("1", out);

    parse_text("> я", out, sizeof(out), serialcmd::Kind::Ask);
    TEST_ASSERT_EQUAL_STRING("я", out);
}

/// `screen <имя>` — открыть экран без клавиатуры, тем же путём, что Ctrl+….
void test_screen_names()
{
    auto parse_screen_cmd = [](const char* line) {
        const char* text = nullptr;
        size_t text_len = 0;
        const serialcmd::Kind kind = serialcmd::parse(line, strlen(line), &text, &text_len);
        if (kind != serialcmd::Kind::Screen) return serialcmd::Screen::None;
        return serialcmd::parse_screen(text, text_len);
    };

    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::Chat), static_cast<int>(parse_screen_cmd("screen chat")));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::About), static_cast<int>(parse_screen_cmd("screen about")));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::Help), static_cast<int>(parse_screen_cmd("screen help")));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::Notes), static_cast<int>(parse_screen_cmd("screen notes")));

    // Регистр имени не важен.
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::Diag), static_cast<int>(parse_screen_cmd("screen DIAG")));

    // Лишние пробелы не мешают: имя разбирается как слово.
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::Wifi), static_cast<int>(parse_screen_cmd("screen   wifi")));
}

/// `screen` без имени и с незнакомым именем — подсказка, а не молчаливый
/// переход в чат: человек должен узнать, какие имена есть.
void test_screen_without_name_is_help()
{
    char out[8];
    parse_text("screen", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("screen   ", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("screen chatx", out, sizeof(out), serialcmd::Kind::Help);
    parse_text("screen 42", out, sizeof(out), serialcmd::Kind::Help);
}

/// Слово, начинающееся с «screen», но не команда.
void test_screen_prefix_is_not_screen()
{
    char out[8];
    parse_text("screens", out, sizeof(out), serialcmd::Kind::Unknown);
    parse_text("screenplay", out, sizeof(out), serialcmd::Kind::Unknown);
}

/// Подсказка должна упоминать `screen`: иначе о команде никто не узнает.
void test_help_mentions_screen()
{
    const char* text = serialcmd::help_text();
    TEST_ASSERT_NOT_NULL(strstr(text, "screen"));
}

/// Имя без строки `screen` — не команда: `parse_screen` вызывают только после
/// проверки вида строки, а сам по себе он на мусор отвечает None.
void test_parse_screen_rejects_garbage()
{
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::None), static_cast<int>(serialcmd::parse_screen(nullptr, 0)));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::None), static_cast<int>(serialcmd::parse_screen("", 0)));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Screen::None), static_cast<int>(serialcmd::parse_screen("chatx", 5)));
}

/// `new` — новый тред без клавиатуры. Тред упирается в предел 200 сообщений,
/// и выйти из этого состояния можно только Ctrl+N; команда делает то же самое,
/// чтобы приёмочный прогон не останавливался.
void test_parse_new_thread()
{
    auto kind_of = [](const char* line) { return serialcmd::parse(line, strlen(line), nullptr, nullptr); };

    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Kind::NewThread), static_cast<int>(kind_of("new")));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Kind::NewThread), static_cast<int>(kind_of("NEW")));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Kind::NewThread), static_cast<int>(kind_of("  new  ")));

    // Не команда: «new» — начало другого слова, а не слово целиком.
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Kind::Unknown), static_cast<int>(kind_of("newthread")));
    TEST_ASSERT_EQUAL(static_cast<int>(serialcmd::Kind::Unknown), static_cast<int>(kind_of("newdoc")));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_ask_basic);
    RUN_TEST(test_ask_case_insensitive_command);
    RUN_TEST(test_short_form);
    RUN_TEST(test_leading_spaces_trimmed);
    RUN_TEST(test_inner_spaces_kept);
    RUN_TEST(test_trailing_cr_trimmed);
    RUN_TEST(test_empty_is_none);
    RUN_TEST(test_bare_text_is_unknown);
    RUN_TEST(test_ask_without_text_is_help);
    RUN_TEST(test_help);
    RUN_TEST(test_help_prefix_is_not_help);
    RUN_TEST(test_thread);
    RUN_TEST(test_wifi_switch);
    RUN_TEST(test_wifi_without_argument_is_help);
    RUN_TEST(test_wifi_prefix_is_not_command);
    RUN_TEST(test_help_mentions_wifi);
    RUN_TEST(test_api_address);
    RUN_TEST(test_api_without_address_is_help);
    RUN_TEST(test_api_prefix_is_not_api);
    RUN_TEST(test_tz_zone);
    RUN_TEST(test_tz_without_argument_is_help);
    RUN_TEST(test_tz_prefix_is_not_tz);
    RUN_TEST(test_help_mentions_tz);
    RUN_TEST(test_screen_names);
    RUN_TEST(test_screen_without_name_is_help);
    RUN_TEST(test_screen_prefix_is_not_screen);
    RUN_TEST(test_help_mentions_screen);
    RUN_TEST(test_parse_screen_rejects_garbage);
    RUN_TEST(test_ask_prefix_is_not_ask);
    RUN_TEST(test_null_line);
    RUN_TEST(test_cyrillic_whole_question);
    RUN_TEST(test_single_char_question);
    RUN_TEST(test_parse_new_thread);
    return UNITY_END();
}
