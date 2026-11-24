// Видимое окно ленты чата. Без Arduino — тестируется на хосте.
//
// Заполнение снизу вверх: самая свежая строка прижата к низу области. Так при
// открытии треда видно конец разговора, а не его начало, и растущий ответ не
// уезжает за край экрана.

#include "util/chat_view.h"

namespace chatview {

size_t lines_per_page(int area_height, int line_height)
{
    if (line_height <= 0 || area_height <= 0) return 0;
    return static_cast<size_t>(area_height / line_height);
}

Window compute(const uint16_t* line_counts, size_t count, size_t max_lines, size_t scroll)
{
    Window w;
    if (line_counts == nullptr || count == 0 || max_lines == 0) return w;

    // Идём с конца, набирая строки, пока хватает места. scroll — сколько строк
    // пропустить снизу (прокрутка вверх).
    size_t budget = max_lines;
    size_t skipped = 0;

    int first = -1;
    size_t first_line = 0;
    size_t visible = 0;

    for (size_t i = count; i > 0; --i) {
        const size_t idx = i - 1;
        const size_t lines = line_counts[idx] > 0 ? line_counts[idx] : 1;

        // Прокрутка: пропускаем строки снизу, не показывая их.
        if (skipped < scroll) {
            const size_t skip = scroll - skipped;
            if (skip >= lines) {
                skipped += lines;
                w.clipped_bottom = true;
                continue;
            }
            // Сообщение видно частично: часть строк осталась под нижним краем.
            const size_t shown = lines - skip;
            skipped = scroll;
            w.clipped_bottom = true;
            if (shown > budget) {
                // Сверху тоже не помещается: окно — это середина сообщения.
                // Считаем от низа видимой части, а не от конца сообщения, иначе
                // прокрутка внутри длинного ответа показывала бы его хвост.
                first = static_cast<int>(idx);
                first_line = shown - budget;
                visible = 1;
                w.lines_used = budget;
                w.first_msg = first;
                w.first_line = first_line;
                w.visible_count = 1;
                w.clipped_top = true;
                return w;
            }
            budget -= shown;
            if (first < 0) first = static_cast<int>(idx);
            ++visible;
            w.lines_used += shown;
            continue;
        }

        if (lines > budget) {
            if (budget == 0) break;
            // Сообщение не влезает целиком: показываем его хвост, остальное
            // остаётся сверху за кадром. Счётчик не обнуляем: сообщения,
            // которые уже заняли место снизу, видны и должны быть нарисованы.
            first = static_cast<int>(idx);
            first_line = lines - budget;
            ++visible;
            w.lines_used += budget;
            w.clipped_top = true;
            w.first_msg = first;
            w.first_line = first_line;
            w.visible_count = visible;
            return w;
        }

        budget -= lines;
        first = static_cast<int>(idx);
        first_line = 0;
        ++visible;
        w.lines_used += lines;
    }

    // Всё, что выше окна, не показано.
    if (first > 0) w.clipped_top = true;

    w.first_msg = first;
    w.first_line = first_line;
    w.visible_count = visible;
    return w;
}

}  // namespace chatview
