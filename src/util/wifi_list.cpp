// Реализация списка Wi-Fi сетей. Чистый код: ни WiFi.h, ни Arduino — иначе
// тесты на хосте не собрались бы.

#include "util/wifi_list.h"

#include <string.h>

namespace wifi {
namespace {

/// Копирует строку с гарантированным нулём, не разрезая UTF-8.
///
/// Имя сети приходит от чужого устройства и может быть не по-русски: SSID —
/// произвольные байты. Резать посреди многобайтовой последовательности нельзя,
/// иначе на экране вместо иероглифа появится «квадратик». ESID по стандарту
/// и так ограничен 32 байтами, но проверять дешевле, чем отлаживать.
void copy_ssid(char* dst, size_t dst_len, const char* src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
        // Отступаем назад, пока не встанем на начало последовательности.
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) {
            --n;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/// Находит сеть с таким именем, либо -1.
int find(const NetworkList& list, const char* ssid)
{
    for (uint8_t i = 0; i < list.count; ++i) {
        if (strcmp(list.items[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

}  // namespace

void clear(NetworkList& list)
{
    list.count = 0;
    for (Network& n : list.items) {
        n = Network{};
    }
}

bool add(NetworkList& list, const char* ssid, int rssi, bool secure)
{
    // Скрытые сети отдают пустое имя: подключаться к ним нечем, а в списке
    // они выглядели бы пустыми строками.
    if (!ssid || ssid[0] == '\0') return false;

    // Уровень сигнала вне разумных пределов — мусор от драйвера.
    if (rssi > 0) return false;

    const int existing = find(list, ssid);
    if (existing >= 0) {
        // Та же сеть с другой точки доступа: оставляем сильнейшую.
        if (rssi > list.items[existing].rssi) {
            list.items[existing].rssi = static_cast<int16_t>(rssi);
            list.items[existing].secure = secure;
        }
        return true;
    }

    if (list.count >= kMaxNetworks) return false;

    Network& n = list.items[list.count];
    copy_ssid(n.ssid, sizeof(n.ssid), ssid);
    n.rssi = static_cast<int16_t>(rssi);
    n.secure = secure;
    ++list.count;
    return true;
}

void sort(NetworkList& list)
{
    // Вставками: список короткий (≤12), а он почти всегда уже упорядочен
    // драйвером — на реальных данных это дешевле обмена через memmove.
    for (uint8_t i = 1; i < list.count; ++i) {
        const Network key = list.items[i];
        int j = i - 1;
        // Строго больше: при равном уровне элемент остаётся на месте.
        while (j >= 0 && list.items[j].rssi < key.rssi) {
            list.items[j + 1] = list.items[j];
            --j;
        }
        list.items[j + 1] = key;
    }
}

uint8_t signal_level(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    if (rssi >= -88) return 1;
    return 0;
}

bool password_valid(const char* password)
{
    if (!password) return false;

    const size_t len = strlen(password);

    // «Сырой» PSK: ровно 64 шестнадцатеричных символа.
    if (len == 64) {
        for (size_t i = 0; i < len; ++i) {
            const char c = password[i];
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
        return true;
    }

    // Парольная фраза: 8..63 символа.
    if (len < 8 || len > 63) return false;

    // Непечатаемые байты в пароле почти наверняка означают ошибку ввода или
    // обрезанную строку: WPA2 допускает любые байты, но не с клавиатуры.
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(password[i]);
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

const char* security_name(bool secure)
{
    return secure ? "WPA2" : "открытая";
}

}  // namespace wifi
