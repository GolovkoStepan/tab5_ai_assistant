// Заставка на время запуска: заголовок, полоса прогресса и список шагов.
//
// Подробности и зачем это нужно — в include/ui/splash.h. Коротко: экран не
// должен оставаться чёрным, пока идёт инициализация, и показывать он должен
// настоящие шаги, а не выдуманный таймер.

#include "ui/splash.h"

#include <M5Unified.h>

#include <stddef.h>
#include <stdio.h>

#include "ui/draw.h"
#include "version.h"

namespace ui {
namespace {

// --- Геометрия ---------------------------------------------------------------
//
// Всё привязано к центру по горизонтали: экран 1280×720, и заставка на нём
// должна выглядеть одинаково с обеих сторон, без «поехавшей» середины.

constexpr int kTitleY = 168;
/// Заголовок набран размером 4.0 от `efontJA_24` (глиф 14 px), то есть занимает
/// 168..224. Подпись отодвинута от него на 52 px: вплотную две строки разного
/// кегля читались как одна «плашка» и название терялось. Разделительной линии
/// между ними нет — отступ справляется сам, а лишний декор на заставке не
/// сообщает ничего (и в 1 px на 720 он выглядел случайной царапиной).
constexpr int kSubtitleY = 276;
constexpr int kBarY = 320;
constexpr int kBarW = 480;
constexpr int kBarH = 4;
constexpr int kStepsY = 372;
constexpr int kStepRowH = 30;
constexpr int kStepW = 420;

/// Версия — в правом нижнем углу, мелко и серым. Место выбрано так, чтобы не
/// спорить с содержимым: шаги и пояснение к ним занимают центр и низ до ~570,
/// а угол остаётся пустым. Крупно её показывать незачем — на заставке она
/// отвечает на вопрос «что за сборка?», а не служит заголовком.
constexpr int kVersionMargin = 24;

/// Крупный шрифт для названия. В efontJA_24 есть кириллица и цифры (проверено
/// разбором таблицы глифов), поэтому латиница и русский заголовок набраны
/// одним шрифтом, без смешивания начертаний.
constexpr const lgfx::IFont* kBigFont = &lgfx::fonts::efontJA_24;

constexpr uint16_t kColorTitle = TFT_WHITE;
constexpr uint16_t kColorDim = 0x8410;     // серый: подписи шагов
constexpr uint16_t kColorFaint = 0x4208;   // тёмно-серый: ожидающие шаги
constexpr uint16_t kColorAccent = 0x2D7F;  // синий: активный шаг и полоса
constexpr uint16_t kColorDone = 0x07E0;    // зелёный: выполненный шаг

/// Подписи шагов. Порядок совпадает с enum SplashStep — проверяется ниже.
constexpr const char* kStepNames[kSplashSteps] = {
    "Экран и поворот",
    "Карта памяти",
    "Настройки",
    "Клавиатура",
    "Сеть",
    "Готово",
};

static_assert(
    sizeof(kStepNames) / sizeof(kStepNames[0]) == kSplashSteps, "подписей шагов должно быть столько же, сколько шагов");

/// Нарисованный шаг: чтобы не перерисовывать уже готовые строки каждый раз.
int g_drawn_step = -1;

/// Строка с заданным размером шрифта и выравниванием по центру.
///
/// Размер шрифта возвращается к 1 обязательно: `ui::text` и трей на других
/// экранах рассчитывают на обычный кегль, а стиль M5GFX глобальный.
void draw_center(
    const char* text, int center_x, int y, uint16_t color, float size = 1.0f, const lgfx::IFont* font = nullptr)
{
    M5.Display.setFont(font ? font : ui::font());
    M5.Display.setTextDatum(textdatum_t::top_center);
    M5.Display.setTextColor(color, TFT_BLACK);
    M5.Display.setTextSize(size);
    M5.Display.drawString(text, center_x, y);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(textdatum_t::top_left);
}

/// Значок состояния шага. `●` рисуется, `✓` есть только в 16 px — поэтому
/// готовность показывается галочкой, а ожидание — пустым кружком.
void draw_step_marker(int x, int y, int step, int current)
{
    const char* mark = "○";
    uint16_t color = kColorFaint;
    if (step < current) {
        mark = "✓";
        color = kColorDone;
    } else if (step == current) {
        mark = "●";
        color = kColorAccent;
    }
    ui::text(mark, x, y, color);
}

/// Строка шага: значок, номер и подпись.
void draw_step_row(int screen_w, int step, int current)
{
    const int row_y = kStepsY + step * kStepRowH;
    const int x = (screen_w - kStepW) / 2;

    // Стираем строку целиком: цвет и значок меняются, иначе остался бы хвост
    // прежнего состояния.
    M5.Display.fillRect(x, row_y, kStepW, kStepRowH, TFT_BLACK);

    draw_step_marker(x, row_y + 1, step, current);

    const bool done = step < current;
    const bool active = step == current;
    const uint16_t color = done ? kColorDim : (active ? TFT_WHITE : kColorFaint);
    ui::text(kStepNames[step], x + 28, row_y + 1, color);
}

/// Полоса прогресса: дорожка и заполнение по числу пройденных шагов.
void draw_progress(int screen_w, int done_steps)
{
    const int x = (screen_w - kBarW) / 2;
    M5.Display.fillRect(x, kBarY, kBarW, kBarH, kColorFaint);

    if (done_steps <= 0) return;
    int w = kBarW * done_steps / kSplashSteps;
    if (w < 2) w = 2;  // первый шаг должен быть виден, а не нулевой ширины
    M5.Display.fillRect(x, kBarY, w, kBarH, kColorAccent);
}

}  // namespace

void splash_begin(int screen_w, int screen_h)
{
    const int w = screen_w;
    const int h = screen_h;

    M5.Display.fillScreen(TFT_BLACK);

    // Название. Крупный кегль — единственное место в прошивке, где он нужен:
    // заставка это витрина, а не рабочий экран.
    draw_center("TAB5", w / 2, kTitleY, kColorTitle, 4.0f, kBigFont);
    draw_center("карманный ИИ-помощник", w / 2, kSubtitleY, kColorDim, 1.0f);

    // Версия в правом нижнем углу. Она нужна не для красоты: когда сборок
    // много, «та ли прошивка на плате?» проверяется глазами за секунду, а не
    // через Serial и `version`. Формат «v0.5.0» — с буквой, чтобы не путалось
    // с числом на экране.
    char version[24];
    snprintf(version, sizeof(version), "v%s", APP_VERSION);
    ui::text_right(version, w - kVersionMargin, h - kVersionMargin - 16, kColorFaint);

    draw_progress(w, 0);

    g_drawn_step = -1;
    // Первый шаг сразу активен: экран уже настроен в этот момент, но строка
    // «готово» появится только после splash_step(Display).
    draw_step_row(w, 0, 0);
    g_drawn_step = 0;
}

void splash_step(SplashStep step, const char* note)
{
    const int w = M5.Display.width();
    const int idx = static_cast<int>(step);
    if (idx < 0 || idx >= kSplashSteps) return;

    // Перерисовываем предыдущую строку как выполненную и текущую как активную.
    // Достаточно двух строк: остальные уже нарисованы в нужном состоянии.
    if (g_drawn_step >= 0 && g_drawn_step < kSplashSteps && g_drawn_step != idx) {
        draw_step_row(w, g_drawn_step, idx);
    }
    draw_step_row(w, idx, idx);

    draw_progress(w, idx);

    // Пояснение к шагу — там, где оно есть (например, имя SD или модель).
    // Рисуем под списком, а не рядом со строкой: подписи шагов выровнены по
    // левому краю, и приписка справа ломала бы колонку.
    if (note != nullptr && note[0] != '\0') {
        const int note_y = kStepsY + kSplashSteps * kStepRowH + 8;
        M5.Display.fillRect(0, note_y, w, 24, TFT_BLACK);
        draw_center(note, w / 2, note_y, kColorDim);
    }

    g_drawn_step = idx;
}

void splash_finish(uint32_t hold_ms)
{
    const int w = M5.Display.width();

    splash_step(SplashStep::Ready);
    draw_progress(w, kSplashSteps);

    // Пауза нужна только чтобы «готово» успели прочитать: без неё экран
    // сменился бы через миллисекунды и выглядел бы как мигание.
    if (hold_ms > 0) delay(hold_ms);
}

}  // namespace ui
