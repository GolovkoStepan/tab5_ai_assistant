// Проверка и нормализация путей документов. Без Arduino — тестируется на хосте.

#include "util/doc_path.h"

#include <string.h>

namespace paths {
namespace {

bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/// Разрешённые символы сегмента: латиница, цифры, дефис, подчёркивание, точка
/// (только внутри имени — расширение отдельно проверяется).
bool is_name_char(char c)
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '_' || c == '.';
}

char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Транслитерация кириллицы: одна буква — одна или несколько латинских.
/// Таблица нарочно простая и предсказуемая, а не «правильная» по стандарту.
const char* translit(char32_t cp, char* buf, size_t buf_len)
{
    switch (cp) {
    case U'а': return "a";
    case U'б': return "b";
    case U'в': return "v";
    case U'г': return "g";
    case U'д': return "d";
    case U'е': return "e";
    case U'ё': return "e";
    case U'ж': return "zh";
    case U'з': return "z";
    case U'и': return "i";
    case U'й': return "y";
    case U'к': return "k";
    case U'л': return "l";
    case U'м': return "m";
    case U'н': return "n";
    case U'о': return "o";
    case U'п': return "p";
    case U'р': return "r";
    case U'с': return "s";
    case U'т': return "t";
    case U'у': return "u";
    case U'ф': return "f";
    case U'х': return "kh";
    case U'ц': return "ts";
    case U'ч': return "ch";
    case U'ш': return "sh";
    case U'щ': return "shch";
    case U'ъ': return "";
    case U'ы': return "y";
    case U'ь': return "";
    case U'э': return "e";
    case U'ю': return "yu";
    case U'я': return "ya";
    default:
        (void)buf;
        (void)buf_len;
        return nullptr;
    }
}

/// Прочитать один UTF-8 символ. Возвращает длину в байтах (0 — конец строки).
size_t utf8_next(const char* s, char32_t* out)
{
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c == 0) return 0;
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *out = static_cast<char32_t>(((c & 0x1F) << 6) | (static_cast<unsigned char>(s[1]) & 0x3F));
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *out = static_cast<char32_t>(((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
                                     (static_cast<unsigned char>(s[2]) & 0x3F));
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *out = static_cast<char32_t>(((c & 0x07) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 12) |
                                     ((static_cast<unsigned char>(s[2]) & 0x3F) << 6) |
                                     (static_cast<unsigned char>(s[3]) & 0x3F));
        return 4;
    }
    // Битый байт: пропускаем по одному, чтобы не зациклиться.
    *out = 0xFFFD;
    return 1;
}

/// Кириллица в нижнем/верхнем регистре — и греческий, и латиница: для слога
/// регистр не важен, транслит одинаковый.
char32_t fold(char32_t cp)
{
    if (cp >= U'А' && cp <= U'Я') return cp - U'А' + U'а';
    if (cp == U'Ё') return U'ё';
    return cp;
}

}  // namespace

const char* error_text(Result r)
{
    switch (r) {
    case Result::Ok: return "путь в порядке";
    case Result::Empty:
        return "пустой путь. Укажите путь вида home/air-purifier-filter.md (латиница, цифры, дефис, слэш)";
    case Result::TooLong: return "путь слишком длинный. Сократите: не больше 96 символов";
    case Result::TooDeep:
        return "слишком много вложенных папок. Не больше трёх уровней, например home/kitchen/notes/file.md";
    case Result::SegmentTooLong: return "слишком длинное имя папки или файла. Не больше 48 символов в одном сегменте";
    case Result::Absolute:
        return "путь не должен начинаться со слэша. Укажите путь относительно архива, например home/file.md";
    case Result::Traversal: return "в пути есть «..», это запрещено. Укажите путь внутри архива, например home/file.md";
    case Result::DotSegment: return "в пути есть сегмент «.», это запрещено. Укажите путь вида home/file.md";
    case Result::BadChar:
        return "в пути недопустимый символ. Разрешены только латиница, цифры, дефис, "
               "подчёркивание и слэш; пример: home/air-purifier-filter.md";
    case Result::BadExtension: return "документ должен быть файлом .md. Пример: home/air-purifier-filter.md";
    case Result::EmptySegment: return "в пути пустой сегмент (двойной слэш). Пример: home/air-purifier-filter.md";
    }
    return "путь не подходит";
}

Result check(const char* in)
{
    if (in == nullptr || in[0] == '\0') return Result::Empty;
    if (in[0] == '/') return Result::Absolute;

    const size_t len = strlen(in);
    if (len >= kMaxPathLen) return Result::TooLong;

    // Разбираем по сегментам. Так «..» проверяется как отдельный сегмент, а не
    // как подстрока: имя `a..b.md` — законное, а `a/../b.md` — нет.
    size_t segments = 0;
    size_t i = 0;
    while (i <= len) {
        const size_t start = i;
        while (i < len && in[i] != '/')
            ++i;
        const size_t seg_len = i - start;

        if (seg_len == 0) {
            // Пустой сегмент: либо путь пуст, либо двойной слэш, либо слэш в
            // конце. Путь из одного сегмента закончится здесь после цикла.
            if (segments == 0) return Result::Empty;
            return Result::EmptySegment;
        }
        if (seg_len == 1 && in[start] == '.') return Result::DotSegment;
        if (seg_len == 2 && in[start] == '.' && in[start + 1] == '.') return Result::Traversal;
        if (seg_len > kMaxSegmentLen) return Result::SegmentTooLong;

        for (size_t k = 0; k < seg_len; ++k) {
            if (!is_name_char(in[start + k])) return Result::BadChar;
        }

        segments += 1;
        if (i < len)
            ++i;  // пропустить слэш
        else
            break;  // дошли до конца
    }

    if (segments == 0) return Result::Empty;
    // Последний сегмент — файл, остальные — папки.
    if (segments - 1 > kMaxDepth) return Result::TooDeep;

    // Расширение: только .md.
    const char* dot = strrchr(in, '.');
    const char* slash = strrchr(in, '/');
    if (dot == nullptr || (slash != nullptr && dot < slash)) return Result::BadExtension;
    if (strlen(dot) != 3) return Result::BadExtension;
    // «home/.md» — файл без имени: точка сразу после слэша или в начале.
    // Такой документ нельзя ни открыть осмысленно, ни показать в списке.
    if (dot == in || (slash != nullptr && dot == slash + 1)) return Result::EmptySegment;
    if (to_lower(dot[1]) != 'm' || to_lower(dot[2]) != 'd') return Result::BadExtension;

    return Result::Ok;
}

