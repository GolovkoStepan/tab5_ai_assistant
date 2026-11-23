/*
 * M5Stack TAB5 AI Assistant — точка входа прошивки.
 *
 * Этап v0.1.0 (bring-up). Задача — проверить на живой плате:
 *   - платформа собирается и стартует;
 *   - дисплей и тач инициализируются, поворот правильный;
 *   - кириллица рисуется без «квадратиков»;
 *   - клавиатура A164 отвечает, все 70 клавиш отдают ожидаемое, раскладка верна;
 *   - тач попадает туда, куда нажали;
 *   - питание читается через INA226;
 *   - SD-карта монтируется;
 *   - Wi-Fi через ESP32-C6 поднимается.
 *
 * Экран - матрица клавиатуры 5x14. Нажатая клавиша подсвечивается жёлтым, уже
 * проверенная становится зелёной, непроверенная остаётся тёмной. Ниже - счётчик
 * покрытия со списком ненажатых позиций и калибровочные мишени для тача: при
 * касании показывается фактическая координата и отклонение от ближайшей мишени.
 *
 * Экран диагностический и временный: на этапе v0.2.0 его заменит интерфейс чата.
 */

#include <M5Unified.h>
#include <SD_MMC.h>
#include <stdarg.h>
#include <string.h>

#include "app_config.h"
#include "hw/keyboard.h"
#include "hw/settings_store.h"
#include "hw/wifi.h"
#include "ui/draw.h"
#include "ui/wifi_screen.h"
#include "util/format.h"
#include "version.h"

hw::Keyboard g_keyboard;
hw::SettingsStore g_settings;
hw::WifiManager g_wifi;
ui::WifiScreen g_wifi_screen;

/// Какой экран сейчас на дисплее.
///
/// Диагностический экран bring-up и экран Wi-Fi живут одновременно: первый
/// остаётся инструментом проверки железа, второй — первым настоящим экраном
/// настроек. Переключение по Ctrl+W.
enum class Screen : uint8_t {
    Diagnostic,
    Wifi,
};
Screen g_screen = Screen::Diagnostic;

