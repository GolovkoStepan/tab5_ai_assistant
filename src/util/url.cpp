// Разбор адреса API. Чистые функции — без Arduino и сокетов.
//
// HTTPClient требует хост, порт и путь по отдельности, а в config.json лежит
// один адрес, который пользователь правит руками. Каждый неучтённый случай
// (завершающий слэш, порт после хоста, адрес без схемы) выглядит на плате как
// «не удалось подключиться» без объяснения — поэтому разбор отдельный и с
// тестами.

#include "util/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace url {
namespace {

void set_err(char* err, size_t err_len, const char* text)
{
    if (err && err_len > 0) snprintf(err, err_len, "%s", text);
}

/// Копирует счётчик символов строки, требуя, чтобы она поместилась целиком.
bool copy_exact(char* dst, size_t dst_len, const char* src, size_t src_len)
{
    if (src_len + 1 > dst_len) return false;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return true;
}

}  // namespace

bool parse(const char* text, Parts& out, char* err, size_t err_len)
{
    out = Parts{};
    if (err && err_len > 0) err[0] = '\0';

    if (text == nullptr || text[0] == '\0') {
        set_err(err, err_len, "пустой адрес");
        return false;
    }

    const char* rest = nullptr;
    if (strncmp(text, "https://", 8) == 0) {
        out.tls = true;
        out.port = 443;
        rest = text + 8;
    } else if (strncmp(text, "http://", 7) == 0) {
        out.tls = false;
        out.port = 80;
        rest = text + 7;
    } else {
        set_err(err, err_len, "адрес должен начинаться с http:// или https://");
        return false;
    }

    // Хост заканчивается на '/' (начало пути), ':' (порт), '?' (запрос) или
    // конце строки. '?' тоже стоп: иначе он попадёт в имя хоста, и сервер
    // ответит невнятной ошибкой DNS вместо понятного «это не базовый адрес».
    const char* host_end = rest;
    while (*host_end != '\0' && *host_end != '/' && *host_end != ':' && *host_end != '?')
        ++host_end;

    const size_t host_len = static_cast<size_t>(host_end - rest);
    if (host_len == 0) {
        set_err(err, err_len, "в адресе нет имени хоста");
        return false;
    }
    if (!copy_exact(out.host, sizeof(out.host), rest, host_len)) {
        set_err(err, err_len, "слишком длинное имя хоста");
        return false;
    }

    const char* after_host = host_end;

    // Порт.
    if (*after_host == ':') {
        ++after_host;
        char* end = nullptr;
        const long port = strtol(after_host, &end, 10);
        if (end == after_host) {
            set_err(err, err_len, "после двоеточия ожидался номер порта");
            return false;
        }
        if (port <= 0 || port > 65535) {
            set_err(err, err_len, "порт вне диапазона 1..65535");
            return false;
        }
        out.port = static_cast<uint16_t>(port);
        after_host = end;
    }

    // Путь: всё остальное, без завершающего слэша. `https://h/` и `https://h`
    // должны давать одинаковый результат — иначе склейка с эндпоинтом даст
    // двойной слэш, и сервер вернёт 404.
    if (*after_host == '/') {
        size_t path_len = strlen(after_host);
        while (path_len > 1 && after_host[path_len - 1] == '/')
            --path_len;

        if (path_len == 1) {
            out.path[0] = '\0';  // ровно "/" — пустой путь
        } else if (!copy_exact(out.path, sizeof(out.path), after_host, path_len)) {
            set_err(err, err_len, "слишком длинный путь");
            return false;
        }
    } else if (*after_host != '\0') {
        // Что-то вроде "https://host?query" — для base_url это ошибка ввода.
        set_err(err, err_len, "неожиданные символы после имени хоста");
        return false;
    }

    return true;
}

bool append_path(Parts& parts, const char* segment)
{
    if (segment == nullptr) return true;

    // Ведущий слэш у сегмента необязателен, но двойного быть не должно.
    while (*segment == '/')
        ++segment;

    const size_t path_len = strlen(parts.path);
    const size_t seg_len = strlen(segment);
    if (path_len + 1 + seg_len + 1 > sizeof(parts.path)) return false;

    parts.path[path_len] = '/';
    memcpy(parts.path + path_len + 1, segment, seg_len);
    parts.path[path_len + 1 + seg_len] = '\0';
    return true;
}

}  // namespace url
