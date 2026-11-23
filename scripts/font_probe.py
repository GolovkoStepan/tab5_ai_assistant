#!/usr/bin/env python3
"""Разбор шрифтов M5GFX: какие символы действительно рисуются.

Зачем этот скрипт. При переборе клавиш на живой плате выяснилось, что `_`
не отображается, а `\\` рисуется как `¥`. Визуально шрифт `lgfxJapanGothic_16`
выглядел нормально — в образцах вроде «Съешь ещё этих мягких булок» этих
знаков просто нет. Гадать по внешнему виду нельзя, нужен разбор таблицы
глифов. Решение Р-017 в docs/DECISIONS.md.

Формат u8g2 (см. M5GFX, lgfx_fonts.cpp::U8g2font::getGlyph):

  байты 0..22   заголовок
    [0]         количество глифов
    [2],[3]     бит на нулевой/единичный отрезок в битовом потоке
    [4..8]      бит на ширину, высоту, x-смещение, y-смещение, шаг
    [17..18]    смещение сегмента заглавных
    [19..20]    смещение сегмента строчных
    [21..22]    смещение unicode-сегмента (коды > 255)

  далее три сегмента с кодами < 256 и один с кодами > 255. Запись сегмента:
  байт кода, байт размера записи, данные; размер 0 — конец сегмента.

Поиск глифа повторяет M5GFX один в один: иначе результат не совпадёт с тем,
что видно на экране.

Использование:
    ./scripts/font_probe.py                 # проверить все шрифты из комплекта
    ./scripts/font_probe.py --list          # что вообще есть
    ./scripts/font_probe.py --glyph U+005F  # показать глиф точками

По умолчанию читает зависимости из .pio/libdeps/tab5/M5GFX.
"""

import argparse
import os
import sys

DEFAULT_ROOT = '.pio/libdeps/tab5/M5GFX/src/lgfx/Fonts'

# Что нужно интерфейсу: печать, кириллица, типографика, стрелки.
NEEDED = (
    [(c, chr(c)) for c in range(0x20, 0x7F)]
    + [(c, chr(c)) for c in range(0x410, 0x450)]
    + [(0x0401, 'Ё'), (0x0451, 'ё')]
    + [(0x00AB, '«'), (0x00BB, '»'), (0x2116, '№')]
    + [(0x2190, '←'), (0x2191, '↑'), (0x2192, '→'), (0x2193, '↓')]
    + [(0x00B0, '°'), (0x00B7, '·'), (0x2014, '—'), (0x2026, '…')]
)

# Шрифты, которые имеет смысл сравнивать:
#   имя переменной в C++, файл, имя C-массива, объявленный размер.
CANDIDATES = [
    ('lgfxJapanGothic_16', 'IPA/lgfx_font_japan.c', 'lgfx_font_japan_gothic_16', 159271),
    ('efontJA_16', 'efont/lgfx_efont_ja.c', 'lgfx_efont_ja_16', 467155),
    ('efontCN_16', 'efont/lgfx_efont_cn.c', 'lgfx_efont_cn_16', 318199),
]

