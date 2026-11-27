// Разбор и сборка YAML-фронтматтера. Без Arduino — тестируется на хосте.

#include "util/front_matter.h"

#include <stdio.h>
#include <string.h>

namespace doc {
namespace {

constexpr const char* kFence = "---";

/// Прочитать значение после «ключ:». Убирает пробелы по краям и обрамляющие
/// кавычки, если они есть.
void copy_value(const char* src, size_t src_len, char* dst, size_t dst_len)
{
    if (dst_len == 0) return;
    dst[0] = '\0';

    // Пробелы слева
    while (src_len > 0 && (*src == ' ' || *src == '\t')) {
        ++src;
        --src_len;
    }
    // Пробелы справа
    while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\t' || src[src_len - 1] == '\r')) {
        --src_len;
    }

    // Обрамляющие кавычки: "текст" или 'текст'. В двойных кавычках внутри
    // могут быть экранированные \" и \\ — их надо снять, иначе после
    // «собрать → разобрать» в заголовке остались бы обратные косые.
    bool quoted = false;
    if (src_len >= 2 && ((src[0] == '"' && src[src_len - 1] == '"') || (src[0] == '\'' && src[src_len - 1] == '\''))) {
        quoted = src[0] == '"';
        ++src;
        src_len -= 2;
    }

    size_t n = 0;
    for (size_t i = 0; i < src_len && n + 1 < dst_len; ++i) {
        if (quoted && src[i] == '\\' && i + 1 < src_len && (src[i + 1] == '"' || src[i + 1] == '\\')) {
            dst[n++] = src[++i];
            continue;
        }
        dst[n++] = src[i];
    }
    dst[n] = '\0';
}

/// Сравнить ключ строки с ожидаемым: «title» совпадает с «title:» и «title :».
bool key_is(const char* line, size_t len, const char* key)
{
    const size_t key_len = strlen(key);
    if (len < key_len) return false;
    if (memcmp(line, key, key_len) != 0) return false;

    size_t i = key_len;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    return i < len && line[i] == ':';
}

/// Указатель на значение в строке «ключ: значение».
const char* value_of(const char* line, size_t len, size_t* value_len)
{
    size_t i = 0;
    while (i < len && line[i] != ':')
        ++i;
    if (i >= len) {
        *value_len = 0;
        return line + len;
    }
    ++i;  // за двоеточие
    *value_len = len - i;
    return line + i;
}

/// Найти конец строки, начиная с позиции start.
size_t line_end(const char* text, size_t len, size_t start)
{
    size_t i = start;
    while (i < len && text[i] != '\n')
        ++i;
    return i;
}

/// Строка целиком равна «---» (с возможным \r)?
bool is_fence(const char* line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
        --len;
    return len == 3 && memcmp(line, kFence, 3) == 0;
}

/// Границы одного тега в списке. Разделитель — запятая; запятая внутри тега
/// невозможна by design (см. Meta::tags), поэтому кавычки тут только
/// снимаются, но не влияют на разбиение.
///
/// @param list  начало списка
/// @param len   его длина
/// @param pos   [вход/выход] позиция разбора
/// @param tag   [выход] начало тега
/// @param tag_len [выход] его длина
/// @return false, если тегов больше нет
bool next_tag(const char* list, size_t len, size_t& pos, const char** tag, size_t* tag_len)
{
    while (pos < len && (list[pos] == ' ' || list[pos] == '\t' || list[pos] == ','))
        ++pos;
    if (pos >= len) return false;

    const size_t start = pos;
    while (pos < len && list[pos] != ',')
        ++pos;

    const char* t = list + start;
    size_t n = pos - start;
    while (n > 0 && (*t == ' ' || *t == '\t')) {
        ++t;
        --n;
    }
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t'))
        --n;

    // Снять обрамляющие кавычки: документ мог быть написан руками.
    if (n >= 2 && t[0] == '"' && t[n - 1] == '"') {
        ++t;
        n -= 2;
    }

    if (n == 0) return false;  // пустой элемент между запятыми
    *tag = t;
    *tag_len = n;
    return true;
}

/// Разобрать содержимое `[...]` в строку «a, b, c» без квадратных скобок.
void parse_tags(const char* src, size_t len, char* out, size_t out_len)
{
    out[0] = '\0';
    if (len == 0) return;

    // Снять квадратные скобки и пробелы по краям.
    while (len > 0 && (*src == ' ' || *src == '[')) {
        ++src;
        --len;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == ']' || src[len - 1] == '\r'))
        --len;

