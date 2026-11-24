#pragma once

// Разбор адреса API вида `https://host:port/path`.
//
// Нужен, потому что HTTPClient требует хост, порт и путь по отдельности, а в
// настройках хранится один адрес: пользователь правит его руками в
// config.json и вправе написать его как угодно. Ошибка здесь выглядит как
// «не удалось подключиться» без объяснения, поэтому разбор — отдельная
// функция с тестами, а не пара strstr в сетевом коде.

#include <stddef.h>
#include <stdint.h>

namespace url {

constexpr size_t kMaxHost = 128;
constexpr size_t kMaxPath = 192;

struct Parts {
    /// false — http (без TLS), true — https.
    bool tls{true};
    char host[kMaxHost]{};
    uint16_t port{443};
    /// Путь с ведущим слэшем, без завершающего. "/api/v1" для base_url.
    char path[kMaxPath]{};
};

/// Разбирает адрес. Порт по умолчанию — 443 для https и 80 для http.
///
/// @param text адрес целиком
/// @param out  результат
/// @param err  причина отказа
/// @return false, если адрес не разобрался (нет схемы, пустой хост, порт вне
///         диапазона, слишком длинный хост или путь)
bool parse(const char* text, Parts& out, char* err, size_t err_len);

/// Дописывает сегмент к пути, не допуская двойного слэша.
///
/// @param parts адрес
/// @param segment сегмент вида "/chat/completions" или "chat/completions"
/// @return false, если не помещается
bool append_path(Parts& parts, const char* segment);

}  // namespace url
