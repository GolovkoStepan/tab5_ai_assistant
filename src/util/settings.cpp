// Настройки: чистые функции — валидация, разбор и запись JSON.
// Без Arduino, чтобы тестировалось на хосте без платы.
//
// Свой минимальный разборщик JSON, а не ArduinoJson, по трём причинам:
//   - нужен на этапе v0.1.0, до подключения сети, и не тянет зависимость;
//   - тестируется на хосте, значит не может зависеть от Arduino;
//   - конфиг — плоский объект известной формы, полноценный парсер избыточен.
// Если формат усложнится, это первое место, где стоит взять библиотеку.

#include "util/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace settings {
namespace {

constexpr size_t kNoBytes = 0;

/// Копирует строку, гарантируя завершающий ноль. Обрезает при нехватке места,
/// причём по границе UTF-8: разрезанная посередине кириллица дала бы битый
/// символ, который потом уехал бы обратно в файл при сохранении.
void copy_str(char* dst, size_t dst_len, const char* src, size_t src_len)
{
    if (dst_len == 0) return;

    size_t n = src_len < dst_len - 1 ? src_len : dst_len - 1;

    // Отступаем назад, пока не окажемся на стартовом байте символа
    // (не на продолжении 10xxxxxx).
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80)
        --n;

    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

/// Пропускает пробелы (в JSON это пробел, таб, CR, LF).
const char* skip_ws(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    return p;
}

/// Разбирает строковый литерал JSON. p указывает на открывающую кавычку.
/// Возвращает указатель за закрывающей кавычкой либо nullptr при ошибке.
///
/// Если значение не влезает в буфер, оно обрезается по границе UTF-8: строка
/// из буфера потом сериализуется обратно в файл, и половинка кириллического
/// символа превратилась бы в мусор, который уже не прочитать.
const char* parse_string(const char* p, char* out, size_t out_len)
{
    if (*p != '"') return nullptr;
    if (out_len == 0) return nullptr;
    ++p;

    size_t written = 0;
    bool full = false;  // буфер полон: дочитываем строку, но не пишем
    while (*p != '\0') {
        if (*p == '"') {
            out[written] = '\0';
            return p + 1;
        }

        char ch = *p;
        if (ch == '\\') {
            ++p;
            switch (*p) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
                // \uXXXX: полный разбор вне задачи конфига. Пропускаем
                // четыре шестнадцатеричные цифры, пишем заглушку.
                for (int i = 0; i < 4; ++i) {
                    ++p;
                    const char c = *p;
                    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                    if (!hex) return nullptr;
                }
                ch = '?';
                break;
            default: return nullptr;
            }
        }

        // Место проверяем на стартовом байте символа и сразу резервируем его
        // целиком: байты-продолжения (10xxxxxx) допишутся без проверки.
        const unsigned char uc = static_cast<unsigned char>(ch);
        size_t char_len = 0;
        if ((uc & 0x80) == 0x00) {
            char_len = 1;
        } else if ((uc & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((uc & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((uc & 0xF8) == 0xF0) {
            char_len = 4;
        }

        if (!full && char_len > 0) {
            if (written + char_len >= out_len) {
                full = true;  // обрезаем по границе символа, дальше только дочитываем
            } else {
                out[written++] = ch;
            }
        } else if (!full && (uc & 0xC0) == 0x80) {
            // Продолжение символа, место зарезервировано стартовым байтом.
            out[written++] = ch;
        } else if (!full) {
            // Некорректный байт UTF-8 (например, 0xFF): заменяем на '?'.
            if (written + 1 >= out_len) {
                full = true;
            } else {
                out[written++] = '?';
            }
        }

        ++p;
    }
    return nullptr;  // строка не закрыта
}

/// Разбирает число. Возвращает указатель за числом либо nullptr.
const char* parse_number(const char* p, double* out)
{
    const char* start = p;
    if (*p == '-') ++p;
    if (*p < '0' || *p > '9') return nullptr;
    while (*p >= '0' && *p <= '9')
        ++p;
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return nullptr;
        while (*p >= '0' && *p <= '9')
            ++p;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (*p < '0' || *p > '9') return nullptr;
        while (*p >= '0' && *p <= '9')
            ++p;
    }

    char buf[32];
    const size_t n = static_cast<size_t>(p - start);
    if (n == 0 || n >= sizeof(buf)) return nullptr;
    memcpy(buf, start, n);
    buf[n] = '\0';
    *out = strtod(buf, nullptr);
    return p;
}

/// Пропускает значение любого типа (для неизвестных ключей).
const char* skip_value(const char* p);

const char* skip_container(const char* p, char open, char close)
{
    if (*p != open) return nullptr;
    int depth = 0;
    while (*p != '\0') {
        if (*p == '"') {
            char tmp[2];
            p = parse_string(p, tmp, sizeof(tmp));
            if (!p) return nullptr;
            continue;
        }
        if (*p == open) ++depth;
        if (*p == close) {
            --depth;
            if (depth == 0) return p + 1;
        }
        ++p;
    }
    return nullptr;
}

const char* skip_value(const char* p)
{
    p = skip_ws(p);
    switch (*p) {
    case '"': {
        char tmp[2];
        return parse_string(p, tmp, sizeof(tmp));
    }
    case '{': return skip_container(p, '{', '}');
    case '[': return skip_container(p, '[', ']');
    case 't':
        if (strncmp(p, "true", 4) == 0) return p + 4;
        return nullptr;
    case 'f':
        if (strncmp(p, "false", 5) == 0) return p + 5;
        return nullptr;
    case 'n':
        if (strncmp(p, "null", 4) == 0) return p + 4;
        return nullptr;
    default: {
        double d = 0;
        return parse_number(p, &d);
    }
    }
}

/// Разбирает bool. Возвращает false, если значение не true/false.
bool parse_bool(const char* p, bool* out, const char** end)
{
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        *end = p + 4;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        *end = p + 5;
        return true;
    }
    return false;
}

/// Секции конфига. Формат файла документирован в ARCHITECTURE.md, 5.3:
///
/// ```json
/// {"api":{"base_url":…,"model":…,"temperature":…,"reasoning_effort":…},
///  "search":{"enabled":…,"max_results":…,"engine":…},
///  "ui":{"brightness":…,"layout":…,"timeout_min":…},
///  "power":{"warn":…,"critical":…,"shutdown":…},
///  "time":{"tz":…}}
/// ```
///
/// Плоские ключи (`"api_base_url"`, `"brightness"`) тоже принимаются: так проще
/// править файл руками, а старый конфиг не ломает новую прошивку.
/// Разбор одного места, разные имена — цена пары строк в таблице.
enum class Section { None, Api, Search, Ui, Power, Time };

Section section_by_name(const char* name)
{
    if (strcmp(name, "api") == 0) return Section::Api;
    if (strcmp(name, "search") == 0) return Section::Search;
    if (strcmp(name, "ui") == 0) return Section::Ui;
    if (strcmp(name, "power") == 0) return Section::Power;
    if (strcmp(name, "time") == 0) return Section::Time;
    return Section::None;
}

/// Переводит «секция + ключ» в плоское имя поля.
/// nullptr — ключ в этой секции неизвестен (пропускается, не ошибка).
const char* map_section_key(Section s, const char* key)
{
    struct Mapping {
        Section section;
        const char* key;
        const char* flat;
    };
    static const Mapping kMap[] = {
        {Section::Api, "base_url", "api_base_url"},
        {Section::Api, "model", "api_model"},
        {Section::Api, "temperature", "temperature"},
        {Section::Api, "reasoning_effort", "reasoning_effort"},
        {Section::Search, "enabled", "search_enabled"},
        {Section::Search, "max_results", "search_max_results"},
        {Section::Ui, "brightness", "brightness"},
        {Section::Ui, "layout", "layout"},
        {Section::Ui, "timeout_min", "screen_timeout_min"},
        {Section::Power, "warn", "battery_warn_percent"},
        {Section::Power, "critical", "battery_critical_percent"},
        {Section::Power, "shutdown", "battery_shutdown_percent"},
        {Section::Time, "tz", "timezone"},
    };

    for (const auto& m : kMap) {
        if (m.section == s && strcmp(m.key, key) == 0) return m.flat;
    }
    return nullptr;
}

/// Применяет пару «ключ: значение» к настройкам. Неизвестные ключи пропускает.
/// Ошибкой считается только структурная поломка JSON, а не чужой ключ.
bool apply_pair(const char* key, const char* p, Config& out, const char** end)
{
    *end = p;
    double num = 0;
    bool flag = false;
    const char* after = nullptr;

    if (strcmp(key, "api_base_url") == 0) {
        after = parse_string(p, out.api_base_url, sizeof(out.api_base_url));
    } else if (strcmp(key, "api_model") == 0) {
        after = parse_string(p, out.api_model, sizeof(out.api_model));
    } else if (strcmp(key, "timezone") == 0) {
        after = parse_string(p, out.timezone, sizeof(out.timezone));
    } else if (strcmp(key, "temperature") == 0) {
        after = parse_number(p, &num);
        if (after) {
            const double t = num * 10.0;
            if (t >= 0.0 && t <= 20.0) out.temperature_tenths = static_cast<uint8_t>(t + 0.5);
        }
    } else if (strcmp(key, "reasoning_effort") == 0) {
        // Допустимо и число (0..3), и строка ("low"), как в API RouterAI.
        if (*p == '"') {
            char tmp[16];
            after = parse_string(p, tmp, sizeof(tmp));
            if (after) {
                if (strcmp(tmp, "none") == 0)
                    out.reasoning_effort = 0;
                else if (strcmp(tmp, "low") == 0)
                    out.reasoning_effort = 1;
                else if (strcmp(tmp, "medium") == 0)
                    out.reasoning_effort = 2;
                else if (strcmp(tmp, "high") == 0)
                    out.reasoning_effort = 3;
            }
        } else {
            after = parse_number(p, &num);
            if (after && num >= 0 && num <= 3) out.reasoning_effort = static_cast<uint8_t>(num);
        }
    } else if (strcmp(key, "search_enabled") == 0) {
        if (parse_bool(p, &flag, &after)) out.search_enabled = flag;
    } else if (strcmp(key, "search_max_results") == 0) {
        after = parse_number(p, &num);
        if (after && num >= 0 && num <= 10) out.search_max_results = static_cast<uint8_t>(num);
    } else if (strcmp(key, "brightness") == 0) {
        after = parse_number(p, &num);
        if (after && num >= 0 && num <= 255) out.brightness = static_cast<uint8_t>(num);
    } else if (strcmp(key, "layout") == 0) {
        char tmp[8];
        after = parse_string(p, tmp, sizeof(tmp));
        if (after) out.layout_russian = (strcmp(tmp, "ru") == 0);
    } else if (strcmp(key, "screen_timeout_min") == 0) {
        after = parse_number(p, &num);
        if (after && num >= 0 && num <= 60) out.screen_timeout_min = static_cast<uint8_t>(num);
    } else if (strcmp(key, "battery_warn_percent") == 0) {
        after = parse_number(p, &num);
        if (after && num >= 0 && num <= 100) out.battery_warn_percent = static_cast<uint8_t>(num);
    } else if (strcmp(key, "battery_critical_percent") == 0) {
        after = parse_number(p, &num);
        if (after && num >= 0 && num <= 100) out.battery_critical_percent = static_cast<uint8_t>(num);
    } else if (strcmp(key, "battery_shutdown_percent") == 0) {
        after = parse_number(p, &num);
        if (after && num >= 0 && num <= 100) out.battery_shutdown_percent = static_cast<uint8_t>(num);
    } else {
        after = skip_value(p);
    }

    if (!after) return false;
    *end = after;
    return true;
}

/// Разбирает вложенный объект секции: `{"enabled": true, …}`.
/// p указывает на открывающую скобку, *end — за закрывающей.
bool parse_section(Section section, const char* p, Config& out, const char** end)
{
    if (*p != '{') return false;
    ++p;

    p = skip_ws(p);
    if (*p == '}') {
        *end = p + 1;
        return true;
    }

    while (*p != '\0') {
        char key[40];
        p = parse_string(p, key, sizeof(key));
        if (!p) return false;

        p = skip_ws(p);
        if (*p != ':') return false;
        p = skip_ws(p + 1);

        const char* after = nullptr;
        const char* flat = map_section_key(section, key);
        if (flat == nullptr) {
            // Ключ чужой для этой секции (например, "engine" в search):
            // пропускаем значение целиком.
            after = skip_value(p);
            if (!after) return false;
        } else if (!apply_pair(flat, p, out, &after)) {
            return false;
        }

        p = skip_ws(after);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            *end = p + 1;
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace

void make_default_config(Config& out)
{
    out = Config{};
    copy_str(out.api_base_url, sizeof(out.api_base_url), defaults::kApiBaseUrl, strlen(defaults::kApiBaseUrl));
    copy_str(out.api_model, sizeof(out.api_model), defaults::kApiModel, strlen(defaults::kApiModel));
    copy_str(out.timezone, sizeof(out.timezone), defaults::kTimezone, strlen(defaults::kTimezone));
    out.temperature_tenths = 3;
    out.reasoning_effort = 1;
    out.search_enabled = true;
    out.search_max_results = 3;
    out.brightness = 128;
    out.layout_russian = false;
    out.screen_timeout_min = 2;
    out.battery_warn_percent = 20;
    out.battery_critical_percent = 10;
    out.battery_shutdown_percent = 5;
}

bool validate(const Config& cfg, char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';

    auto fail = [&](const char* msg) {
        if (err && err_len > 0) {
            snprintf(err, err_len, "%s", msg);
        }
        return false;
    };

    if (cfg.api_base_url[0] == '\0') return fail("пустой адрес API");

    // Адрес должен быть http(s) и с хостом: иначе запрос упадёт уже в сети,
    // где причина будет выглядеть загадочно.
    const bool has_scheme =
        strncmp(cfg.api_base_url, "http://", 7) == 0 || strncmp(cfg.api_base_url, "https://", 8) == 0;
    if (!has_scheme) return fail("адрес API должен начинаться с http:// или https://");

    const char* host = strstr(cfg.api_base_url, "://");
    host = host ? host + 3 : cfg.api_base_url;
    if (*host == '\0' || *host == '/') return fail("в адресе API нет имени хоста");

    if (cfg.api_model[0] == '\0') return fail("не задана модель");
    if (cfg.temperature_tenths > 20) return fail("температура вне диапазона 0.0..2.0");
    if (cfg.reasoning_effort > 3) return fail("reasoning_effort вне диапазона 0..3");
    if (cfg.search_max_results > 10) return fail("search_max_results вне диапазона 0..10");
    if (cfg.screen_timeout_min > 60) return fail("timeout_min вне диапазона 0..60");
    if (cfg.timezone[0] == '\0') return fail("не задана таймзона");

    // Пороги разряда должны идти по убыванию: иначе «критично» наступит раньше
    // «предупреждения», и предупреждение не покажется никогда.
    const bool warn_ok = cfg.battery_warn_percent > cfg.battery_critical_percent &&
                         cfg.battery_critical_percent > cfg.battery_shutdown_percent;
    if (!warn_ok) return fail("пороги разряда должны убывать: warn > critical > shutdown");
    return true;
}

bool parse_config_json(const char* json, Config& out, char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (json == nullptr) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "пустой конфиг");
        return false;
    }

    const char* p = skip_ws(json);
    if (*p != '{') {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "конфиг должен быть объектом JSON");
        return false;
    }
    ++p;

    p = skip_ws(p);
    if (*p == '}') return true;  // пустой объект: остаются значения по умолчанию

    while (*p != '\0') {
        char key[40];
        p = parse_string(p, key, sizeof(key));
        if (!p) {
            if (err && err_len > 0) snprintf(err, err_len, "%s", "ожидалось имя поля");
            return false;
        }

        p = skip_ws(p);
        if (*p != ':') {
            if (err && err_len > 0) snprintf(err, err_len, "%s", "ожидалось двоеточие после имени поля");
            return false;
        }
        p = skip_ws(p + 1);

        // Вложенная секция ("api": {...}) либо плоский ключ ("brightness": 128).
        const Section section = section_by_name(key);
        const char* after = nullptr;

        if (section != Section::None && *p == '{') {
            if (!parse_section(section, p, out, &after)) {
                if (err && err_len > 0) snprintf(err, err_len, "не удалось разобрать секцию «%s»", key);
                return false;
            }
        } else if (!apply_pair(key, p, out, &after)) {
            if (err && err_len > 0) snprintf(err, err_len, "не удалось разобрать поле «%s»", key);
            return false;
        }
        p = skip_ws(after);

        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '}') return true;
        if (err && err_len > 0) snprintf(err, err_len, "%s", "ожидалась запятая или конец объекта");
        return false;
    }

