#!/usr/bin/env python3
"""Сверка serial-лога перебора клавиш с таблицей keymap.cpp.

Читает строки вида `r<n> c<n> -> "<символ>"` и `r<n> c<n> -> спец #<n>`
и сравнивает с ожидаемым содержимым матрицы A164.

Ожидаемые подписи взяты из src/util/keymap.cpp - если таблица изменится,
надо поправить и здесь (это осознанно: скрипт - независимый оракул).
"""

import re
import sys
from collections import OrderedDict

# Ожидаемое содержимое: (row, col) -> подпись или None для спецклавиш.
# Источник: src/util/keymap.cpp, массив kKeys по 14 клавиш в строке.
LATIN = {
    (0, 0): 'ESC', (0, 1): '1', (0, 2): '2', (0, 3): '3', (0, 4): '4',
    (0, 5): '5', (0, 6): '6', (0, 7): '7', (0, 8): '8', (0, 9): '9',
    (0, 10): '0', (0, 11): '-', (0, 12): '+', (0, 13): 'DEL',

    (1, 0): '`', (1, 1): '!', (1, 2): '@', (1, 3): '#', (1, 4): '$',
    (1, 5): '%', (1, 6): '^', (1, 7): '&', (1, 8): '*', (1, 9): '(',
    (1, 10): ')', (1, 11): '[', (1, 12): ']', (1, 13): '\\',

    (2, 0): 'TAB', (2, 1): 'q', (2, 2): 'w', (2, 3): 'e', (2, 4): 'r',
    (2, 5): 't', (2, 6): 'y', (2, 7): 'u', (2, 8): 'i', (2, 9): 'o',
    (2, 10): 'p', (2, 11): ';', (2, 12): "'", (2, 13): 'BS',

    (3, 0): 'SYM', (3, 1): 'AA', (3, 2): 'a', (3, 3): 's', (3, 4): 'd',
    (3, 5): 'f', (3, 6): 'g', (3, 7): 'h', (3, 8): 'j', (3, 9): 'k',
    (3, 10): 'l', (3, 11): 'UP', (3, 12): '_', (3, 13): 'ENTER',

    (4, 0): 'CTRL', (4, 1): 'ALT', (4, 2): 'z', (4, 3): 'x', (4, 4): 'c',
    (4, 5): 'v', (4, 6): 'b', (4, 7): 'n', (4, 8): 'm', (4, 9): '.',
    (4, 10): 'LEFT', (4, 11): 'DOWN', (4, 12): 'RIGHT', (4, 13): 'SPACE',
}

# ЙЦУКЕН: те же позиции, другие буквы. У символьных клавиш значения те же.
RUSSIAN = dict(LATIN)
RUSSIAN.update({
    (1, 0): 'ё',
    (2, 1): 'й', (2, 2): 'ц', (2, 3): 'у', (2, 4): 'к', (2, 5): 'е',
    (2, 6): 'н', (2, 7): 'г', (2, 8): 'ш', (2, 9): 'щ', (2, 10): 'з',
    (2, 11): 'ж', (2, 12): 'э',
    (3, 2): 'ф', (3, 3): 'ы', (3, 4): 'в', (3, 5): 'а', (3, 6): 'п',
    (3, 7): 'р', (3, 8): 'о', (3, 9): 'л', (3, 10): 'д',
    (4, 2): 'я', (4, 3): 'ч', (4, 4): 'с', (4, 5): 'м', (4, 6): 'и',
    (4, 7): 'т', (4, 8): 'ь', (4, 9): 'ю',
    (1, 11): 'х', (1, 12): 'ъ',
})

# Спецклавиши по номерам из keymap::Special.
SPECIAL_NAMES = {
    1: 'ESC', 2: 'TAB', 3: 'BS', 4: 'ENTER', 5: 'SPACE', 6: 'DEL',
    7: 'UP', 8: 'DOWN', 9: 'LEFT', 10: 'RIGHT', 11: 'SYM', 12: 'AA',
    13: 'CTRL', 14: 'ALT',
}