namespace {

/// Шрифт интерфейса: efontJA (Efont Biwidth), 16 px.
///
/// Почему не lgfxJapanGothic_16, который стоит по умолчанию в примерах M5GFX:
/// в нём два дефекта, критичных для редактора текста.
///   1. Обратный слэш (U+005C) нарисован как иена `¥` - шрифт следует JIS X 0201,
///      где 0x5C это знак иены, а не обратный слэш.
///   2. Подчёркивание (U+005F) - пустой глиф (w=0, h=0), то есть не рисуется
///      вообще. Проверено разбором таблицы глифов, не на глаз.
/// У efontJA_16 оба знака корректны, есть кириллица, «», №, стрелки, тире и
/// многоточие. Покрытие нужного набора - 171 символ из 172 (пустой только
/// пробел, так и должно быть).
///
/// Заголовок рисуется тем же шрифтом: отдельный efontJA_24 стоит +750 КБ
/// флеша ради одной строки на временном диагностическом экране.
const lgfx::IFont* kUiFont = &lgfx::fonts::efontJA_16;

// --- Геометрия экрана 1280x720 ----------------------------------------------
constexpr int kMargin = 10;
constexpr int kCellW = 90;
constexpr int kCellH = 42;
constexpr int kGridX = kMargin;
constexpr int kGridY = 60;
constexpr int kGridBottom = kGridY + keymap::kRows * kCellH;  // 270

constexpr int kStatusY = 6;
constexpr int kTitleY = 28;
constexpr int kLastY = kGridBottom + 8;  // 278
constexpr int kCoverY = kLastY + 26;     // 304
constexpr int kMissedY = kCoverY + 24;   // 328
/// Сколько строк отводим списку ненажатых позиций. При 70 ненажатых нужно
/// четыре строки, но они налезают на мишени тача: показываем три и «...».
constexpr int kMissedLines = 3;
constexpr int kTargetsY = 396;
constexpr int kTouchTextY = 654;
constexpr int kHintY = 708;

/// Сколько символов влезает в строку. Измерено по факту: при 16 px ширина
/// глифа ~13 px, то есть около 96 символов на 1280 px.
constexpr int kCharsPerLine = 96;

// --- Состояние проверки -----------------------------------------------------
/// Позиции, которые хотя бы раз нажали. Индекс - row * 14 + col.
bool g_seen[keymap::kRows * keymap::kCols] = {};

/// Покрытие на момент последней отрисовки - чтобы не перерисовывать зря.
int g_drawn_seen = -1;

/// Раскладка на момент последней отрисовки заголовка.
///
/// Счётчик покрытия при Ctrl+Space не меняется, поэтому заголовок с названием
/// раскладки оставался старым — «ENG» в строке и русская буква в поле ввода.
/// Отслеживаем раскладку отдельно.
keymap::Layout g_drawn_layout = keymap::Layout::Latin;

/// Последнее нажатие клавиши.
char g_last_text[128] = {};

/// Последнее касание экрана.
char g_touch_text[128] = "тач: коснитесь мишени";
bool g_has_touch = false;

/// Строка состояния, собирается в setup().
char g_status[192] = "диагностика…";

/// Текущая раскладка для экрана.
const char* layoutName()
{
    return g_keyboard.layout() == keymap::Layout::Russian ? "РУС" : "ENG";
}

/// Рисует строку с явно заданным выравниванием.
///
/// Выравнивание НЕ наследуется между вызовами намеренно: drawCell() внутри
/// сетки переключает datum на middle_center, и если после этого не задать его
/// заново, строка уезжает и обрезается слева.
void drawText(const char* text, int x, int y, uint16_t color, const lgfx::IFont* font = nullptr)
{
    M5.Display.setFont(font ? font : kUiFont);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(color, TFT_BLACK);
    M5.Display.drawString(text, x, y);
}

/// Печатает строку в Serial. На экран выводится отдельно - длинные сообщения
/// не должны лезть в сетку клавиш.
void logLine(const char* fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.println(buf);
}

/// Словесное описание состояния питания.
///
/// Аппаратного признака «есть ли аккумулятор» у Tab5 нет:
/// M5.Power.getBatteryPresence() возвращает unsupported. Напряжение тоже не
/// признак - без батареи INA226 показывает напряжение холостого хода зарядного
/// узла (наблюдали и 4.34 В, и 8.37 В при пустом отсеке). Достоверно судить
/// можно только по току: он течёт лишь когда батарея есть.
const char* chargeStateName(m5::charge_state_t state)
{
    switch (state) {
    case m5::charge_state_t::charging: return "заряд идёт";
    case m5::charge_state_t::discharging: return "разряд";
    case m5::charge_state_t::full: return "полный";
    case m5::charge_state_t::idle: return "покой (АКБ нет либо заряжен)";
    case m5::charge_state_t::not_charging: return "не заряжается";
    case m5::charge_state_t::disabled: return "заряд отключён";
    case m5::charge_state_t::io_error: return "ошибка I2C";
    case m5::charge_state_t::not_initialized: return "не инициализировано";
    default: return "не определяется";
    }
}

/// Строка состояния питания. Проценты показываем только когда по току видно,
/// что батарея реально работает: иначе они врут (100% на пустом отсеке).
void formatPowerStatus(char* out, size_t out_len)
{
    const int mv = M5.Power.getBatteryVoltage();
    const int ma = M5.Power.getBatteryCurrent();
    const auto state = M5.Power.getChargeState();

    const bool battery_working = state == m5::charge_state_t::charging || state == m5::charge_state_t::discharging;

    if (battery_working) {
        snprintf(out, out_len, "%d%% %.2fВ %+dмА", M5.Power.getBatteryLevel(), mv / 1000.0f, ma);
    } else {
        snprintf(out, out_len, "%.2fВ %+dмА", mv / 1000.0f, ma);
    }
}

/// Калибровочные мишени. Координаты в системе экрана (после поворота).
struct Target {
    int x;
    int y;
    char name;
};
constexpr Target kTargets[] = {
    {60, 460, 'A'},
    {1220, 460, 'B'},
    {640, 545, 'C'},
    {60, 620, 'D'},
    {1220, 620, 'E'},
};

/// Подпись клавиши для ячейки матрицы: «q/й», «Esc», «↑».
void cellLabel(uint8_t row, uint8_t col, char* out, size_t out_len)
{
    out[0] = '\0';

    switch (keymap::special(row, col)) {
    case keymap::Special::Escape: strlcpy(out, "Esc", out_len); return;
    case keymap::Special::Tab: strlcpy(out, "Tab", out_len); return;
    case keymap::Special::Backspace: strlcpy(out, "BS", out_len); return;
    case keymap::Special::Enter: strlcpy(out, "Ent", out_len); return;
    case keymap::Special::Space: strlcpy(out, "Spc", out_len); return;
    case keymap::Special::Delete: strlcpy(out, "Del", out_len); return;
    case keymap::Special::Up: strlcpy(out, "\u2191", out_len); return;
    case keymap::Special::Down: strlcpy(out, "\u2193", out_len); return;
    case keymap::Special::Left: strlcpy(out, "\u2190", out_len); return;
    case keymap::Special::Right: strlcpy(out, "\u2192", out_len); return;
    case keymap::Special::Sym: strlcpy(out, "Sym", out_len); return;
    case keymap::Special::Aa: strlcpy(out, "Aa", out_len); return;
    case keymap::Special::Ctrl: strlcpy(out, "Ctl", out_len); return;
    case keymap::Special::Alt: strlcpy(out, "Alt", out_len); return;
    case keymap::Special::None: break;
    }

    // Печатная клавиша: латиница и кириллица в одной ячейке.
    char latin[8] = {};
    char russian[8] = {};
    keymap::utf8_encode(keymap::codepoint(row, col, keymap::Layout::Latin, false, false), latin, sizeof(latin));
    keymap::utf8_encode(keymap::codepoint(row, col, keymap::Layout::Russian, false, false), russian, sizeof(russian));

    if (latin[0] && russian[0] && strcmp(latin, russian) != 0) {
        snprintf(out, out_len, "%s/%s", latin, russian);
    } else if (latin[0]) {
        strlcpy(out, latin, out_len);
    } else {
        strlcpy(out, russian, out_len);
    }
}

/// Цвет ячейки: жёлтый - зажата сейчас, зелёный - уже нажимали.
uint16_t cellColor(uint8_t row, uint8_t col, bool pressed_now)
{
    if (pressed_now) return TFT_YELLOW;
    if (g_seen[row * keymap::kCols + col]) return 0x0320;  // тёмно-зелёный
    return 0x18E3;                                         // тёмно-серый
}

void drawCell(uint8_t row, uint8_t col, bool pressed_now)
{
    const int x = kGridX + col * kCellW + 2;
    const int y = kGridY + row * kCellH + 2;
    const int w = kCellW - 4;
    const int h = kCellH - 4;

    const uint16_t fill = cellColor(row, col, pressed_now);
    M5.Display.fillRect(x, y, w, h, fill);
    M5.Display.drawRect(x, y, w, h, TFT_DARKGRAY);

    char label[16];
    cellLabel(row, col, label, sizeof(label));

    M5.Display.setFont(kUiFont);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(pressed_now ? TFT_BLACK : TFT_WHITE, fill);
    M5.Display.drawString(label, x + w / 2, y + h / 2);
}

int seenCount()
{
    int n = 0;
    for (int i = 0; i < keymap::kRows * keymap::kCols; ++i) {
        if (g_seen[i]) ++n;
    }
    return n;
}

/// Перерисовка подсветки сетки: зажатые сейчас и уже проверенные.
void drawGrid()
{
    for (uint8_t row = 0; row < keymap::kRows; ++row) {
        for (uint8_t col = 0; col < keymap::kCols; ++col) {
            drawCell(row, col, g_keyboard.isPressed(row, col));
        }
    }
    g_drawn_seen = seenCount();
}

/// Перерисовывает только те ячейки, чьё состояние изменилось.
///
/// Полная перерисовка 70 ячеек на каждое нажатие/отпускание давала заметное
/// мерцание. Здесь сравниваем текущее состояние с предыдущим и трогаем лишь
/// изменившиеся прямоугольники.
void updateGrid()
{
    static bool prev_pressed[keymap::kRows * keymap::kCols] = {};

    for (uint8_t row = 0; row < keymap::kRows; ++row) {
        for (uint8_t col = 0; col < keymap::kCols; ++col) {
            const uint8_t idx = row * keymap::kCols + col;
            const bool down = g_keyboard.isPressed(row, col);
            if (down == prev_pressed[idx]) continue;

            prev_pressed[idx] = down;
            drawCell(row, col, down);
        }
    }
    g_drawn_seen = seenCount();
}

/// Строка «Последняя: r2 c1 → "й"».
void drawLast()
{
    M5.Display.fillRect(0, kLastY, M5.Display.width(), 24, TFT_BLACK);
    drawText(g_last_text[0] ? g_last_text : "Последняя: -", kMargin, kLastY, TFT_WHITE);
}

/// Счётчик покрытия.
void drawCoverage()
{
    M5.Display.fillRect(0, kCoverY, M5.Display.width(), 22, TFT_BLACK);

    const int total = keymap::kRows * keymap::kCols;
    const int done = seenCount();

    char head[96];
    snprintf(head, sizeof(head), "Проверено %d из %d, раскладка %s", done, total, layoutName());
    drawText(head, kMargin, kCoverY, done == total ? TFT_GREEN : TFT_WHITE);

    g_drawn_layout = g_keyboard.layout();
}

/// Список ненажатых позиций, по несколько строк.
void drawMissed()
{
    M5.Display.fillRect(0, kMissedY, M5.Display.width(), kTargetsY - kMissedY - 4, TFT_BLACK);

    const int total = keymap::kRows * keymap::kCols;
    if (seenCount() == total) {
        drawText("Все клавиши нажимались.", kMargin, kMissedY, TFT_GREEN);
        return;
    }

    drawText("Не нажимались:", kMargin, kMissedY, TFT_LIGHTGRAY);

    char line[kCharsPerLine + 1] = {};
    size_t used = 0;
    int line_no = 0;
    bool clipped = false;

    for (int i = 0; i < total; ++i) {
        if (g_seen[i]) continue;

        char item[12];
        const int n = snprintf(item, sizeof(item), "%d,%d ", i / keymap::kCols, i % keymap::kCols);
        if (used + static_cast<size_t>(n) >= sizeof(line)) {
            line[used] = '\0';
            drawText(line, kMargin, kMissedY + 22 + line_no * 22, TFT_LIGHTGRAY);
            ++line_no;
            used = 0;
            line[0] = '\0';
            // Строки кончились: дальше только многоточие, чтобы не налезать
            // на раздел тача. Полный список всё равно есть в сетке выше.
            if (line_no >= kMissedLines) {
                clipped = true;
                break;
            }
        }
        memcpy(line + used, item, static_cast<size_t>(n));
        used += static_cast<size_t>(n);
    }
    if (used > 0) {
        line[used] = '\0';
        drawText(line, kMargin, kMissedY + 22 + line_no * 22, TFT_LIGHTGRAY);
    } else if (clipped) {
        drawText("…", kMargin, kMissedY + 22 + line_no * 22, TFT_LIGHTGRAY);
    }
}

/// Калибровочные мишени: крестик и буква.
void drawTargets()
{
    drawText("Тач: коснитесь мишени - покажу координату и отклонение", kMargin, kTargetsY, TFT_DARKGRAY);

    for (const Target& t : kTargets) {
        M5.Display.drawFastHLine(t.x - 14, t.y, 29, TFT_MAGENTA);
        M5.Display.drawFastVLine(t.x, t.y - 14, 29, TFT_MAGENTA);
        M5.Display.drawCircle(t.x, t.y, 3, TFT_MAGENTA);

        char name[2] = {t.name, '\0'};
        drawText(name, t.x + 18, t.y - 8, TFT_MAGENTA);
    }
}

/// Результат последнего касания: координата, ближайшая мишень и отклонение.
void drawTouchText()
{
    M5.Display.fillRect(0, kTouchTextY, M5.Display.width(), 26, TFT_BLACK);
    drawText(g_touch_text, kMargin, kTouchTextY, g_has_touch ? TFT_MAGENTA : TFT_DARKGRAY);
}

/// Крестик в точке касания. Нужен именно визуально: без него нельзя глазами
/// сверить, где палец и где драйвер сообщил координату.
void drawTouchMarker(int x, int y)
{
    constexpr int kArm = 18;
    M5.Display.drawCircle(x, y, 14, TFT_YELLOW);
    M5.Display.drawCircle(x, y, 2, TFT_YELLOW);
    M5.Display.drawFastHLine(x - kArm, y, 2 * kArm + 1, TFT_YELLOW);
    M5.Display.drawFastVLine(x, y - kArm, 2 * kArm + 1, TFT_YELLOW);
}

/// Область под крестиком, сохранённая перед его отрисовкой.
constexpr int kMarkerArm = 22;
uint16_t g_marker_backup[(2 * kMarkerArm + 1) * (2 * kMarkerArm + 1)];
int g_marker_x = -1;
int g_marker_y = -1;

/// Рисует крестик, предварительно запомнив то, что под ним.
///
/// Полная перерисовка экрана на каждое касание выглядела как мерцание -
/// fillScreen + 70 ячеек + текст дают заметную вспышку. Здесь трогаются только
/// пиксели под крестиком.
void saveAndDrawTouchMarker(int x, int y)
{
    constexpr int kSize = 2 * kMarkerArm + 1;
    int bx = x - kMarkerArm;
    int by = y - kMarkerArm;

    // У края экрана область обрезается - запоминаем фактические границы.
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (bx + kSize > M5.Display.width()) bx = M5.Display.width() - kSize;
    if (by + kSize > M5.Display.height()) by = M5.Display.height() - kSize;

    M5.Display.readRect(bx, by, kSize, kSize, g_marker_backup);
    g_marker_x = bx;
    g_marker_y = by;

    drawTouchMarker(x, y);
}

/// Возвращает сохранённую область на место - крестик исчезает без перерисовки.
void restoreUnderTouchMarker()
{
    if (g_marker_x < 0) return;

    constexpr int kSize = 2 * kMarkerArm + 1;
    M5.Display.pushImage(g_marker_x, g_marker_y, kSize, kSize, g_marker_backup);
    g_marker_x = -1;
    g_marker_y = -1;
}

/// Собирает текст о касании: ищем ближайшую мишень и считаем отклонение.
void updateTouchText(int x, int y)
{
    const Target* best = &kTargets[0];
    long best_d2 = -1;
    for (const Target& t : kTargets) {
        const long dx = x - t.x;
        const long dy = y - t.y;
        const long d2 = dx * dx + dy * dy;
        if (best_d2 < 0 || d2 < best_d2) {
            best_d2 = d2;
            best = &t;
        }
    }

    const int dx = x - best->x;
    const int dy = y - best->y;
    snprintf(g_touch_text, sizeof(g_touch_text), "тач: %d,%d · мишень %c (%d,%d) · откл. %+d,%+d", x, y, best->name,
        best->x, best->y, dx, dy);
    logLine("Тач: %d,%d мишень %c отклонение %+d,%+d", x, y, best->name, dx, dy);
}

/// Полная отрисовка экрана.
void drawAll()
{
    M5.Display.fillScreen(TFT_BLACK);

    drawText(g_status, kMargin, kStatusY, TFT_CYAN);

    // Заголовок и тестовая строка кириллицы: строчные, заглавные, Ё,
    // типографские кавычки, знак номера.
    drawText("TAB5 AI Assistant " APP_VERSION, kMargin, kTitleY, TFT_WHITE);
    drawText("Съешь же ещё этих мягких булок — «Ёж» №5…", 620, kTitleY + 4, TFT_LIGHTGRAY);

    drawGrid();
    drawLast();
    drawCoverage();
    drawMissed();
    drawTargets();
    drawTouchText();

    drawText("Ctrl+Space — раскладка · Ctrl+L — сброс покрытия · Ctrl+W — Wi-Fi", kMargin, kHintY - 16, TFT_DARKGRAY);
}

}  // namespace