    if (err && err_len > 0) snprintf(err, err_len, "%s", "объект не закрыт");
    return false;
}

size_t json_escape(const char* src, char* out, size_t out_len)
{
    if (out_len == 0) return kNoBytes;

    // Таблица символов, которые обязательно экранировать в JSON.
    static const struct {
        char ch;
        const char* escaped;
    } kEscapes[] = {
        {'"', "\\\""},
        {'\\', "\\\\"},
        {'\n', "\\n"},
        {'\r', "\\r"},
        {'\t', "\\t"},
    };

    size_t written = 0;
    for (const char* p = src; *p != '\0'; ++p) {
        const char* repl = nullptr;
        size_t repl_len = 1;

        for (const auto& e : kEscapes) {
            if (e.ch == *p) {
                repl = e.escaped;
                repl_len = strlen(e.escaped);
                break;
            }
        }

        char literal = *p;
        if (!repl) {
            repl = &literal;
            repl_len = 1;
        }

        // Управляющие символы без короткого вида записываем как \u00XX.
        if (static_cast<unsigned char>(*p) < 0x20 && repl == &literal) {
            char buf[8];
            const int n = snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(*p));
            if (written + static_cast<size_t>(n) >= out_len) break;
            memcpy(out + written, buf, static_cast<size_t>(n));
            written += static_cast<size_t>(n);
            continue;
        }

        if (written + repl_len >= out_len) break;  // не влезло — обрезаем, но не рвём
        memcpy(out + written, repl, repl_len);
        written += repl_len;
    }

    out[written] = '\0';
    return written;
}