    size_t n = 0;
    size_t pos = 0;
    const char* tag = nullptr;
    size_t tag_len = 0;
    bool first = true;
    while (next_tag(src, len, pos, &tag, &tag_len)) {
        if (!first && n + 2 < out_len) {
            out[n++] = ',';
            out[n++] = ' ';
        }
        for (size_t k = 0; k < tag_len && n + 1 < out_len; ++k)
            out[n++] = tag[k];
        first = false;
    }
    out[n] = '\0';
}

}  // namespace

bool Meta::operator==(const Meta& o) const
{
    return strcmp(title, o.title) == 0 && strcmp(tags, o.tags) == 0 && strcmp(source_url, o.source_url) == 0 &&
           strcmp(created, o.created) == 0 && strcmp(updated, o.updated) == 0;
}

bool parse(const char* text, size_t len, Meta& out, const char** body_out, size_t* body_len)
{
    out = Meta{};
    if (body_out != nullptr) *body_out = nullptr;
    if (body_len != nullptr) *body_len = 0;

    if (text == nullptr || len == 0) return false;

    // Первая строка обязана быть «---».
    const size_t first_end = line_end(text, len, 0);
    if (!is_fence(text, first_end)) {
        // Не документ: тело — весь файл, чтобы вызывающий мог его показать.
        if (body_out != nullptr) *body_out = text;
        if (body_len != nullptr) *body_len = len;
        return false;
    }

    // Идём по строкам до закрывающего «---».
    size_t pos = first_end;
    if (pos < len && text[pos] == '\n') ++pos;

    Meta parsed;
    bool closed = false;
    while (pos < len) {
        const size_t end = line_end(text, len, pos);
        const size_t line_len = end - pos;
        const char* line = text + pos;

        if (is_fence(line, line_len)) {
            closed = true;
            pos = end;
            if (pos < len && text[pos] == '\n') ++pos;
            break;
        }

        size_t vlen = 0;
        if (key_is(line, line_len, "title")) {
            const char* v = value_of(line, line_len, &vlen);
            copy_value(v, vlen, parsed.title, sizeof(parsed.title));
        } else if (key_is(line, line_len, "tags")) {
            const char* v = value_of(line, line_len, &vlen);
            parse_tags(v, vlen, parsed.tags, sizeof(parsed.tags));
        } else if (key_is(line, line_len, "source_url")) {
            const char* v = value_of(line, line_len, &vlen);
            copy_value(v, vlen, parsed.source_url, sizeof(parsed.source_url));
        } else if (key_is(line, line_len, "created")) {
            const char* v = value_of(line, line_len, &vlen);
            copy_value(v, vlen, parsed.created, sizeof(parsed.created));
        } else if (key_is(line, line_len, "updated")) {
            const char* v = value_of(line, line_len, &vlen);
            copy_value(v, vlen, parsed.updated, sizeof(parsed.updated));
        }
        // Незнакомые ключи пропускаем: чужая версия прошивки могла дописать своё
        // поле, и из-за него документ ломаться не должен.

        pos = end;
        if (pos < len && text[pos] == '\n') ++pos;
    }

    // Фронтматтер без закрывающего «---» — это не фронтматтер, а текст,
    // который случайно начался с «---». Документом не считаем и поля не
    // отдаём: иначе список показал бы «Обрыв» как нормальный документ.
    if (!closed) {
        if (body_out != nullptr) *body_out = text;
        if (body_len != nullptr) *body_len = len;
        return false;
    }

    // Одна пустая строка между фронтматтером и телом — разделитель, а не
    // часть текста. Без этого тело после «собрать → разобрать» начиналось бы
    // с лишнего перевода строки.
    if (pos + 1 < len && text[pos] == '\n') ++pos;

    out = parsed;
    if (body_out != nullptr) *body_out = text + pos;
    if (body_len != nullptr) *body_len = len - pos;
    return true;
}

