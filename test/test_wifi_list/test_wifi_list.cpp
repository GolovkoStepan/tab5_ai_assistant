/*
 * Тесты списка Wi-Fi сетей и проверки пароля.
 *
 * Проверяем то, что на плате выглядит как «странно» и не воспроизводится:
 * дубликаты одной сети, пустые имена, порядок, границы уровней сигнала.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "util/wifi_list.h"

void setUp(void) {}
void tearDown(void) {}

// --- Список сетей ---

void test_add_keeps_order_and_count()
{
    wifi::NetworkList list;
    wifi::clear(list);

    TEST_ASSERT_TRUE(wifi::add(list, "Alpha", -50, true));
    TEST_ASSERT_TRUE(wifi::add(list, "Beta", -70, false));

    TEST_ASSERT_EQUAL_UINT8(2, list.count);
    TEST_ASSERT_EQUAL_STRING("Alpha", list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Beta", list[1].ssid);
    TEST_ASSERT_TRUE(list[0].secure);
    TEST_ASSERT_FALSE(list[1].secure);
}

/// Одна квартира с репитером отдаёт одну сеть несколькими записями. Без
/// схлопывания список заполнялся одинаковыми строками, и выбрать сеть было
/// нельзя: все пункты выглядели одинаково.
void test_duplicate_ssid_keeps_strongest_signal()
{
    wifi::NetworkList list;
    wifi::clear(list);

    wifi::add(list, "Home", -80, true);
    wifi::add(list, "Home", -45, true);
    wifi::add(list, "Home", -65, true);

    TEST_ASSERT_EQUAL_UINT8(1, list.count);
    TEST_ASSERT_EQUAL_INT16(-45, list[0].rssi);
}

/// Слабый дубликат не должен понижать уровень уже найденной точки доступа.
void test_weaker_duplicate_does_not_overwrite()
{
    wifi::NetworkList list;
    wifi::clear(list);

    wifi::add(list, "Home", -45, true);
    wifi::add(list, "Home", -85, true);

    TEST_ASSERT_EQUAL_INT16(-45, list[0].rssi);
}

void test_hidden_network_is_skipped()
{
    wifi::NetworkList list;
    wifi::clear(list);

    TEST_ASSERT_FALSE(wifi::add(list, "", -50, true));
    TEST_ASSERT_FALSE(wifi::add(list, nullptr, -50, true));
    TEST_ASSERT_EQUAL_UINT8(0, list.count);
}

void test_positive_rssi_is_rejected()
{
    wifi::NetworkList list;
    wifi::clear(list);

    // RSSI не бывает положительным: это мусор от драйвера.
    TEST_ASSERT_FALSE(wifi::add(list, "Bad", 10, false));
    TEST_ASSERT_EQUAL_UINT8(0, list.count);
}

void test_list_respects_max_networks()
{
    wifi::NetworkList list;
    wifi::clear(list);

    char name[8];
    for (size_t i = 0; i < wifi::kMaxNetworks; ++i) {
        snprintf(name, sizeof(name), "n%u", static_cast<unsigned>(i));
        TEST_ASSERT_TRUE(wifi::add(list, name, -50, false));
    }

    // Тринадцатая сеть не должна попасть: экран столько строк не покажет.
    TEST_ASSERT_FALSE(wifi::add(list, "overflow", -50, false));
    TEST_ASSERT_EQUAL_UINT8(wifi::kMaxNetworks, list.count);
}

void test_sort_orders_by_signal_descending()
{
    wifi::NetworkList list;
    wifi::clear(list);

    wifi::add(list, "weak", -88, true);
    wifi::add(list, "strong", -40, true);
    wifi::add(list, "middle", -60, true);
    wifi::sort(list);

    TEST_ASSERT_EQUAL_STRING("strong", list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("middle", list[1].ssid);
    TEST_ASSERT_EQUAL_STRING("weak", list[2].ssid);
}

/// При равном сигнале порядок добавления сохраняется. Иначе две одинаково
/// сильные сети менялись бы местами между сканированиями, и палец попадал
/// бы не туда.
void test_sort_is_stable_for_equal_signal()
{
    wifi::NetworkList list;
    wifi::clear(list);

    wifi::add(list, "first", -50, true);
    wifi::add(list, "second", -50, true);
    wifi::add(list, "third", -50, true);
    wifi::sort(list);

    TEST_ASSERT_EQUAL_STRING("first", list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("second", list[1].ssid);
    TEST_ASSERT_EQUAL_STRING("third", list[2].ssid);
}

void test_sort_handles_empty_and_single()
{
    wifi::NetworkList list;
    wifi::clear(list);
    wifi::sort(list);  // не должно падать
    TEST_ASSERT_EQUAL_UINT8(0, list.count);

    wifi::add(list, "only", -50, true);
    wifi::sort(list);
    TEST_ASSERT_EQUAL_STRING("only", list[0].ssid);
}

/// Длинный SSID обрезается по границе UTF-8: разрезанная последовательность
/// на экране выглядит как «квадратик» вместо последнего символа.
void test_long_utf8_ssid_is_cut_on_char_boundary()
{
    wifi::NetworkList list;
    wifi::clear(list);

    // 20 раз по «я» (2 байта) = 40 байт. В 33 байта влезает 16 символов ровно.
    char long_ssid[64] = {};
    for (int i = 0; i < 20; ++i) {
        strcat(long_ssid, "я");
    }

    TEST_ASSERT_TRUE(wifi::add(list, long_ssid, -50, false));

    const char* got = list[0].ssid;
    const size_t n = strlen(got);
    TEST_ASSERT_TRUE(n <= wifi::kMaxSsidLen - 1);

    // Каждая последовательность должна быть полной: идём по строке и на каждом
    // шаге проверяем, что символ целиком влез в буфер.
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(got[i]);
        size_t need = 1;
        if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        }
        TEST_ASSERT_TRUE_MESSAGE(i + need <= n, "символ разрезан посреди последовательности");
        i += need;
    }
}

void test_clear_empties_the_list()
{
    wifi::NetworkList list;
    wifi::clear(list);
    wifi::add(list, "Home", -50, true);
    TEST_ASSERT_EQUAL_UINT8(1, list.count);

    wifi::clear(list);
    TEST_ASSERT_EQUAL_UINT8(0, list.count);
    TEST_ASSERT_TRUE(list.empty());
    TEST_ASSERT_EQUAL_STRING("", list[0].ssid);
}

// --- Уровень сигнала ---

void test_signal_level_boundaries()
{
    TEST_ASSERT_EQUAL_UINT8(4, wifi::signal_level(-30));
    TEST_ASSERT_EQUAL_UINT8(4, wifi::signal_level(-55));
    TEST_ASSERT_EQUAL_UINT8(3, wifi::signal_level(-56));
    TEST_ASSERT_EQUAL_UINT8(3, wifi::signal_level(-67));
    TEST_ASSERT_EQUAL_UINT8(2, wifi::signal_level(-68));
    TEST_ASSERT_EQUAL_UINT8(2, wifi::signal_level(-78));
    TEST_ASSERT_EQUAL_UINT8(1, wifi::signal_level(-79));
    TEST_ASSERT_EQUAL_UINT8(1, wifi::signal_level(-88));
    TEST_ASSERT_EQUAL_UINT8(0, wifi::signal_level(-89));
    TEST_ASSERT_EQUAL_UINT8(0, wifi::signal_level(-95));
}

// --- Пароль ---

void test_password_accepts_typical_phrase()
{
    TEST_ASSERT_TRUE(wifi::password_valid("password"));
    TEST_ASSERT_TRUE(wifi::password_valid("мой пароль 123"));
    TEST_ASSERT_TRUE(wifi::password_valid("12345678"));
}

void test_password_rejects_too_short()
{
    TEST_ASSERT_FALSE(wifi::password_valid(""));
    TEST_ASSERT_FALSE(wifi::password_valid("1234567"));
    TEST_ASSERT_FALSE(wifi::password_valid(nullptr));
}

void test_password_rejects_too_long()
{
    // 64 символа — это уже PSK, а не фраза; но не hex, значит невалидно.
    char phrase[70] = {};
    for (int i = 0; i < 64; ++i) {
        phrase[i] = 'z';
    }
    TEST_ASSERT_FALSE(wifi::password_valid(phrase));
}

/// Сырой PSK — ровно 64 шестнадцатеричных символа. Это единственный случай
/// длиной 64, который WPA2 принимает.
void test_password_accepts_raw_psk()
{
    const char* psk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    TEST_ASSERT_TRUE(wifi::password_valid(psk));

    // 64 символа, но не hex.
    const char* not_psk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg";
    TEST_ASSERT_FALSE(wifi::password_valid(not_psk));
}

/// Управляющие байты означают обрезанную или испорченную строку, а не пароль.
void test_password_rejects_control_chars()
{
    TEST_ASSERT_FALSE(wifi::password_valid("pass\tword1"));
    TEST_ASSERT_FALSE(wifi::password_valid("pass\nword1"));
}

void test_security_name()
{
    TEST_ASSERT_EQUAL_STRING("WPA2", wifi::security_name(true));
    TEST_ASSERT_EQUAL_STRING("открытая", wifi::security_name(false));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_add_keeps_order_and_count);
    RUN_TEST(test_duplicate_ssid_keeps_strongest_signal);
    RUN_TEST(test_weaker_duplicate_does_not_overwrite);
    RUN_TEST(test_hidden_network_is_skipped);
    RUN_TEST(test_positive_rssi_is_rejected);
    RUN_TEST(test_list_respects_max_networks);
    RUN_TEST(test_sort_orders_by_signal_descending);
    RUN_TEST(test_sort_is_stable_for_equal_signal);
    RUN_TEST(test_sort_handles_empty_and_single);
    RUN_TEST(test_long_utf8_ssid_is_cut_on_char_boundary);
    RUN_TEST(test_clear_empties_the_list);

    RUN_TEST(test_signal_level_boundaries);

    RUN_TEST(test_password_accepts_typical_phrase);
    RUN_TEST(test_password_rejects_too_short);
    RUN_TEST(test_password_rejects_too_long);
    RUN_TEST(test_password_accepts_raw_psk);
    RUN_TEST(test_password_rejects_control_chars);
    RUN_TEST(test_security_name);

    return UNITY_END();
}