size_t write_config_json(const Config& cfg, char* out, size_t out_len)
{
    if (out_len == 0) return kNoBytes;

    char url[kMaxUrl * 6];
    char model[kMaxModel * 6];
    char tz[kMaxTimezone * 6];
    json_escape(cfg.api_base_url, url, sizeof(url));
    json_escape(cfg.api_model, model, sizeof(model));
    json_escape(cfg.timezone, tz, sizeof(tz));

    // Формат совпадает с ARCHITECTURE.md, 5.3: вложенные секции, чтобы файл
    // читался человеком и правился руками. Ключ API здесь отсутствует по
    // построению — он в NVS.
    const int n = snprintf(out, out_len,
        "{\n"
        "  \"api\": {\n"
        "    \"base_url\": \"%s\",\n"
        "    \"model\": \"%s\",\n"
        "    \"temperature\": %.1f,\n"
        "    \"reasoning_effort\": %u\n"
        "  },\n"
        "  \"search\": {\"enabled\": %s, \"max_results\": %u, \"engine\": \"native\"},\n"
        "  \"ui\": {\"brightness\": %u, \"layout\": \"%s\", \"timeout_min\": %u},\n"
        "  \"power\": {\"warn\": %u, \"critical\": %u, \"shutdown\": %u},\n"
        "  \"time\": {\"tz\": \"%s\"}\n"
        "}\n",
        url, model, cfg.temperature_tenths / 10.0, static_cast<unsigned>(cfg.reasoning_effort),
        cfg.search_enabled ? "true" : "false", static_cast<unsigned>(cfg.search_max_results),
        static_cast<unsigned>(cfg.brightness), cfg.layout_russian ? "ru" : "en",
        static_cast<unsigned>(cfg.screen_timeout_min), static_cast<unsigned>(cfg.battery_warn_percent),
        static_cast<unsigned>(cfg.battery_critical_percent), static_cast<unsigned>(cfg.battery_shutdown_percent), tz);

    if (n < 0 || static_cast<size_t>(n) >= out_len) return kNoBytes;
    return static_cast<size_t>(n);
}

}  // namespace settings