size_t escape_value(const char* value, char* out, size_t out_len)
{
    if (value == nullptr || out == nullptr || out_len < 4) return 0;
    // Пустая строка не пишется вовсе: 0 означает «нечего писать», и
    // вызывающий пропускает поле.
    const size_t value_len = strlen(value);
    if (value_len == 0) return 0;

    // Можно ли писать значение как есть.
    //
    // Правила YAML мягче, чем «экранировать всё подозрительное»: двоеточие
    // опасно только перед пробелом или в конце (иначе ломались бы все URL и
    // метки времени), решётка — только после пробела или в начале строки.
    bool need_quotes = false;
    if (value[0] == ' ' || value[0] == '\t' || value[value_len - 1] == ' ') need_quotes = true;

    if (!need_quotes) {
        // Индикатор в начале строки. '-' и '?' опасны только с пробелом после,
        // но закавычить лишний раз дешевле, чем отлаживать YAML.
        static const char kLeading[] = "-?:,[]{}#&*!|>'\"%@`";
        if (strchr(kLeading, value[0]) != nullptr) need_quotes = true;
    }

    for (size_t i = 0; i < value_len && !need_quotes; ++i) {
        const char c = value[i];
        if (c == '\n' || c == '\r') {
            need_quotes = true;
        } else if (c == ':' && (i + 1 == value_len || value[i + 1] == ' ' || value[i + 1] == '\t')) {
            need_quotes = true;
        } else if (c == '#' && (i == 0 || value[i - 1] == ' ' || value[i - 1] == '\t')) {
            need_quotes = true;
        } else if (strchr("[]{}\"'\\`", c) != nullptr) {
            need_quotes = true;
        }
    }

    if (!need_quotes) {
        if (value_len + 1 > out_len) return 0;
        memcpy(out, value, value_len + 1);
        return value_len;
    }

    // В кавычках: экранируем обратную косую и кавычку, переводы строк
    // превращаем в пробел (YAML умеет многострочные, но нам это не нужно).
    size_t n = 0;
    if (n + 1 >= out_len) return 0;
    out[n++] = '"';
    for (size_t i = 0; i < value_len; ++i) {
        if (n + 3 >= out_len) return 0;
        if (value[i] == '"' || value[i] == '\\') {
            out[n++] = '\\';
            out[n++] = value[i];
        } else if (value[i] == '\n' || value[i] == '\r') {
            out[n++] = ' ';
        } else {
            out[n++] = value[i];
        }
    }
    if (n + 2 > out_len) return 0;
    out[n++] = '"';
    out[n] = '\0';
    return n;
}

size_t title_from_body(const char* body, size_t len, char* out, size_t out_len)
{
    if (out == nullptr || out_len < 2) return 0;
    out[0] = '\0';
    if (body == nullptr) return 0;
    if (len == 0) len = strlen(body);

    // Ищем первую непустую строку. Пропускаем ровно пустые и «из пробелов»:
    // тело, начатое с перевода строки, — обычное дело после фронтматтера.
    size_t pos = 0;
    while (pos < len) {
        const size_t end = line_end(body, len, pos);
        const char* line = body + pos;
        size_t line_len = end - pos;

        // Без \r: файл мог приехать с Windows, и заголовок с невидимым \r
        // попал бы во фронтматтер, где ломает разбор.
        if (line_len > 0 && line[line_len - 1] == '\r') --line_len;

        // Пустая строка или одни пробелы — ищем дальше.
        size_t first = 0;
        while (first < line_len && (line[first] == ' ' || line[first] == '\t'))
            ++first;
        if (first == line_len) {
            pos = end + 1;
            continue;
        }

        // Снять ведущие пробелы.
        line += first;
        line_len -= first;

        // Разметка начала строки. Заголовок «# Как сбросить» в списке читался
        // бы как мусор: человек писал название, а не разметку.
        size_t skip = 0;
        while (skip < line_len && line[skip] == '#')
            ++skip;
        if (skip > 0) {
            if (skip == line_len) {
                // Строка из одних решёток — пустой заголовок markdown, а не
                // название. Берём следующую строку.
                pos = end + 1;
                continue;
            }
            // После решёток по правилам markdown должен быть пробел; если его
            // нет («#1 способ»), решётка — часть текста, не заголовок.
            if (line[skip] == ' ' || line[skip] == '\t') {
                line += skip;
                line_len -= skip;
                while (line_len > 0 && (*line == ' ' || *line == '\t')) {
                    ++line;
                    --line_len;
                }
            }
        } else if (line_len >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
                   (line[1] == ' ' || line[1] == '\t')) {
            line += 2;
            line_len -= 2;
            while (line_len > 0 && (*line == ' ' || *line == '\t')) {
                ++line;
                --line_len;
            }
        }

        // Пробелы и приметы пункта справа.
        while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t'))
            --line_len;

        if (line_len == 0) {
            // Строка состояла из одной разметки — берём следующую.
            pos = end + 1;
            continue;
        }

        // Обрезка по границе UTF-8: продолжающие байты — 0b10xxxxxx. Иначе
        // кириллический заголовок кончался бы половиной буквы.
        size_t n = line_len;
        bool truncated = false;
        if (n + 1 > out_len) {
            truncated = true;
            // Оставляем место под «…» (3 байта): обрубок без многоточия
            // читается как настоящий конец названия.
            const size_t limit = out_len - 1 > 3 ? out_len - 1 - 3 : out_len - 1;
            n = limit;
            while (n > 0 && (static_cast<unsigned char>(line[n]) & 0xC0) == 0x80)
                --n;
            if (n == 0) {
                // Один символ шире буфера — писать нечего.
                return 0;
            }
            // Отступаем до границы слова: «...проверяете связь и» кончается
            // союзом, и это выглядит как обрыв, а не как название.
            size_t word = n;
            while (word > 0 && line[word - 1] != ' ' && line[word - 1] != '\t' && word > n / 2)
                --word;
            if (word > 0 && word > n / 2) n = word;
            while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == ','))
                --n;
        }

        memcpy(out, line, n);
        if (truncated) {
            memcpy(out + n, "…", 3);
            n += 3;
        }
        out[n] = '\0';
        return n;
    }

    return 0;
}