Result normalize(const char* in, char* out, size_t out_len)
{
    if (out == nullptr || out_len < kMaxPathLen) return Result::TooLong;

    // Отсекаем ведущие «./» и пробелы: модель иногда пишет «./home/x.md».
    const char* p = in;
    if (p == nullptr) return Result::Empty;
    while (*p == ' ' || *p == '\t')
        ++p;
    while (p[0] == '.' && p[1] == '/')
        p += 2;
    if (p[0] == '\0') return Result::Empty;

    char tmp[kMaxPathLen];
    size_t n = 0;
    for (const char* q = p; *q != '\0' && n + 1 < sizeof(tmp); ++q) {
        tmp[n++] = to_lower(*q);
    }
    // Хвостовые пробелы — та же привычка модели, что и ведущие.
    while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t'))
        --n;
    tmp[n] = '\0';
    if (n == 0) return Result::Empty;

    // Добавить .md, если расширения нет вовсе. Если расширение есть, но не
    // .md — check() откажет: молча менять `file.txt` на `file.txt.md` нельзя,
    // модель бы не поняла, куда делся файл.
    size_t len = strlen(tmp);
    const char* slash = strrchr(tmp, '/');
    const char* dot = strrchr(tmp, '.');
    const bool has_ext = dot != nullptr && (slash == nullptr || dot > slash);
    if (!has_ext) {
        if (len + 3 >= sizeof(tmp)) return Result::TooLong;
        tmp[len++] = '.';
        tmp[len++] = 'm';
        tmp[len++] = 'd';
        tmp[len] = '\0';
    }

    const Result r = check(tmp);
    if (r != Result::Ok) return r;

    if (len + 1 > out_len) return Result::TooLong;
    memcpy(out, tmp, len + 1);
    return Result::Ok;
}

bool slugify(const char* text, char* out, size_t out_len)
{
    if (text == nullptr || out == nullptr || out_len < 2) return false;

    size_t n = 0;
    bool last_dash = true;  // в начале дефис не нужен
    const char* p = text;

    while (*p != '\0') {
        char32_t cp = 0;
        const size_t step = utf8_next(p, &cp);
        if (step == 0) break;
        p += step;

        // Проверять надо код символа, а не его младший байт: у кириллицы
        // U+0430 («а») младший байт равен 0x30 — это '0', и без проверки на
        // ASCII любая русская буква проходила бы как цифра.
        if (cp < 0x80 && is_alpha(static_cast<char>(cp))) {
            if (n + 1 < out_len) {
                out[n++] = to_lower(static_cast<char>(cp));
                last_dash = false;
            }
            continue;
        }
        if (cp < 0x80 && is_digit(static_cast<char>(cp))) {
            if (n + 1 < out_len) {
                out[n++] = static_cast<char>(cp);
                last_dash = false;
            }
            continue;
        }

        char buf[8];
        const char* tr = translit(fold(cp), buf, sizeof(buf));
        if (tr != nullptr) {
            // Известная буква. Транслит может быть пустым («ь», «ъ») — это не
            // повод ставить дефис: «фильтр» должно стать «filtr», а не «fil-tr».
            for (const char* t = tr; *t != '\0' && n + 1 < out_len; ++t) {
                out[n++] = *t;
                last_dash = false;
            }
            continue;
        }

        // Всё остальное (пробелы, знаки, эмодзи) — один дефис на группу.
        if (!last_dash && n + 1 < out_len) {
            out[n++] = '-';
            last_dash = true;
        }
    }

    // Убрать хвостовой дефис.
    while (n > 0 && out[n - 1] == '-')
        --n;
    out[n] = '\0';
    return n > 0;
}

void folder_of(const char* path, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;
    out[0] = '\0';
    if (path == nullptr) return;
    const char* slash = strrchr(path, '/');
    if (slash == nullptr) return;
    const size_t n = static_cast<size_t>(slash - path);
    if (n + 1 > out_len) return;
    memcpy(out, path, n);
    out[n] = '\0';
}

void name_of(const char* path, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;
    out[0] = '\0';
    if (path == nullptr) return;

    const char* slash = strrchr(path, '/');
    const char* name = (slash != nullptr) ? slash + 1 : path;

    // Отрезать .md
    const size_t full = strlen(name);
    size_t n = full;
    if (full > 3 && name[full - 3] == '.') n = full - 3;

    if (n + 1 > out_len) n = out_len - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

}  // namespace paths
