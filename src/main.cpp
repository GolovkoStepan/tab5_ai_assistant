/*
 * M5Stack TAB5 AI Assistant — точка входа прошивки.
 *
 * Основной экран — чат: лента сообщений, поле ввода, стриминг ответа модели
 * через OpenAI-совместимый API (v0.2.0). Треды лежат на SD в JSONL, ключ API —
 * в NVS.
 *
 * Маршрутизация: Ctrl+M — меню со всеми разделами и справкой, Ctrl+W — Wi-Fi,
 * Ctrl+A — настройки API, Ctrl+T — треды, Ctrl+O — документы, Ctrl+D —
 * диагностика; Esc с любого экрана возвращает в чат (а из меню — туда, откуда
 * его открыли). Диагностика — наследство этапа v0.1.0 (матрица клавиатуры 5x14,
 * покрытие клавиш, калибровочные мишени тача); она больше не стартовый экран,
 * но осталась инструментом проверки железа.
 */

#include <M5Unified.h>
#include <SD_MMC.h>
#include <stdarg.h>
#include <string.h>

#include "app_config.h"
#include "hw/api_client.h"
#include "hw/chat_store.h"
#include "hw/clock.h"
#include "hw/doc_store.h"
#include "hw/keyboard.h"
#include "hw/settings_store.h"
#include "hw/wifi.h"
#include "ui/api_settings_screen.h"
#include "ui/chat_screen.h"
#include "ui/draw.h"
#include "ui/help_screen.h"
#include "ui/notes_screen.h"
#include "ui/splash.h"
#include "ui/threads_screen.h"
#include "ui/tray.h"
#include "ui/wifi_screen.h"
#include "util/format.h"
#include "util/tray_state.h"
#include "version.h"

hw::Keyboard g_keyboard;
hw::SettingsStore g_settings;
hw::WifiManager g_wifi;
hw::ApiClient g_api;
hw::ChatStore g_chat_store;
hw::DocStore g_doc_store;
ui::WifiScreen g_wifi_screen;
ui::ChatScreen g_chat_screen;
ui::ApiSettingsScreen g_api_screen;
ui::ThreadsScreen g_threads_screen;
ui::NotesScreen g_notes_screen;
ui::HelpScreen g_help_screen;

/// Какой экран сейчас на дисплее.
///
/// Диагностический экран bring-up остаётся инструментом проверки железа,
/// остальные — настоящими экранами. Чат — основной; Ctrl+W и Ctrl+A уводят в
/// настройки, Esc возвращает в чат.
enum class Screen : uint8_t {
    Diagnostic,
    Chat,
    Threads,
    Notes,
    Wifi,
    ApiSettings,
    Help,
};
Screen g_screen = Screen::Chat;

/// Откуда открыли меню помощи: Esc и Ctrl+M возвращают туда, а не в чат.
/// Из архива возврат в чат читался бы как потеря места, где ты был.
Screen g_help_return = Screen::Chat;

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

/// Строка состояния питания.
///
/// Проценты показываем **только на разряде**: на Tab5 `getBatteryLevel()`
/// считает их по напряжению на ячейку, а зарядник держит 8.4 В — верхнюю
/// границу формулы. Поэтому при кабеле там всегда 100 % при любом реальном
/// заряде, и выводить это число значит врать в самой диагностике, которая
/// нужна как раз для того, чтобы понять настоящее состояние.
void formatPowerStatus(char* out, size_t out_len)
{
    const int mv = M5.Power.getBatteryVoltage();
    const int ma = M5.Power.getBatteryCurrent();
    const auto state = M5.Power.getChargeState();

    if (state == m5::charge_state_t::discharging) {
        snprintf(out, out_len, "%d%% %.2fВ %+dмА", M5.Power.getBatteryLevel(), mv / 1000.0f, ma);
    } else if (state == m5::charge_state_t::charging) {
        // «заряд» вместо числа — по той же причине, что и в трее: во время
        // заряда показание вольтметра упирается в потолок формулы, и процент
        // оттуда был бы нарисован, а не измерен.
        snprintf(out, out_len, "заряд %.2fВ %+dмА", mv / 1000.0f, ma);
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

    drawText("Ctrl+Space — раскладка · Ctrl+L — сброс покрытия · Ctrl+D — чат · Ctrl+M — меню", kMargin, kHintY - 16,
        TFT_DARKGRAY);

    // Трей и здесь: экран диагностики — тоже экран устройства, а заряд и сеть
    // нужны на нём не меньше. Тестовая строка кириллицы кончается на 948 px,
    // трей начинается с 1004 — они не пересекаются.
    ui::draw_tray(M5.Display.width());
}

}  // namespace