void setup()
{
    auto m5cfg = M5.config();
    M5.begin(m5cfg);

    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("=== TAB5 AI Assistant " APP_VERSION " ===");

    // Панель физически 720x1280 (портрет), поэтому ландшафт - это поворот
    // 1 или 3, и они отличаются на 180 градусов. Поворот 1 даёт картинку
    // вверх ногами, правильный - 3.
    M5.Display.setRotation(3);
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(TFT_BLACK);

    logLine("Экран: %dx%d, поворот %d", M5.Display.width(), M5.Display.height(), M5.Display.getRotation());
    logLine("PSRAM: %u КБ, flash: %u МБ", (unsigned)(ESP.getPsramSize() / 1024),
        (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    logLine("Свободно: heap %u КБ, PSRAM %u КБ", (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(ESP.getFreePsram() / 1024));
    logLine("CPU: %u МГц", (unsigned)getCpuFrequencyMhz());

    // --- Питание ------------------------------------------------------------
    // Напряжение и ток INA226 печатаем как есть, без вывода «есть батарея»:
    // пустой отсек даёт такое же напряжение, как заряженный аккумулятор.
    // Состояние заряда M5Unified определяет по току - оно и показывается.
    const int millivolts = M5.Power.getBatteryVoltage();
    const int current = M5.Power.getBatteryCurrent();
    const auto charge_state = M5.Power.getChargeState();
    logLine("Питание: %.2f В, %+d мА, %s", millivolts / 1000.0f, current, chargeStateName(charge_state));
    logLine("Наличие АКБ: %s", M5.Power.getBatteryPresence() == m5::battery_presence_t::present
                                   ? "есть"
                                   : "не определяется (нет аппаратного признака)");
    logLine("Проценты (считаются от напряжения): %d %%", M5.Power.getBatteryLevel());

    char power_status[48];
    formatPowerStatus(power_status, sizeof(power_status));

    // --- SD -----------------------------------------------------------------
    // Вариант платы m5stack_tab5 не задаёт BOARD_SDMMC_POWER_CHANNEL, поэтому
    // Arduino не включает LDO4 - питание карты. Без этого mount падает
    // в send_op_cond timeout (0x107). esp-bsp делает это явно.
    SD_MMC.setPowerChannel(4);
    bool sd_ok = false;
    char sd_size[16] = "-";
    if (SD_MMC.begin(cfg::kMountPoint, false) && SD_MMC.cardType() != CARD_NONE) {
        format_bytes(SD_MMC.totalBytes(), sd_size, sizeof(sd_size));
        File probe = SD_MMC.open("/.probe", FILE_WRITE);
        if (probe) {
            probe.print("ok");
            probe.close();
            SD_MMC.remove("/.probe");
            sd_ok = true;
        }
        logLine("SD: %s, запись %s", sd_size, sd_ok ? "работает" : "НЕ работает");
    } else {
        logLine("SD: НЕ смонтирована");
    }

    // --- Настройки ----------------------------------------------------------
    // Секреты читаются из NVS в любом случае, файл — только если SD доступна.
    const bool settings_ok = g_settings.begin();
    logLine("Настройки: %s, модель %s, таймзона %s", settings_ok ? "из файла" : "по умолчанию",
        g_settings.config().api_model, g_settings.config().timezone);

    // --- Клавиатура ---------------------------------------------------------
    const bool kbd_ok = g_keyboard.begin();
    if (kbd_ok) {
        logLine("Клавиатура: OK, прошивка v%u", g_keyboard.firmwareVersion());
    } else {
        logLine("Клавиатура: НЕ отвечает на адресе 0x%02X", cfg::kKeyboardAddr);
    }

    // Настройки, которые видны сразу: яркость и раскладка. Читались они и
    // раньше, но не применялись — config.json существовал, а экран и
    // клавиатура жили на значениях по умолчанию. Это как раз то расхождение
    // документации с кодом, которое находят глазами, а не тестами.
    M5.Display.setBrightness(g_settings.config().brightness);
    g_keyboard.setLayout(g_settings.config().layout_russian ? keymap::Layout::Russian : keymap::Layout::Latin);
    logLine("Яркость %u, раскладка %s", g_settings.config().brightness, layoutName());

    // --- Wi-Fi --------------------------------------------------------------
    // Подключение к сохранённой сети запускаем в фоне: неблокирующий start
    // и tick() в цикле, иначе первый экран замирал бы до 20 секунд.
    const bool wifi_ok = g_wifi.begin();
    if (wifi_ok) {
        g_wifi.auto_connect(g_settings.secrets());
    } else {
        logLine("Wi-Fi: радио не поднялось");
    }

    // Экран Wi-Fi привязываем к драйверам здесь: его tick() работает с этого
    // момента, даже если экран ни разу не открывали.
    g_wifi_screen.init(g_wifi, g_settings);
    // Компактная строка состояния: подробности остались в Serial.
    snprintf(g_status, sizeof(g_status), "%s · SD %s%s · Wi-Fi %s · клав %s", power_status, sd_size,
        sd_ok ? " RW" : " RO", wifi_ok ? "OK" : "нет", kbd_ok ? "v1" : "нет");
    logLine("Статус: %s", g_status);

    drawAll();
}

void loop()
{
    M5.update();

    // --- Клавиатура ---------------------------------------------------------
    hw::KeyEvent ev;
    bool last_changed = false;
    while (g_keyboard.poll(ev)) {
        // Ctrl+W — переход между диагностикой и экраном Wi-Fi. Проверяем до
        // маршрутизации: это общая команда, она работает на любом экране.
        if (ev.ctrl && ev.type == hw::KeyEvent::Type::Char && strcmp(ev.utf8, "w") == 0) {
            if (g_screen == Screen::Diagnostic) {
                g_screen = Screen::Wifi;
                g_wifi_screen.onEnter();
            } else {
                g_wifi_screen.onExit();
                g_screen = Screen::Diagnostic;
                drawAll();
            }
            continue;
        }

        // Экран Wi-Fi обрабатывает события сам, включая Esc.
        if (g_screen == Screen::Wifi) {
            if (g_wifi_screen.onKey(ev)) {
                g_wifi_screen.onExit();
                g_screen = Screen::Diagnostic;
                drawAll();
            }
            continue;
        }

        // Ctrl+L - сбросить покрытие, чтобы пройти заново.
        if (ev.ctrl && ev.type == hw::KeyEvent::Type::Char && strcmp(ev.utf8, "l") == 0) {
            memset(g_seen, 0, sizeof(g_seen));
            drawGrid();
            drawCoverage();
            drawMissed();
            continue;
        }

        if (ev.row >= keymap::kRows || ev.col >= keymap::kCols) continue;

        // Переключение раскладки: позицию помечаем (клавиша нажата), но в
        // «последнее нажатие» пишем спецклавишу, а не мнимый символ.
        if (ev.special == keymap::Special::LayoutToggle) {
            g_seen[ev.row * keymap::kCols + ev.col] = true;
            drawCell(ev.row, ev.col, true);
            snprintf(g_last_text, sizeof(g_last_text), "Последняя: раскладка → %s", layoutName());
            // Метку пишем после переключения: драйвер уже сменил раскладку,
            // поэтому здесь верное значение, а не прежнее. Позицию указываем,
            // чтобы check_keylog.py засчитал клавишу как нажатую — это
            // Ctrl+Space, то есть физически Space.
            logLine("r%u c%u → Ctrl+Space, раскладка %s", ev.row, ev.col, layoutName());
            // Раскладку запоминаем в config.json: иначе после перезагрузки она
            // сбрасывалась бы в латиницу, хотя ARCHITECTURE 4.2 обещает
            // сохранение.
            {
                char err[96];
                g_settings.set_layout_russian(g_keyboard.layout() == keymap::Layout::Russian, err, sizeof(err));
            }
            last_changed = true;
            continue;
        }

        g_seen[ev.row * keymap::kCols + ev.col] = true;
        drawCell(ev.row, ev.col, true);

        if (ev.type == hw::KeyEvent::Type::Char) {
            snprintf(g_last_text, sizeof(g_last_text), "Последняя: r%u c%u → \"%s\"%s%s", ev.row, ev.col, ev.utf8,
                ev.ctrl ? " Ctrl" : "", ev.shift ? " Shift" : "");
            // Раскладку пишем в каждую строку: иначе по логу не понять, какая
            // таблица проверялась в этот момент.
            logLine("r%u c%u → \"%s\"%s%s [%s]", ev.row, ev.col, ev.utf8, ev.ctrl ? " Ctrl" : "",
                ev.shift ? " Shift" : "", layoutName());
        } else {
            snprintf(g_last_text, sizeof(g_last_text), "Последняя: r%u c%u → спец #%u%s%s", ev.row, ev.col,
                (unsigned)ev.special, ev.ctrl ? " Ctrl" : "", ev.shift ? " Shift" : "");
            logLine("r%u c%u → спец #%u%s%s [%s]", ev.row, ev.col, (unsigned)ev.special, ev.ctrl ? " Ctrl" : "",
                ev.shift ? " Shift" : "", layoutName());
        }
        last_changed = true;
    }

    // --- Экран Wi-Fi --------------------------------------------------------
    // tick() вызывается на любом экране: автоподключение к сохранённой сети
    // начинается ещё в setup(), и если ждать открытия экрана Wi-Fi, оно
    // никогда не завершится.
    g_wifi.tick();
    g_wifi_screen.tick();

    // Касание обрабатывается до выхода: на экране Wi-Fi оно выбирает сеть.
    const auto& td = M5.Touch.getDetail();

    if (g_screen == Screen::Wifi) {
        if (td.wasReleased()) g_wifi_screen.onTouch(td.x, td.y);
        if (g_wifi_screen.dirty()) g_wifi_screen.draw();
        delay(10);
        return;
    }

    // Подсветка зажатых клавиш: точечно, только изменившиеся ячейки.
    updateGrid();

    // Покрытие, раскладка и список ненажатых меняются редко - по факту.
    // Раскладку сравниваем отдельно: Ctrl+Space не меняет счётчик, но меняет
    // заголовок, и раньше он оставался с прежним «ENG».
    if (seenCount() != g_drawn_seen || g_keyboard.layout() != g_drawn_layout) {
        drawCoverage();
        drawMissed();
    }
    if (last_changed) drawLast();

    // --- Тач ----------------------------------------------------------------
    // Калибровочная мишень на диагностическом экране. Крестик рисуется ПОВЕРХ
    // содержимого, а перед этим область под ним сохраняется в память. При
    // отпускании сохранённое возвращается на место — никакой полной
    // перерисовки экрана, а значит нет мерцания.
    if (td.wasPressed()) {
        g_has_touch = true;
        updateTouchText(td.x, td.y);
        drawTouchText();
        saveAndDrawTouchMarker(td.x, td.y);
    } else if (td.wasReleased()) {
        restoreUnderTouchMarker();
    }

    delay(10);
}
