// Проверка ключа API. Без Arduino — тестируется на хосте.

#include "util/api_key.h"

#include <stdio.h>
#include <string.h>

namespace apikey {

size_t clean(char* key)
{
    if (key == nullptr) return 0;

    size_t len = strlen(key);

    // Хвост: пробелы, переводы строк, обрамляющие кавычки.
    while (len > 0) {
        const char c = key[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') {
            key[--len] = '\0';
        } else {
            break;
        }
    }

    // Начало — то же самое. Сдвигаем строку влево через memmove.
    size_t start = 0;
    while (start < len) {
        const char c = key[start];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') {
            ++start;
        } else {
            break;
        }
    }

    if (start > 0) {
        memmove(key, key + start, len - start + 1);
        len -= start;
    }
    return len;
}

bool validate(const char* key, size_t max_len, char* err, size_t err_len)
{
    if (err != nullptr && err_len > 0) err[0] = '\0';

    if (key == nullptr || key[0] == '\0') {
        if (err && err_len > 0) snprintf(err, err_len, "Ключ пустой");
        return false;
    }

    size_t len = 0;
    for (const char* p = key; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);

        // Заголовки HTTP — latin-1: всё выше 0x7E уже не отправить.
        if (c < 0x21 || c > 0x7E) {
            if (c >= 0x80) {
                if (err && err_len > 0) snprintf(err, err_len, "Ключ содержит не-ASCII символы");
            } else {
                if (err && err_len > 0) snprintf(err, err_len, "Ключ содержит пробел или управляющий символ");
            }
            return false;
        }

        if (++len > max_len) {
            if (err && err_len > 0) snprintf(err, err_len, "Ключ длиннее %u символов", (unsigned)max_len);
            return false;
        }
    }

    if (len < kMinLen) {
        if (err && err_len > 0) snprintf(err, err_len, "Ключ слишком короткий (нужно хотя бы %u)", (unsigned)kMinLen);
        return false;
    }

    return true;
}

}  // namespace apikey