SIMPLE_ESCAPES = {'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34, "'": 39}


def read_c_array(path, name, max_bytes):
    """Достаёт байты C-массива `name` из исходника.

    Читаем файл целиком в бинарном виде и идём по нему линейно: регулярки по
    17-мегабайтной строке съедают память и вешают процесс.
    """
    raw = open(path, 'rb').read()
    key = name.encode() + b'['
    try:
        start = raw.index(key)
    except ValueError:
        return None
    start = raw.index(b'=', start) + 1

    out = bytearray()
    n = len(raw)
    i = start
    while i < n and len(out) < max_bytes:
        c = raw[i]
        if c == 0x3B:  # ';'
            break
        if c != 0x22:  # '"'
            i += 1
            continue
        i += 1
        while i < n and raw[i] != 0x22:
            c = raw[i]
            if c != 0x5C:  # '\\'
                out.append(c)
                i += 1
                continue
            e = raw[i + 1]
            if 0x30 <= e <= 0x37:  # восьмеричная escape-последовательность
                j = i + 1
                digits = b''
                while j < n and 0x30 <= raw[j] <= 0x37 and len(digits) < 3:
                    digits += bytes([raw[j]])
                    j += 1
                out.append(int(digits, 8) & 0xFF)
                i = j
            elif e == 0x78:  # 'x'
                j = i + 2
                digits = b''
                while j < n and chr(raw[j]) in '0123456789abcdefABCDEF':
                    digits += bytes([raw[j]])
                    j += 1
                out.append(int(digits[:2], 16) & 0xFF)
                i = j
            else:
                out.append(SIMPLE_ESCAPES.get(chr(e), e))
                i += 2
        i += 1
    return bytes(out)


class U8g2Font:
    """Минимальный разборщик u8g2-шрифта: покрытие и метрики глифов."""

    def __init__(self, name, data):
        self.name = name
        self.d = data
        d = data
        self.bits_zero, self.bits_one = d[2], d[3]
        self.bits_w, self.bits_h = d[4], d[5]
        self.bits_x, self.bits_y, self.bits_adv = d[6], d[7], d[8]
        self.pos_upper = self._be16(17)
        self.pos_lower = self._be16(19)
        self.pos_unicode = self._be16(21)

    def _be16(self, off):
        return (self.d[off] << 8) | self.d[off + 1]

    def glyph_ptr(self, code):
        """Указатель на данные глифа. None — глифа нет."""
        d = self.d
        if code <= 255:
            if code >= 0x61:
                start = self.pos_lower
            elif code >= 0x41:
                start = self.pos_upper
            else:
                start = 0
            f = 23 + start
            while f + 1 < len(d) and d[f + 1]:
                if d[f] == code:
                    return f + 2
                f += d[f + 1]
            return None

        # Коды > 255: LUT пар (смещение, код), смещения накапливаются,
        # пока встреченный код меньше искомого. Так же в M5GFX.
        # Важно: «код больше искомого» — НЕ повод сдаться. M5GFX просто ищет
        # в блоке, на котором остановился LUT. Если добавить здесь ранний
        # выход, кириллица перестаёт находиться и покрытие врёт.
        lut = 23 + self.pos_unicode
        font = lut
        ptr = lut
        steps = 0
        while True:
            if ptr + 3 >= len(d):
                return None
            font += self._be16(ptr)
            enc = self._be16(ptr + 2)
            ptr += 4
            steps += 1
            if steps > 200000:
                return None
            if not (enc < code):
                break
        guard = 0
        while font + 2 < len(d):
            guard += 1
            if guard > 100000:
                return None
            enc = self._be16(font)
            if enc == 0:
                return None
            if enc == code:
                return font + 3
            font += d[font + 2]
        return None

    def _bits(self, ptr):
        """Читалка битового потока глифа (u8g2_font_decode_t)."""
        state = {'ptr': ptr, 'bit': 0}
        d = self.d

        def unsigned(count):
            bp = state['bit']
            val = d[state['ptr']] >> bp
            nb = bp + count
            if nb >= 8:
                nb -= 8
                state['ptr'] += 1
                val |= d[state['ptr']] << (8 - bp)
            state['bit'] = nb
            return val & ((1 << count) - 1)

        def signed(count):
            return unsigned(count) - (1 << (count - 1))

        return unsigned, signed

    def metrics(self, code):
        """(ширина, высота, шаг) либо None, если глифа нет."""
        ptr = self.glyph_ptr(code)
        if ptr is None:
            return None
        unsigned, signed = self._bits(ptr)
        w = unsigned(self.bits_w)
        h = unsigned(self.bits_h)
        unsigned(self.bits_x)
        unsigned(self.bits_y)
        adv = signed(self.bits_adv)
        return w, h, adv

    def render(self, code):
        """ASCII-арт глифа."""
        ptr = self.glyph_ptr(code)
        if ptr is None:
            return None
        unsigned, _ = self._bits(ptr)
        w = unsigned(self.bits_w)
        h = unsigned(self.bits_h)
        unsigned(self.bits_x)
        unsigned(self.bits_y)
        unsigned(self.bits_adv)
        if not w or not h:
            return []

        grid = [[' '] * w for _ in range(h)]
        lx = ly = 0
        while ly < h:
            runs = [unsigned(self.bits_zero), unsigned(self.bits_one)]
            i = 0
            while True:
                length = runs[i]
                while length:
                    take = min(length, w - lx)
                    length -= take
                    if i:
                        for k in range(lx, lx + take):
                            if ly < h:
                                grid[ly][k] = '#'
                    lx += take
                    if lx == w:
                        lx = 0
                        ly += 1
                i ^= 1
                if i == 0 and not unsigned(1):
                    break
        return [''.join(row) for row in grid]


def load(path, c_name, declared):
    """Загрузить шрифт: c_name — имя массива в C, а не переменной C++."""
    if not os.path.exists(path):
        return None
    data = read_c_array(path, c_name, declared + 8)
    if not data or len(data) < 64:
        return None
    return U8g2Font(c_name, data)


def report(font):
    missing, blank = [], []
    for code, ch in NEEDED:
        m = font.metrics(code)
        if m is None:
            missing.append(ch)
        elif m[0] == 0 or m[1] == 0:
            blank.append(ch)

    total = len(NEEDED)
    ok = total - len(missing) - len(blank)
    print(f'\n=== {font.name}: {len(font.d)} байт ===')
    print(f'  покрыто: {ok} из {total}')
    if missing:
        print(f'  НЕТ глифа ({len(missing)}): {"".join(missing)}')
    if blank:
        print(f'  ПУСТОЙ глиф ({len(blank)}): {"".join(blank)}')
    if not missing and not blank:
        print('  весь нужный набор на месте')
    return ok


def main():
    ap = argparse.ArgumentParser(description='Разбор шрифтов M5GFX.')
    ap.add_argument('--root', default=DEFAULT_ROOT, help='каталог Fonts в M5GFX')
    ap.add_argument('--list', action='store_true', help='показать проверяемые шрифты')
    ap.add_argument('--glyph', metavar='U+XXXX', help='нарисовать глиф точками')
    args = ap.parse_args()

    if args.list:
        for name, rel, c_name, declared in CANDIDATES:
            print(f'{name:20} {rel:28} {c_name:28} {declared} байт')
        return 0

    if args.glyph:
        if args.glyph.lower().startswith('u+'):
            code = int(args.glyph[2:], 16)
        else:
            code = ord(args.glyph)
        for name, rel, c_name, declared in CANDIDATES:
            font = load(os.path.join(args.root, rel), c_name, declared)
            if font is None:
                print(f'{name}: не найден')
                continue
            art = font.render(code)
            ch = chr(code)
            if art is None:
                print(f'\n{name}: U+{code:04X} {ch!r} — глифа нет')
            elif not art:
                m = font.metrics(code)
                print(f'\n{name}: U+{code:04X} {ch!r} — ПУСТОЙ глиф ({m[0]}x{m[1]})')
            else:
                print(f'\n{name}: U+{code:04X} {ch!r}')
                for row in art:
                    print('   |' + row + '|')
        return 0

    best = None
    for name, rel, c_name, declared in CANDIDATES:
        font = load(os.path.join(args.root, rel), c_name, declared)
        if font is None:
            print(f'{name}: файл не найден — пропущен')
            continue
        ok = report(font)
        if best is None or ok > best[1]:
            best = (name, ok)

    if best:
        print(f'\nЛучшее покрытие: {best[0]} ({best[1]} символов).')
    print('\nПравило: выбирать шрифт по разбору таблицы глифов, а не по образцу.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
