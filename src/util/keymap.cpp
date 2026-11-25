// Раскладка клавиатуры A164: позиция в матрице → символ.
// Чистые функции, без Arduino — чтобы тестировалось на хосте.

#include "util/keymap.h"

namespace keymap {
namespace {

/// Запись таблицы раскладки.
/// Для символьных клавиш русская раскладка не задаётся (0) — берётся латинская:
/// на клавишах со знаками препинания ЙЦУКЕН ведёт себя так же, как ANSI.
struct KeyDef {
    Special special;
    char32_t latin;
    char32_t latin_shift;
    char32_t russian;
    char32_t russian_shift;
};

// Порядок — построчно, 14 клавиш в строке (row * 14 + col).
//
//   Row 0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del
//   Row 1: ` ! @ # $ % ^ & * ( ) [ ] backslash
//   Row 2: Tab q w e r t y u i o p ; ' Backspace
//   Row 3: Sym Aa a s d f g h j k l ↑ _ Enter
//   Row 4: Ctrl Alt z x c v b n m . ← ↓ → Space
constexpr KeyDef kKeys[kRows * kCols] = {
    // --- Row 0 ---
    {Special::Escape, 0, 0, 0, 0},
    {Special::None, U'1', U'!', 0, 0},
    {Special::None, U'2', U'@', 0, 0},
    {Special::None, U'3', U'#', 0, 0},
    {Special::None, U'4', U'$', 0, 0},
    {Special::None, U'5', U'%', 0, 0},
    {Special::None, U'6', U'^', 0, 0},
    {Special::None, U'7', U'&', 0, 0},
    {Special::None, U'8', U'*', 0, 0},
    {Special::None, U'9', U'(', 0, 0},
    {Special::None, U'0', U')', 0, 0},
    {Special::None, U'-', U'_', 0, 0},
    // Клавиша физически подписана «+»: без модификаторов отдаёт «+».
    {Special::None, U'+', U'+', 0, 0},
    {Special::Delete, 0, 0, 0, 0},

    // --- Row 1 ---
    // На «ё» в ЙЦУКЕН переехал ` (стандартное расположение Windows).
    {Special::None, U'`', U'~', U'ё', U'Ё'},
    {Special::None, U'!', U'!', 0, 0},
    {Special::None, U'@', U'@', 0, 0},
    {Special::None, U'#', U'#', 0, 0},
    {Special::None, U'$', U'$', 0, 0},
    {Special::None, U'%', U'%', 0, 0},
    {Special::None, U'^', U'^', 0, 0},
    {Special::None, U'&', U'&', 0, 0},
    {Special::None, U'*', U'*', 0, 0},
    {Special::None, U'(', U'(', 0, 0},
    {Special::None, U')', U')', 0, 0},
    {Special::None, U'[', U'{', U'х', U'Х'},
    {Special::None, U']', U'}', U'ъ', U'Ъ'},
    {Special::None, U'\\', U'|', 0, 0},

    // --- Row 2 ---
    {Special::Tab, 0, 0, 0, 0},
    {Special::None, U'q', U'Q', U'й', U'Й'},
    {Special::None, U'w', U'W', U'ц', U'Ц'},
    {Special::None, U'e', U'E', U'у', U'У'},
    {Special::None, U'r', U'R', U'к', U'К'},
    {Special::None, U't', U'T', U'е', U'Е'},
    {Special::None, U'y', U'Y', U'н', U'Н'},
    {Special::None, U'u', U'U', U'г', U'Г'},
    {Special::None, U'i', U'I', U'ш', U'Ш'},
    {Special::None, U'o', U'O', U'щ', U'Щ'},
    {Special::None, U'p', U'P', U'з', U'З'},
    {Special::None, U';', U':', U'ж', U'Ж'},
    {Special::None, U'\'', U'"', U'э', U'Э'},
    {Special::Backspace, 0, 0, 0, 0},

    // --- Row 3 ---
    {Special::Sym, 0, 0, 0, 0},
    {Special::Aa, 0, 0, 0, 0},
    {Special::None, U'a', U'A', U'ф', U'Ф'},
    {Special::None, U's', U'S', U'ы', U'Ы'},
    {Special::None, U'd', U'D', U'в', U'В'},
    {Special::None, U'f', U'F', U'а', U'А'},
    {Special::None, U'g', U'G', U'п', U'П'},
    {Special::None, U'h', U'H', U'р', U'Р'},
    {Special::None, U'j', U'J', U'о', U'О'},
    {Special::None, U'k', U'K', U'л', U'Л'},
    {Special::None, U'l', U'L', U'д', U'Д'},
    {Special::Up, 0, 0, 0, 0},
    // В кириллице на этой клавише живёт точка: на A164 нет отдельной
    // клавиши `.`, а без точки не набрать ни одного предложения. С Aa —
    // запятая, ровно как Shift+`.` на обычной клавиатуре.
    {Special::None, U'_', U'_', U'.', U','},
    {Special::Enter, 0, 0, 0, 0},

    // --- Row 4 ---
    {Special::Ctrl, 0, 0, 0, 0},
    {Special::Alt, 0, 0, 0, 0},
    {Special::None, U'z', U'Z', U'я', U'Я'},
    {Special::None, U'x', U'X', U'ч', U'Ч'},
    {Special::None, U'c', U'C', U'с', U'С'},
    {Special::None, U'v', U'V', U'м', U'М'},
    {Special::None, U'b', U'B', U'и', U'И'},
    {Special::None, U'n', U'N', U'т', U'Т'},
    {Special::None, U'm', U'M', U'ь', U'Ь'},
    {Special::None, U'.', U'>', U'ю', U'Ю'},
    {Special::Left, 0, 0, 0, 0},
    {Special::Down, 0, 0, 0, 0},
    {Special::Right, 0, 0, 0, 0},
    {Special::Space, U' ', U' ', 0, 0},
};

/// Слой символов (зажат Sym). Возвращает код и признак «переопределено».
/// Затрагивает только клавиши, у которых на Sym лежит другой символ.
///
/// Раскладка здесь нужна: на одной и той же клавише в латинице полезен знак
/// (`_` → `=`), а в кириллице — буква (`_` → `б`). Без раскладки пришлось бы
/// выбирать что-то одно и терять второе.
bool symOverride(uint8_t row, uint8_t col, Layout layout, bool shift, char32_t* out)
{
    // Клавиши с символьным слоем: `!*()[]\ ;' _ — как в прошивке клавиатуры.
    struct SymOverride {
        uint8_t row;
        uint8_t col;
        Layout layout;
        bool shift;
        char32_t code;
    };
    static constexpr SymOverride kOverrides[] = {
        {1, 0, Layout::Latin, false, U'~'},   // ` → ~
        {1, 1, Layout::Latin, false, U'?'},   // ! → ?
        {1, 8, Layout::Latin, false, U'/'},   // * → /
        {1, 9, Layout::Latin, false, U'<'},   // ( → <
        {1, 10, Layout::Latin, false, U'>'},  // ) → >
        {1, 11, Layout::Latin, false, U'{'},  // [ → {
        {1, 12, Layout::Latin, false, U'}'},  // ] → }
        {1, 13, Layout::Latin, false, U'|'},  // \ → |
        {2, 11, Layout::Latin, false, U':'},  // ; → :
        {2, 12, Layout::Latin, false, U'"'},  // ' → "
        {3, 12, Layout::Latin, false, U'='},  // _ → =

        // Пунктуация и «б» — для кириллицы.
        //
        // На A164 в нижнем ряду одна пунктуационная клавиша (`.`), тогда как на
        // обычной клавиатуре их две: `,` и `/.`. В ЙЦУКЕН на них живут «б» и
        // «ю/». Из-за этого по-русски нельзя было поставить НИ ТОЧКУ, НИ
        // ЗАПЯТУЮ, а буквы «б» не было вовсе — самая заметная потеря из всех,
        // ведь точка нужна в каждом предложении.
        //
        // Запятая на Sym+`.` совпадает с тем, что делает и сама M5Stack в своих
        // HID-таблицах для этой клавиатуры («Row 4 — Sym delta: . → ,»), то
        // есть это не самодеятельность, а восстановленное штатное поведение.
        {4, 9, Layout::Latin, false, U','},     // . → ,
        {4, 9, Layout::Russian, false, U','},   // ю → ,
        {3, 12, Layout::Russian, false, U'б'},  // _ → б (точка живёт на самой клавише)
        // Aa — заглавная. Без отдельной записи «Б» набрать нельзя:
        // переопределение возвращает символ раньше, чем применяется сдвиг.
        {3, 12, Layout::Russian, true, U'Б'},
    };

    for (const auto& o : kOverrides) {
        if (o.row == row && o.col == col && o.layout == layout && o.shift == shift) {
            *out = o.code;
            return true;
        }
    }
    return false;
}

}  // namespace

Special special(uint8_t row, uint8_t col)
{
    if (row >= kRows || col >= kCols) return Special::None;
    return kKeys[row * kCols + col].special;
}

char32_t codepoint(uint8_t row, uint8_t col, Layout layout, bool shift, bool sym)
{
    if (row >= kRows || col >= kCols) return 0;

    const KeyDef& k = kKeys[row * kCols + col];
    if (k.special != Special::None) return 0;

    // Слой символов перекрывает базовый, но не для букв.
    if (sym) {
        char32_t cp = 0;
        if (symOverride(row, col, layout, shift, &cp)) return cp;
    }

    // В русской раскладке у символьных клавиш значение не задано — берём латиницу.
    char32_t cp = (layout == Layout::Russian && k.russian != 0) ? k.russian : k.latin;
    if (shift && layout == Layout::Russian && k.russian_shift != 0) {
        cp = k.russian_shift;
    } else if (shift && k.latin_shift != 0) {
        cp = k.latin_shift;
    }
    return cp;
}

size_t utf8_encode(char32_t cp, char* out, size_t out_len)
{
    if (out == nullptr || cp == 0) return 0;

    if (cp < 0x80) {
        if (out_len < 1) return 0;
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        if (out_len < 2) return 0;
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (out_len < 3) return 0;
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        if (out_len < 4) return 0;
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

size_t char_utf8(uint8_t row, uint8_t col, Layout layout, bool shift, bool sym, char* out, size_t out_len)
{
    return utf8_encode(codepoint(row, col, layout, shift, sym), out, out_len);
}

}  // namespace keymap
