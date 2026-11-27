// Системный трей: Wi-Fi и батарея в правом верхнем углу.
//
// Значки — примитивы, текст — из шрифта. Подробности и почему именно так —
// в include/ui/tray.h.

#include "ui/tray.h"

#include <M5Unified.h>

#include "ui/draw.h"

namespace ui {
namespace {

// --- Геометрия ---------------------------------------------------------------
//
// Столбцы фиксированные, а не «прижатые к соседу»: подпись батареи меняется
// между «87 %», «заряд» и «8.37 В», и если бы от её ширины зависело положение
// группы, та дёргалась бы каждую секунду.
//
// Порядок справа налево: часы, палочки Wi-Fi с подписью, батарея с подписью.
// Часы стоят у самого края по просьбе владельца: время ищут там же, где оно
// живёт в macOS и на телефоне, — в правом верхнем углу. Всё остальное
// сдвинулось левее на их ширину.

/// Ширина корпуса батареи (без выступа).
constexpr int kBatteryBodyW = 22;
constexpr int kBatteryBodyH = 14;
/// Выступ-клемма справа.
constexpr int kBatteryNubW = 3;
constexpr int kBatteryNubH = 6;
/// Общая ширина значка батареи вместе с выступом.
constexpr int kBatteryIconW = kBatteryBodyW + kBatteryNubW;

/// Место под подпись батареи: «100 %» = 40 px, «8.37 В» = 48, «заряжено» = 64.
/// Меряем самую длинную из возможных: иначе «заряжено» наезжало бы на значок
/// Wi-Fi, а сдвигать группу под каждую смену подписи — дёргать весь трей.
constexpr int kBatteryTextW = 66;

/// Место под подпись Wi-Fi. Адреса здесь больше нет: при связи подпись пустая,
/// а «нет» = 24 px и «…» = 8 px. Место всё равно оставляем — иначе палочки
/// съезжали бы вправо при каждом подключении, а вместе с ними и точка
/// перерисовки.
///
/// Ровно 24, а не «с запасом»: слот невидим, когда связи нет — а тогда он
/// читается как пустое место между батареей и палочками. Измерено по таблице
/// глифов `efontJA_16`: «нет» — самая длинная из возможных подписей.
constexpr int kWifiTextW = 24;

/// Палочки сигнала.
constexpr int kWifiBarW = 3;
constexpr int kWifiBarStep = 4;
constexpr int kWifiIconW = kWifiBarStep * 4 - (kWifiBarStep - kWifiBarW);  // 15
constexpr int kWifiBarH = 12;

/// Зазор между текстом и значком.
constexpr int kTextPad = 5;
/// Зазор между группами батареи и Wi-Fi.
///
/// Именно он становится видимым расстоянием до подписи «нет», когда связи нет:
/// подпись прижата к палочкам справа, и батарея кончается ровно за этот зазор
/// до неё. Четыре пикселя — минимум, при котором группы ещё читаются как
/// отдельные, а не слипаются в одно пятно.
constexpr int kGroupGap = 4;

/// Слот под часы. Ширина измерена по таблице глифов `efontJA_16`, а не
/// прикинута на глаз: худшая строка «Чт, 28 сент. 23:59» занимает 144 px
/// (скрипт `scripts/font_probe.py`). Слот фиксированный, как и у подписи
/// батареи: иначе при смене месяца и переходе «9:05» → «10:05» значок Wi-Fi
/// съезжал бы вправо-влево, а вместе с ним и точка перерисовки.
constexpr int kClockW = 146;

/// Зазор между часами и значком Wi-Fi. Больше `kGroupGap`: часы — отдельная
/// сущность (время), а Wi-Fi и батарея сообщают об одном состоянии
/// устройства, и им положено стоять плотнее.
constexpr int kClockGap = 10;

/// Вертикаль текста: шрифт 16 px в полосе 44 px.
constexpr int kTextY = (kTrayH - 16) / 2;

// Цвета. Те же, что в ленте чата: зелёный — «всё в порядке», оранжевый —
// «внимание», тёмно-серый — «неизвестно/нет».
constexpr uint16_t kColorOk = 0x07E0;
constexpr uint16_t kColorWarn = TFT_ORANGE;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorText = 0xD69A;
/// Синий заряда — тот же, что у правого светодиода клавиатуры (Р-024): одно
/// состояние не должно иметь двух разных цветов на одном устройстве.
constexpr uint16_t kColorCharge = 0x2D7F;

/// Левый край слота часов. Слот фиксированной ширины, прижат к правому краю.
int clock_x(int screen_w)
{
    return screen_w - kTrayMargin - kClockW;
}

int wifi_icon_x(int screen_w)
{
    // Палочки — сразу левее часов: у самого правого края теперь время, а
    // значок связи идёт следующим, как в macOS. Подпись к нему прижата слева,
    // как у батареи. Обе группы читаются одинаково: сначала текст, потом
    // значок.
    return clock_x(screen_w) - kClockGap - kWifiIconW;
}

int wifi_text_right(int screen_w)
{
    return wifi_icon_x(screen_w) - kTextPad;
}

int battery_body_x(int screen_w)
{
    return wifi_text_right(screen_w) - kWifiTextW - kGroupGap - kBatteryIconW;
}

int battery_text_right(int screen_w)
{
    return battery_body_x(screen_w) - kTextPad;
}

/// Четыре палочки. Заполненные — цветом состояния, остальные — тёмной рамкой:
/// шкала должна быть видна целиком, иначе «одна палочка» и «четыре» выглядят
/// как значки разной формы.
void draw_wifi_bars(int x, int y, uint8_t level, bool connected, bool connecting)
{
    uint16_t on = kColorFaint;
    if (connected) {
        on = kColorOk;
    } else if (connecting) {
        on = kColorWarn;
    }

    for (uint8_t i = 0; i < 4; ++i) {
        const int h = 3 + i * 3;
        const int bx = x + i * kWifiBarStep;
        const int by = y + kWifiBarH - h;
        M5.Display.fillRect(bx, by, kWifiBarW, h, i < level ? on : 0x18E3);
    }
}

/// Корпус батареи с заполнением.
///
/// @param fill     0..100; -1 — процент недостоверен (рисуется пустой корпус)
/// @param charging идёт заряд: заполнение синее и полное, а не по проценту
void draw_battery(int x, int y, int fill, bool charging, bool low)
{
    uint16_t body = kColorText;
    uint16_t level = kColorOk;
    if (charging) {
        level = kColorCharge;
    } else if (low) {
        level = kColorWarn;
    }

    // Корпус: заливаем фоном, чтобы стереть прошлое заполнение, затем рамка.
    M5.Display.fillRect(x, y, kBatteryIconW, kBatteryBodyH, TFT_BLACK);
    M5.Display.drawRoundRect(x, y, kBatteryBodyW, kBatteryBodyH, 3, body);

    // Клемма.
    M5.Display.fillRect(x + kBatteryBodyW, y + (kBatteryBodyH - kBatteryNubH) / 2, kBatteryNubW, kBatteryNubH, body);

    const int inset = 3;
    const int inner_w = kBatteryBodyW - inset * 2;
    const int inner_h = kBatteryBodyH - inset * 2;

    if (charging) {
        // При заряде процента нет, но пустой корпус читался бы как «ноль».
        // Заполняем целиком: так значок говорит «заряд идёт» и не притворяется,
        // что знает уровень.
        M5.Display.fillRect(x + inset, y + inset, inner_w, inner_h, level);
        return;
    }

    if (fill < 0) {
        // Процента нет и заряд не идёт: аккумулятора нет либо его заряд
        // неизвестен. Пустой корпус, рядом — напряжение.
        return;
    }

    int w = inner_w * fill / 100;
    if (w < 1 && fill > 0) w = 1;  // «1 %» не должен выглядеть как пустой
    if (w > 0) M5.Display.fillRect(x + inset, y + inset, w, inner_h, level);
}
}  // namespace

/// Снимок состояния для шапок экранов. Один на всю прошивку: цикл один,
/// поток один, гонок нет.
namespace {
tray::Info g_live;
}

const tray::Info& live_info()
{
    return g_live;
}

void set_live_info(const tray::Info& info)
{
    g_live = info;
}

int tray_left_edge(int screen_w)
{
    // 8 px запаса: вплотную к подписи батареи заголовок выглядит слипшимся.
    // Считается от левого края самой левой группы — теперь это батарея.
    return battery_text_right(screen_w) - kBatteryTextW - 8;
}

Rect wifi_rect(int screen_w)
{
    Rect r;
    // Значок и его подпись слева: подпись прижата к палочкам, а не к краю.
    // Правый край — конец палочек, а не край экрана: правее живут часы, и их
    // тап не должен открывать Wi-Fi.
    r.x = wifi_text_right(screen_w) - kWifiTextW;
    r.y = 0;
    r.w = wifi_icon_x(screen_w) + kWifiIconW - r.x;
    r.h = kTrayH;
    return r;
}

Rect battery_rect(int screen_w)
{
    Rect r;
    r.x = battery_text_right(screen_w) - kBatteryTextW;
    r.y = 0;
    r.w = wifi_text_right(screen_w) - kWifiTextW - kGroupGap - r.x;
    r.h = kTrayH;
    return r;
}

void clear_tray(int screen_w)
{
    // Только своя область: левее живёт заголовок экрана, и заливка на всю
    // ширину стирала бы его при каждом обновлении показаний.
    const int x = tray_left_edge(screen_w);
    M5.Display.fillRect(x, 0, screen_w - x, kTrayH, TFT_BLACK);
}

void draw_tray(const tray::Info& info, int screen_w)
{
    if (screen_w <= 0) return;

    // Стираем свою область целиком: подписи меняются («87 %» → «заряд»,
    // пусто → «нет»), и без заливки от прежней, более длинной строки остался
    // бы хвост.
    clear_tray(screen_w);

    // --- Часы ---
    // Прижаты к правому краю. Пустая строка, пока время не выставлено:
    // выдуманную дату не рисуем — см. include/util/tray_state.h.
    if (info.clock_text[0] != '\0') {
        ui::text_right(info.clock_text, screen_w - kTrayMargin, kTextY, kColorText);
    }

    // --- Wi-Fi ---
    const int wifi_y = (kTrayH - kWifiBarH) / 2;
    draw_wifi_bars(wifi_icon_x(screen_w), wifi_y, info.wifi_level, info.wifi_connected, info.wifi_connecting);

    if (info.wifi_text[0] != '\0') {
        uint16_t color = kColorFaint;
        if (info.wifi_connecting) color = kColorWarn;
        // Прижата к палочкам слева, а не к краю экрана: «нет» короткое, и у
        // края оно отъехало бы от значка — группа распалась бы на два куска.
        ui::text_right(info.wifi_text, wifi_text_right(screen_w), kTextY, color);
    }

    // --- Батарея ---
    const int body_y = (kTrayH - kBatteryBodyH) / 2;
    const bool charging = info.charge == tray::Charge::Charging;
    draw_battery(battery_body_x(screen_w), body_y,
        tray::battery_fill(info.charge, info.battery_known, info.battery_percent), charging,
        tray::battery_low(info.battery_known, info.battery_percent));

    uint16_t battery_color = kColorText;
    if (charging) {
        battery_color = kColorCharge;
    } else if (tray::battery_low(info.battery_known, info.battery_percent)) {
        battery_color = kColorWarn;
    }
    ui::text_right(info.battery_text, battery_text_right(screen_w), kTextY, battery_color);
}

void draw_tray(int screen_w)
{
    draw_tray(g_live, screen_w);
}

}  // namespace ui