CHAR_RE = re.compile(r'r(\d+) c(\d+) → "(.*?)"')
SPEC_RE = re.compile(r'r(\d+) c(\d+) → спец #(\d+)')
# Переключение раскладки пишется отдельной строкой: у него нет ни символа,
# ни спецклавиши из таблицы, но позиция (Ctrl+Space) существует и нажималась.
TOGGLE_RE = re.compile(r'r(\d+) c(\d+) → Ctrl\+Space, раскладка (ENG|РУС)')


def main(path):
    raw = open(path, 'rb').read().decode('utf-8', errors='replace')

    # Разбор идёт по ходу лога, а не по «раскладке в конце»: за один перебор
    # раскладку переключают несколько раз, а Shift (Aa) меняет регистр. Раньше
    # сравнение шло с последней раскладкой и без учёта Shift, из-за чего
    # правильное «Ф» после нажатия Aa считалось расхождением с «ф».
    #
    # В логе видны управляющие символы (Esc, BS, Tab) - они ломают строки,
    # но сами события всё равно разбираются по префиксу "rN cM".
    events = []      # (row, col, что получено, раскладка на момент, shift на момент)
    layout = 'ENG'
    shift = False

    for line in raw.splitlines():
        m = TOGGLE_RE.search(line)
        if m:
            layout = m.group(3)  # раскладка ПОСЛЕ переключения
            events.append((int(m.group(1)), int(m.group(2)), 'SPACE', layout, shift))
            continue

        m = SPEC_RE.search(line)
        if m:
            row, col, num = int(m.group(1)), int(m.group(2)), int(m.group(3))
            name = SPECIAL_NAMES.get(num, f'#{num}')
            # Aa - защёлка регистра: следующее нажатие даст заглавную.
            if name == 'AA':
                shift = not shift
            events.append((row, col, name, layout, shift))
            continue

        m = CHAR_RE.search(line)
        if m:
            events.append((int(m.group(1)), int(m.group(2)), m.group(3), layout, ' Shift' in line))

    if not events:
        print('Событий не найдено. Лог пуст или плата не перезагрузилась.')
        return 1

    print(f'Событий: {len(events)}, раскладка в конце лога: {layout}\n')

    seen = OrderedDict()
    for row, col, got, _, _ in events:
        seen[(row, col)] = got

    print(f'Уникальных позиций: {len(seen)} из 70\n')

    # Сверяем каждое событие с той таблицей и тем регистром, что были в момент
    # нажатия. Сравнение с «раскладкой в конце лога» давало ложные расхождения:
    # за перебор раскладку переключают несколько раз.
    mismatches = []
    for row, col, got, ev_layout, ev_shift in events:
        if got in SPECIAL_NAMES.values():
            continue  # спецклавиша: сверяется по имени, регистр не при чём
        table = RUSSIAN if ev_layout == 'РУС' else LATIN
        base = table.get((row, col))
        if base is None:
            mismatches.append(((row, col), got, 'позиции нет в таблице'))
            continue
        if len(base) == 1 and base.isalpha():
            want = base.upper() if ev_shift else base
        else:
            want = base  # символ: Shift на него не влияет в нашей таблице
        if got != want:
            mismatches.append(((row, col), got, want))

    if mismatches:
        print('РАСХОЖДЕНИЯ:')
        for (row, col), got, want in mismatches:
            print(f'  r{row} c{col}: получили {got!r}, ожидали {want!r}')
    else:
        print('Расхождений нет - все нажатые позиции дают ожидаемые символы.')

    missing = [p for p in LATIN if p not in seen]
    if missing:
        print(f'\nНе нажимались ({len(missing)}):')
        line = ''
        for row, col in missing:
            item = f'{row},{col} '
            if len(line) + len(item) > 90:
                print('  ' + line)
                line = ''
            line += item
        if line:
            print('  ' + line)

    return 0 if not mismatches else 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '/tmp/tab5_keys.log'))