namespace {

/// Переключить экран. Сначала выгружаем текущий, потом входим в новый: у
/// каждого экрана есть onExit, и пропустить его значит оставить недописанное
/// или нестёртый пароль.
void switch_screen(Screen next)
{
    if (g_screen == next) return;

    switch (g_screen) {
    case Screen::Chat: g_chat_screen.onExit(); break;
    case Screen::Threads: g_threads_screen.onExit(); break;
    case Screen::Notes: g_notes_screen.onExit(); break;
    case Screen::Wifi: g_wifi_screen.onExit(); break;
    case Screen::ApiSettings: g_api_screen.onExit(); break;
    case Screen::Help: break;
    case Screen::Diagnostic: break;
    }

    g_screen = next;

    switch (g_screen) {
    case Screen::Chat: g_chat_screen.onEnter(); break;
    case Screen::Threads: g_threads_screen.onEnter(); break;
    case Screen::Notes: g_notes_screen.onEnter(); break;
    case Screen::Wifi: g_wifi_screen.onEnter(); break;
    case Screen::ApiSettings: g_api_screen.onEnter(); break;
    // Помощь возвращает туда, откуда её открыли: Esc из меню, открытого в
    // архиве, должен вернуть в архив, а не в чат. Это делает main, а не экран:
    // экран не знает, кто его позвал.
    case Screen::Help: g_help_screen.onEnter(); break;
    case Screen::Diagnostic: drawAll(); break;
    }
}

/// Открыть тред, выбранный в списке, и вернуться в чат.
///
/// Требование экрана списка выполняется здесь, а не в самом экране: смена
/// экрана — работа main, и только он знает, что открытие треда означает ещё и
/// переход в чат. Если открыть не удалось (идёт ответ, файл не прочитался),
/// остаёмся в списке и показываем причину там же.
/// Открытый тред удалили в списке — чат должен это увидеть.
///
/// Без этого лента, поле ввода и измерения относились бы к разговору, которого
/// больше нет на карте, а файл создался бы заново при следующем вопросе.
void sync_after_thread_delete()
{
    if (!g_threads_screen.take_open_removed()) return;
    g_chat_screen.new_thread();
}

bool open_thread_from_list()
{
    const char* slug = g_threads_screen.take_selected_slug();
    if (slug == nullptr || slug[0] == '\0') return false;

    // Копия: clear_request() обнулит буфер, а slug нужен и после перехода.
    char wanted[48];
    snprintf(wanted, sizeof(wanted), "%s", slug);
    g_threads_screen.clear_request();

    if (!g_chat_screen.open_thread(wanted)) {
        g_threads_screen.notice(g_chat_screen.last_error());
        return true;  // запрос был, но открыть не удалось — причина уже на экране
    }

    switch_screen(Screen::Chat);
    g_chat_screen.draw();
    return true;
}

/// Последнее прочитанное состояние питания.
///
/// Читается раз в секунду в update_power(): состояние заряда приходит по I2C с
/// INA226, а кадров при delay(10) около сотни в секунду. Заряд и разряд не
/// меняются за десять миллисекунд, а шина нужна клавиатуре.
///
/// Хранится не внутри функции, потому что нужен в двух местах: светодиоды
/// клавиатуры и трей. Два независимых опроса давали бы расхождение — правый
/// светодиод зелёный, а трей показывает «садится».
///
/// Напряжения здесь нет намеренно: в трее оно не показывается (при пустом
/// отсеке INA226 видит холостой ход зарядного узла, а тот скачет — см.
/// include/util/tray_state.h), а диагностика читает его сама при старте.
struct PowerSnapshot {
    m5::charge_state_t state{m5::charge_state_t::not_initialized};
    int level{-1};
    /// Аккумулятора нет. См. tray::next_presence: единственное доказательство
    /// наличия — ток, единственное доказательство отсутствия — напряжение ниже
    /// физически возможного для 2S-сборки.
    bool absent{false};
};

PowerSnapshot g_power;
unsigned long g_power_at_ms = 0;

/// Обновить состояние питания, если прошла секунда.
void update_power()
{
    const unsigned long now = millis();
    if (g_power_at_ms != 0 && now - g_power_at_ms < 1000) return;
    g_power_at_ms = now;

    g_power.state = M5.Power.getChargeState();

    // Наличие аккумулятора выводится отдельно от состояния заряда: M5Unified на
    // Tab5 видит только направление тока, а «тока нет» — это и «заряжен», и
    // «аккумулятора нет» (Р-016). Отсутствие доказывается напряжением, которое
    // в пустом отсеке «дышит» между 4.34 и 8.37 В (холостой ход IP2326).
    static tray::Presence presence;
    presence = tray::next_presence(presence, M5.Power.getBatteryCurrent() != 0, M5.Power.getBatteryVoltage());
    g_power.absent = presence.absent;

    // Проценты достоверны только на разряде: при заряде напряжение подпёрто
    // до верхней границы формулы `getBatteryLevel()`, а на пустом отсеке она
    // и вовсе гуляет между 0 и 100 % вместе с «дыханием» зарядного узла.
    // Правило одно на трей, светодиоды и строку диагностики, поэтому
    // считается здесь, а не в каждом потребителе.
    g_power.level =
        (g_power.state == m5::charge_state_t::discharging && !g_power.absent) ? M5.Power.getBatteryLevel() : -1;
}

/// Собрать снимок для трея.
tray::Info build_tray_info()
{
    tray::Info info;

    if (g_wifi.connected()) {
        info.wifi_connected = true;
        info.wifi_level = tray::wifi_level(true, g_wifi.rssi());
    } else if (g_wifi.connecting()) {
        info.wifi_connecting = true;
    }
    tray::wifi_label(info.wifi_connected, info.wifi_connecting, info.wifi_text, sizeof(info.wifi_text));

    // Состояние заряда с памятью: M5Unified на Tab5 различает только
    // направление тока, а «тока нет» — это и «заряжен», и «аккумулятора нет».
    // Вывод «заряжено» делается лишь потому, что заряд в этой сессии уже шёл,
    // и лишь пока известно, что аккумулятор на месте.
    static tray::Charge charge = tray::Charge::Unknown;
    charge = tray::next_charge(charge, g_power.state == m5::charge_state_t::charging,
        g_power.state == m5::charge_state_t::discharging, g_power.absent);
    info.charge = charge;

    // Процент достоверен только на разряде. Пока идёт заряд,
    // `getBatteryLevel()` показывает 100 % при любом реальном уровне (см.
    // include/util/tray_state.h).
    info.battery_known = charge == tray::Charge::Discharging && g_power.level >= 0;
    info.battery_percent = g_power.level;
    tray::battery_label(charge, info.battery_known, info.battery_percent, info.battery_text, sizeof(info.battery_text));

    return info;
}

/// Обновить светодиоды клавиатуры.
///
/// Вызывается раз в кадр: драйвер сам не трогает I2C, если ни цвет, ни яркость
/// не изменились, — а меняются они редко. Вызов дешёвый и на каждом кадре
/// нужен, потому что состояние берётся из разных мест (экран чата, INA226),
/// и собирать его «по событию» значило бы забыть про половину переходов.
void update_leds()
{
    led::Status s;
    g_chat_screen.fill_model_status(s);

    s.charging = g_power.state == m5::charge_state_t::charging;
    s.discharging = g_power.state == m5::charge_state_t::discharging;

    // Аккумулятора нет — гасим правый светодиод. Без этого одиночный выброс
    // тока при снятии батареи (направление определяется по току, а он в этот
    // момент скачет) мог показать зелёный «всё в порядке» на пустом отсеке —
    // то есть соврать ровно так же, как врал трей.
    if (g_power.absent) {
        s.charging = false;
        s.discharging = false;
    }

    s.battery_level = g_power.level;

    const led::Colors c = led::compute(s);
    g_keyboard.setLeds(c.model, c.power);
}

/// Показать трей на текущем экране, если снимок изменился.
///
/// Отдельно от update_leds(), хотя снимок тот же: светодиоды обновляются
/// каждый кадр (драйвер сам отсекает повторы по I2C), а трей — только при
/// изменении. Иначе сорок строк с фоном перерисовывались бы сто раз в секунду.
///
/// Снимок берётся раз в секунду, как и питание. Причина — RSSI: он меняется от
/// помех на единицы дБм, и опрос каждый кадр заставлял бы перерисовывать
/// значок почти непрерывно. Глазами это читается как мерцание, а пользы в
/// уровне сигнала с точностью до кадра нет.
void update_tray()
{
    static tray::Info drawn;
    static bool first = true;
    static unsigned long at_ms = 0;

    const unsigned long now = millis();
    if (!first && now - at_ms < 1000) return;
    at_ms = now;

    const tray::Info info = build_tray_info();
    ui::set_live_info(info);

    if (!first && info == drawn) return;
    first = false;
    drawn = info;

    // Рисуем на любом экране, включая диагностику: drawAll() рисует трей при
    // входе, но показания меняются и после — заряд садится, сеть пропадает.
    ui::draw_tray(M5.Display.width());
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

    // Заставка как только есть куда рисовать. Дальше каждый этап инициализации
    // отмечается в ней же: экран не остаётся чёрным на несколько секунд, и
    // видно, на чём именно устройство остановилось, если что-то не поднимется.
    ui::splash_begin(M5.Display.width(), M5.Display.height());

    logLine("Экран: %dx%d, поворот %d", M5.Display.width(), M5.Display.height(), M5.Display.getRotation());
    logLine("PSRAM: %u КБ, flash: %u МБ", (unsigned)(ESP.getPsramSize() / 1024),
        (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    logLine("Свободно: heap %u КБ, PSRAM %u КБ", (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(ESP.getFreePsram() / 1024));
    logLine("CPU: %u МГц", (unsigned)getCpuFrequencyMhz());

    ui::splash_step(ui::SplashStep::Display, "1280×720");

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
    logLine("Проценты (считаются от напряжения, при заряде недостоверны): %d %%", M5.Power.getBatteryLevel());

    char power_status[48];
    formatPowerStatus(power_status, sizeof(power_status));

    // Первое чтение состояния питания для трея и светодиодов. Дальше loop()
    // обновляет его раз в секунду; без этого до первой секунды трей собирался
    // бы из нулей и показывал «0.00 В».
    update_power();

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

    ui::splash_step(ui::SplashStep::Storage, sd_ok ? sd_size : "нет карты");

    // --- Настройки ----------------------------------------------------------
    // Секреты читаются из NVS в любом случае, файл — только если SD доступна.
    const bool settings_ok = g_settings.begin();
    logLine("Настройки: %s, модель %s, таймзона %s", settings_ok ? "из файла" : "по умолчанию",
        g_settings.config().api_model, g_settings.config().timezone);

    ui::splash_step(ui::SplashStep::Settings, g_settings.config().api_model);

    // --- Клавиатура ---------------------------------------------------------
    const bool kbd_ok = g_keyboard.begin();
    if (kbd_ok) {
        logLine("Клавиатура: OK, прошивка v%u", g_keyboard.firmwareVersion());
        // Заводская яркость светодиодов — 20; в тёмной комнате это заметно
        // слепит, а смысла в яркой индикации нет: светодиоды тут для
        // «взглянул и понял», а не для освещения. Половина от заводской.
        g_keyboard.setLedBrightness(10);
    } else {
        logLine("Клавиатура: НЕ отвечает на адресе 0x%02X", cfg::kKeyboardAddr);
    }

    ui::splash_step(ui::SplashStep::Keyboard, kbd_ok ? "A164" : "не найдена");

    // Настройки, которые видны сразу: яркость и раскладка. Читались они и
    // раньше, но не применялись — config.json существовал, а экран и
    // клавиатура жили на значениях по умолчанию. Это как раз то расхождение
    // документации с кодом, которое находят глазами, а не тестами.
    M5.Display.setBrightness(g_settings.config().brightness);
    g_keyboard.setLayout(g_settings.config().layout_russian ? keymap::Layout::Russian : keymap::Layout::Latin);
    logLine("Яркость %u, раскладка %s", g_settings.config().brightness, layoutName());

    // --- Часы ---------------------------------------------------------------
    // Таймзона нужна до первого треда: по ней чат формирует метки времени в
    // файле. Синхронизация по NTP пойдёт в loop(), когда поднимется Wi-Fi.
    hw::clock_sync::begin(g_settings.config());

    // --- Wi-Fi --------------------------------------------------------------
    // Подключение к сохранённой сети запускаем в фоне: неблокирующий start
    // и tick() в цикле, иначе первый экран замирал бы до 20 секунд.
    //
    // Сам `begin()` при этом блокирующий: на живой плате он занимает 2.3 с
    // (первый WiFi.mode поднимает SDIO к сопроцессору C6). Именно поэтому шаг
    // помечается ДО вызова, а не после: иначе полоса стояла бы 2.3 с без
    // объяснения, и заставка выглядела бы разыгранной. Здесь же на экране
    // видно «Сеть ● поднимаю радио…» — то есть что именно происходит.
    //
    // Блокирующее ожидание оставлено в заставке намеренно. Унести его в loop()
    // можно, но тогда на 2.3 с замирал бы уже рабочий экран с меню, где любое
    // нажатие проглатывается, — а заставка для того и нужна, чтобы старт
    // пережидался на экране, который этого и ожидает.
    ui::splash_step(ui::SplashStep::Network, "поднимаю радио…");

    const bool wifi_ok = g_wifi.begin();
    if (wifi_ok) {
        g_wifi.auto_connect(g_settings.secrets());
        // Подпись меняется на настоящую: радио уже поднято, а DHCP идёт в фоне.
        ui::splash_step(ui::SplashStep::Network, "подключаюсь в фоне");
    } else {
        logLine("Wi-Fi: радио не поднялось");
        // Отдельный вызов, потому что при отказе подпись должна измениться:
        // «поднимаю радио…» на неудавшемся шаге было бы неправдой.
        ui::splash_step(ui::SplashStep::Network, "радио нет");
    }

    // Экран Wi-Fi привязываем к драйверам здесь: его tick() работает с этого
    // момента, даже если экран ни разу не открывали.
    g_wifi_screen.init(g_wifi, g_settings);

    // --- Чат -----------------------------------------------------------------
    // Хранилище тредов и экран чата. Каталог /chats создаётся здесь же; без SD
    // чат работать не сможет, и это видно по логу, а не по «пустому экрану».
    const bool chats_ok = g_chat_store.begin();
    if (!chats_ok) logLine("Чат: хранилище тредов недоступно (SD?)");

    // Архив документов. Без него инструменты агента отвечают модели «архив
    // недоступен» — это лучше, чем отключить инструменты целиком: модель
    // скажет пользователю, в чём дело.
    const bool notes_ok = g_doc_store.begin();
    logLine("Архив: %s", notes_ok ? "готов" : "недоступен (SD?)");

    g_chat_screen.init(g_api, g_chat_store, g_settings, g_wifi, g_doc_store);
    g_api_screen.init(g_settings, g_wifi);
    g_threads_screen.init(g_chat_store);
    // Экран заметок получает ссылку на хранилище всегда, даже если архив не
    // поднялся: ready() проверяется внутри, и экран покажет причину, а не
    // пустоту вместо неё.
    g_notes_screen.init(g_doc_store, g_settings);
    g_help_screen.init();

    // Компактная строка состояния: подробности остались в Serial.
    snprintf(g_status, sizeof(g_status), "%s · SD %s%s · Wi-Fi %s · клав %s", power_status, sd_size,
        sd_ok ? " RW" : " RO", wifi_ok ? "OK" : "нет", kbd_ok ? "v1" : "нет");
    logLine("Статус: %s", g_status);

    // Стартуем в меню, а не в чате. При включении человек ещё не знает, зачем
    // взял устройство: меню показывает разом, что оно умеет и по какой
    // клавише, — это дешевле, чем вспоминать команды. Из меню в чат ведёт
    // Enter по первому пункту, а Esc возвращает в чат по умолчанию
    // (g_help_return = Chat).
    //
    // Чат всё равно готовим в фоне: onEnter() в меню не вызывается, поэтому
    // тред подгружаем здесь же. Иначе первый вход в чат начинался бы с пустой
    // ленты и чтения SD прямо на глазах.
    //
    // Снимок трея собираем до первой отрисовки: шапка рисует трей из
    // ui::live_info(), и без этого до первого прохода loop() на экране был бы
    // пустой трей — рамка батареи без подписи и палочки без уровня.
    ui::set_live_info(build_tray_info());

    g_chat_screen.onEnter();  // подготовить тред, но не показывать

    // Заставка досматривается здесь, а не раньше: к этому моменту всё уже
    // готово, и «Готово» с полной полосой — правда, а не обещание.
    ui::splash_finish();

    g_help_return = Screen::Chat;
    g_screen = Screen::Help;
    g_help_screen.onEnter();
    g_help_screen.draw();
}

void loop()
{
    M5.update();

    // --- Клавиатура ---------------------------------------------------------
    hw::KeyEvent ev;
    bool last_changed = false;

    while (g_keyboard.poll(ev)) {
        // Переключение раскладки обрабатывается на любом экране: драйвер уже
        // сменил её внутри poll(), а здесь мы её запоминаем. Раньше это делал
        // только диагностический экран, и из чата раскладка переключалась, но
        // в config.json не сохранялась — после перезагрузки возвращалась
        // латиница, хотя выбор пользователя должен жить дольше сеанса.
        if (ev.special == keymap::Special::LayoutToggle) {
            char err[96];
            g_settings.set_layout_russian(g_keyboard.layout() == keymap::Layout::Russian, err, sizeof(err));

            if (g_screen == Screen::Diagnostic) {
                g_seen[ev.row * keymap::kCols + ev.col] = true;
                drawCell(ev.row, ev.col, true);
                snprintf(g_last_text, sizeof(g_last_text), "Последняя: раскладка → %s", layoutName());
                // Метку пишем после переключения: драйвер уже сменил раскладку,
                // поэтому здесь верное значение, а не прежнее. Позицию указываем,
                // чтобы check_keylog.py засчитал клавишу как нажатую — это
                // Ctrl+Space, то есть физически Space.
                logLine("r%u c%u → Ctrl+Space, раскладка %s", ev.row, ev.col, layoutName());
                last_changed = true;
            }
            continue;
        }

        // Общие команды проверяются до маршрутизации: они работают на любом
        // экране. Ctrl+W — Wi-Fi, Ctrl+A — настройки API, Ctrl+D — диагностика.
        if (ev.ctrl && ev.type == hw::KeyEvent::Type::Char) {
            // Из редактора заметки уходить глобальными командами нельзя, пока
            // есть несохранённый текст: уход молча выбросил бы правку (onExit
            // её забывает), а пользователь ждал переключения экрана, а не
            // потери работы. Esc в редакторе спрашивает подтверждение — он и
            // остаётся способом выйти.
            //
            // Блокируются только команды СМЕНЫ ЭКРАНА. Проверять здесь любой
            // Ctrl нельзя: сюда же приходят Ctrl+S и Ctrl+Z, и редактор не
            // увидел бы их вовсе — сохранить заметку стало бы невозможно.
            const bool nav = strcmp(ev.utf8, "w") == 0 || strcmp(ev.utf8, "ц") == 0 || strcmp(ev.utf8, "a") == 0 ||
                             strcmp(ev.utf8, "ф") == 0 || strcmp(ev.utf8, "t") == 0 || strcmp(ev.utf8, "е") == 0 ||
                             strcmp(ev.utf8, "o") == 0 || strcmp(ev.utf8, "щ") == 0 || strcmp(ev.utf8, "d") == 0 ||
                             strcmp(ev.utf8, "в") == 0 || strcmp(ev.utf8, "m") == 0 || strcmp(ev.utf8, "ь") == 0;
            if (nav && g_screen == Screen::Notes && g_notes_screen.has_unsaved_edits()) {
                g_notes_screen.notice("Есть несохранённые правки — Ctrl+S сохранить · Esc выйти");
                continue;
            }

            if (strcmp(ev.utf8, "w") == 0 || strcmp(ev.utf8, "ц") == 0) {
                switch_screen(g_screen == Screen::Wifi ? Screen::Chat : Screen::Wifi);
                continue;
            }
            if (strcmp(ev.utf8, "a") == 0 || strcmp(ev.utf8, "ф") == 0) {
                switch_screen(g_screen == Screen::ApiSettings ? Screen::Chat : Screen::ApiSettings);
                continue;
            }
            // Ctrl+T — список тредов. «е» — та же клавиша в ЙЦУКЕН.
            if (strcmp(ev.utf8, "t") == 0 || strcmp(ev.utf8, "е") == 0) {
                switch_screen(g_screen == Screen::Threads ? Screen::Chat : Screen::Threads);
                continue;
            }
            // Ctrl+O — архив документов. «щ» — та же клавиша в ЙЦУКЕН.
            if (strcmp(ev.utf8, "o") == 0 || strcmp(ev.utf8, "щ") == 0) {
                switch_screen(g_screen == Screen::Notes ? Screen::Chat : Screen::Notes);
                continue;
            }
            if (strcmp(ev.utf8, "d") == 0 || strcmp(ev.utf8, "в") == 0) {
                switch_screen(g_screen == Screen::Diagnostic ? Screen::Chat : Screen::Diagnostic);
                continue;
            }
            // Ctrl+M — меню и справка. «ь» — та же клавиша в ЙЦУКЕН.
            //
            // Меню помнит, откуда его открыли: Esc возвращает туда. Уходить
            // всегда в чат значило бы, что из архива после справки документ
            // приходится открывать заново.
            if (strcmp(ev.utf8, "m") == 0 || strcmp(ev.utf8, "ь") == 0) {
                if (g_screen == Screen::Help) {
                    switch_screen(g_help_return);
                } else {
                    // Из диагностики тоже: меню — отдельный экран, проверке
                    // железа оно не мешает, а вернёт обратно туда же.
                    g_help_return = g_screen;
                    switch_screen(Screen::Help);
                }
                continue;
            }
        }

        // Меню обрабатывает клавиши само: стрелки, Enter и Esc. Ctrl+M разобран
        // выше — в списке глобальных команд, потому что он и открывает меню.
        if (g_screen == Screen::Help) {
            if (g_help_screen.onKey(ev)) switch_screen(g_help_return);
            continue;
        }

        // Экраны со своим состоянием обрабатывают события сами, включая Esc.
        if (g_screen == Screen::Chat) {
            if (g_chat_screen.onKey(ev)) switch_screen(Screen::Diagnostic);
            continue;
        }
        if (g_screen == Screen::Wifi) {
            if (g_wifi_screen.onKey(ev)) switch_screen(Screen::Chat);
            continue;
        }
        if (g_screen == Screen::ApiSettings) {
            if (g_api_screen.onKey(ev)) switch_screen(Screen::Chat);
            continue;
        }
        if (g_screen == Screen::Threads) {
            // onKey() не закрывает экран сам: Enter и повторный тап оставляют
            // запрос на открытие треда, а открытие делает main — только он
            // знает, что за ним следует переход в чат.
            g_threads_screen.set_chat_busy(g_chat_screen.busy());
            const bool want_close = g_threads_screen.onKey(ev);
            sync_after_thread_delete();
            if (g_threads_screen.take_new_request()) {
                // Новый тред создаётся только на простое: если ответ ещё идёт,
                // он писался бы в чужой файл. Сообщаем причину и остаёмся в
                // списке, а не уводим в чат с пустым результатом.
                if (g_chat_screen.busy()) {
                    g_threads_screen.notice("Дождитесь ответа или нажмите Esc");
                    continue;
                }
                g_chat_screen.new_thread();
                switch_screen(Screen::Chat);
                g_chat_screen.draw();
                continue;
            }
            if (open_thread_from_list()) continue;
            if (want_close) switch_screen(Screen::Chat);
            continue;
        }
        if (g_screen == Screen::Notes) {
            // Экран правит документ сам, но не переключает себя: Esc в списке
            // означает «в чат», и это решает main.
            const bool want_close = g_notes_screen.onKey(ev);
            if (want_close) switch_screen(Screen::Chat);
            continue;
        }

        // Ctrl+L - сбросить покрытие, чтобы пройти заново (диагностика).
        if (ev.ctrl && ev.type == hw::KeyEvent::Type::Char && strcmp(ev.utf8, "l") == 0) {
            memset(g_seen, 0, sizeof(g_seen));
            drawGrid();
            drawCoverage();
            drawMissed();
            continue;
        }

        if (ev.row >= keymap::kRows || ev.col >= keymap::kCols) continue;

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

    // --- Периодика экранов ---------------------------------------------------
    // tick() вызывается на любом экране: автоподключение к сохранённой сети
    // начинается ещё в setup(), а запрос к модели не должен замирать при
    // переключении экрана.
    g_wifi.tick();
    g_wifi_screen.tick();
    g_chat_screen.tick();
    g_api_screen.tick();
    hw::clock_sync::tick(g_wifi.connected());
    update_power();
    update_tray();
    update_leds();

    const auto& td = M5.Touch.getDetail();

    // --- Тап по трею ---------------------------------------------------------
    // Проверяется ДО маршрутизации по экранам: трей есть на всех, и его
    // области не совпадают с рабочими областями экранов (шапка у всех
    // неинтерактивна). Как в macOS: значок сети ведёт к настройкам сети,
    // значок батареи — к диагностике питания.
    //
    // Блокировка несохранённых правок действует и здесь: тап по трею — такая
    // же смена экрана, как Ctrl+W, и молча выбросить правку он не должен.
    if (td.wasReleased()) {
        const ui::Rect wifi_r = ui::wifi_rect(M5.Display.width());
        const ui::Rect batt_r = ui::battery_rect(M5.Display.width());
        const bool nav_blocked = g_screen == Screen::Notes && g_notes_screen.has_unsaved_edits();

        if (wifi_r.contains(td.x, td.y)) {
            if (nav_blocked) {
                g_notes_screen.notice("Есть несохранённые правки — Ctrl+S сохранить · Esc выйти");
            } else {
                switch_screen(g_screen == Screen::Wifi ? Screen::Chat : Screen::Wifi);
            }
            delay(10);
            return;
        }

        if (batt_r.contains(td.x, td.y)) {
            if (nav_blocked) {
                g_notes_screen.notice("Есть несохранённые правки — Ctrl+S сохранить · Esc выйти");
            } else {
                switch_screen(g_screen == Screen::Diagnostic ? Screen::Chat : Screen::Diagnostic);
            }
            delay(10);
            return;
        }
    }

    if (g_screen == Screen::Chat) {
        if (td.wasReleased()) g_chat_screen.onTouch(td.x, td.y);
        if (g_chat_screen.dirty()) g_chat_screen.draw();
        delay(10);
        return;
    }

    if (g_screen == Screen::Wifi) {
        if (td.wasReleased()) g_wifi_screen.onTouch(td.x, td.y);
        if (g_wifi_screen.dirty()) g_wifi_screen.draw();
        delay(10);
        return;
    }

    if (g_screen == Screen::ApiSettings) {
        if (td.wasReleased()) g_api_screen.onTouch(td.x, td.y);
        if (g_api_screen.dirty()) g_api_screen.draw();
        delay(10);
        return;
    }

    if (g_screen == Screen::Threads) {
        g_threads_screen.set_chat_busy(g_chat_screen.busy());
        if (td.wasReleased()) g_threads_screen.onTouch(td.x, td.y);
        open_thread_from_list();
        if (g_threads_screen.dirty()) g_threads_screen.draw();
        delay(10);
        return;
    }

    if (g_screen == Screen::Notes) {
        g_notes_screen.tick();
        if (td.wasReleased()) g_notes_screen.onTouch(td.x, td.y);
        if (g_notes_screen.dirty()) g_notes_screen.draw();
        delay(10);
        return;
    }

    if (g_screen == Screen::Help) {
        // Переход выполняет main: экран помощи не знает про остальные экраны.
        if (td.wasReleased()) g_help_screen.onTouch(td.x, td.y);

        const ui::HelpScreen::Target t = g_help_screen.take_target();
        if (t != ui::HelpScreen::Target::None) {
            switch (t) {
            case ui::HelpScreen::Target::Threads: switch_screen(Screen::Threads); break;
            case ui::HelpScreen::Target::Notes: switch_screen(Screen::Notes); break;
            case ui::HelpScreen::Target::ApiSettings: switch_screen(Screen::ApiSettings); break;
            case ui::HelpScreen::Target::Wifi: switch_screen(Screen::Wifi); break;
            case ui::HelpScreen::Target::Diagnostic: switch_screen(Screen::Diagnostic); break;
            case ui::HelpScreen::Target::None: break;
            }
            delay(10);
            return;
        }

        if (g_help_screen.dirty()) g_help_screen.draw();
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