size_t build(const Meta& meta, const char* body, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;

    // title обязателен: по нему документ показывается в списке.
    if (meta.title[0] == '\0') return 0;

    size_t n = 0;
    // Помощник: дописать строку, не выходя за буфер.
    auto put = [&](const char* s) -> bool {
        const size_t len = strlen(s);
        if (n + len + 1 > out_len) return false;
        memcpy(out + n, s, len);
        n += len;
        out[n] = '\0';
        return true;
    };

    // Значение с экранированием.
    auto put_field = [&](const char* key, const char* value) -> bool {
        char esc[256];
        if (escape_value(value, esc, sizeof(esc)) == 0) return false;
        return put(key) && put(esc) && put("\n");
    };

    if (!put("---\n")) return 0;
    if (!put_field("title: ", meta.title)) return 0;

    // Теги — потоковый список YAML: `[a, b]`. Каждый тег экранируется
    // отдельно: если закавычить всю строку «a, b», получится ОДИН тег с
    // запятой внутри.
    if (meta.tags[0] != '\0') {
        char tags[192];
        size_t tn = 0;
        tags[0] = '\0';
        size_t pos = 0;
        const char* tag = nullptr;
        size_t tag_len = 0;
        bool first = true;
        while (next_tag(meta.tags, strlen(meta.tags), pos, &tag, &tag_len)) {
            char raw[64];
            if (tag_len >= sizeof(raw)) tag_len = sizeof(raw) - 1;
            memcpy(raw, tag, tag_len);
            raw[tag_len] = '\0';

            char esc[96];
            const size_t esc_len = escape_value(raw, esc, sizeof(esc));
            if (esc_len == 0) return 0;

            const size_t need = (first ? 0 : 2) + esc_len;
            if (tn + need + 1 > sizeof(tags)) return 0;
            if (!first) {
                tags[tn++] = ',';
                tags[tn++] = ' ';
            }
            memcpy(tags + tn, esc, esc_len);
            tn += esc_len;
            tags[tn] = '\0';
            first = false;
        }

        if (!first && (!put("tags: [") || !put(tags) || !put("]\n"))) return 0;
    }

    if (meta.source_url[0] != '\0' && !put_field("source_url: ", meta.source_url)) return 0;
    if (meta.created[0] != '\0' && !put_field("created: ", meta.created)) return 0;
    if (meta.updated[0] != '\0' && !put_field("updated: ", meta.updated)) return 0;

    // Пустая строка после фронтматтера — разделитель по конвенции markdown.
    // parse() её пропускает, поэтому «собрать → разобрать» тело не удлиняет.
    if (!put("---\n")) return 0;
    if (body != nullptr && body[0] != '\0') {
        if (!put("\n") || !put(body)) return 0;
        // Тело должно заканчиваться переводом строки: иначе следующая запись
        // приклеится к последней строке.
        if (out[n - 1] != '\n' && !put("\n")) return 0;
    }

    return n;
}

}  // namespace doc
