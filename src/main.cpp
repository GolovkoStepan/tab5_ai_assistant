/*
 * M5Stack TAB5 AI Assistant — точка входа прошивки.
 *
 * Этап v0.1.0 (bring-up). Задача — проверить на живой плате:
 *   - платформа собирается и стартует;
 *   - дисплей и тач инициализируются, поворот правильный;
 *   - кириллица рисуется без «квадратиков»;
 *   - клавиатура A164 отвечает и печатает, включая русскую раскладку;
 *   - батарея читается через INA226;
 *   - SD-карта монтируется;
 *   - Wi-Fi через ESP32-C6 поднимается.
 *
 * Всё, что проверено, выводится на экран и дублируется в Serial — чтобы
 * результаты можно было зафиксировать в docs/HARDWARE_VERIFICATION.md.
 */

#include <M5Unified.h>
#include <SD_MMC.h>
#include <WiFi.h>

#include <string.h>

#include "app_config.h"
#include "hw/keyboard.h"
#include "util/format.h"
#include "version.h"

namespace {

/// Шрифт с кириллицей: полный русский алфавит + «» № …
const lgfx::IFont* kUiFont = &lgfx::fonts::lgfxJapanGothic_16;

constexpr int kLeft = 20;
constexpr int kLineHeight = 26;
constexpr int kDiagTop = 106;
constexpr int kTypingTop = 480;

int g_line = 0;

/// Рисует строку диагностики на экране и дублирует в Serial.
void report(const char* fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    Serial.println(buf);

    M5.Display.setFont(kUiFont);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(buf, kLeft, kDiagTop + g_line * kLineHeight);
    ++g_line;
}

/// Проверка батареи через INA226.
void checkPower()
{
    const int millivolts = M5.Power.getBatteryVoltage();
    const int level = M5.Power.getBatteryLevel();
    const int current = M5.Power.getBatteryCurrent();

    // Наличие батареи — эвристика: отдельного getBatteryPresence() у M5Unified нет.
    // Измерено на этой плате: без аккумулятора INA226 показывает 4.34 В (USB),
    // с заряженным — 8.06 В. Порог лежит между ними с большим запасом.
    const bool has_battery = millivolts > cfg::kBatteryPresentMillivolts;

    report("Батарея: %s %.2f В, %d %%, %d мА", has_battery ? "есть," : "НЕТ,", millivolts / 1000.0f, level, current);
    report("Зарядка: %s", M5.Power.isCharging() ? "идёт" : "нет");
}

/// Проверка SD-карты. Пины (43/44/39..42) подставляет SD_MMC из варианта платы.
void checkSdCard()
{
    // Вариант платы m5stack_tab5 не задаёт BOARD_SDMMC_POWER_CHANNEL, поэтому
    // Arduino не включает LDO4 — питание карты. Без этого mount падает
    // в send_op_cond timeout (0x107). esp-bsp делает это явно.
    SD_MMC.setPowerChannel(4);

    if (!SD_MMC.begin(cfg::kMountPoint, false)) {
        report("SD: НЕ смонтирована");
        return;
    }

    if (SD_MMC.cardType() == CARD_NONE) {
        report("SD: карта не найдена");
        return;
    }

    char sz_total[16];
    char sz_used[16];
    format_bytes(SD_MMC.totalBytes(), sz_total, sizeof(sz_total));
    format_bytes(SD_MMC.usedBytes(), sz_used, sizeof(sz_used));
    report("SD: %s всего, %s занято", sz_total, sz_used);

    // Монтирование без записи ничего не доказывает — проверяем запись.
    File probe = SD_MMC.open("/.probe", FILE_WRITE);
    if (!probe) {
        report("SD: запись НЕ работает");
        return;
    }
    probe.print("ok");
    probe.close();
    SD_MMC.remove("/.probe");
    report("SD: запись работает");
}

/// Проверка Wi-Fi через ESP32-C6. Питание C6 поднимает M5.begin().
void checkWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    const int found = WiFi.scanNetworks();
    if (found < 0) {
        report("Wi-Fi: ошибка сканирования (%d)", found);
        return;
    }

    const String mac = WiFi.macAddress();
    report("Wi-Fi: OK, сетей найдено %d", found);
    report("MAC Wi-Fi (C6): %s", mac.c_str());

    WiFi.scanDelete();
}

}  // namespace

hw::Keyboard g_keyboard;

void setup()
{
    auto m5cfg = M5.config();
    M5.begin(m5cfg);

    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("=== TAB5 AI Assistant " APP_VERSION " ===");

    // Панель физически 720x1280 (портрет), поэтому ландшафт — это поворот
    // 1 или 3, и они отличаются на 180 градусов. Поворот 1 даёт картинку
    // вверх ногами, правильный — 3.
    M5.Display.setRotation(3);
    M5.Display.setBrightness(128);
    M5.Display.fillScreen(TFT_BLACK);

    // Заголовок и тестовая строка с кириллицей целиком: строчные, заглавные,
    // Ё, типографские кавычки, тире, знак номера.
    M5.Display.setFont(&lgfx::fonts::lgfxJapanGothic_24);
    M5.Display.setTextDatum(textdatum_t::top_left);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("TAB5 AI Assistant " APP_VERSION, kLeft, 16);

    M5.Display.setFont(kUiFont);
    M5.Display.drawString("Съешь же ещё этих мягких булок — «Ёж» №5…", kLeft, 52);

    report("Экран: %dx%d, поворот %d", M5.Display.width(), M5.Display.height(), M5.Display.getRotation());
    report("PSRAM: %u КБ, flash: %u МБ", (unsigned)(ESP.getPsramSize() / 1024),
        (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    report("Свободно: heap %u КБ, PSRAM %u КБ", (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(ESP.getFreePsram() / 1024));
    report("CPU: %u МГц", (unsigned)getCpuFrequencyMhz());

    checkPower();
    checkSdCard();

    if (g_keyboard.begin()) {
        report("Клавиатура: OK, прошивка v%u", g_keyboard.firmwareVersion());
    } else {
        report("Клавиатура: НЕ отвечает на адресе 0x%02X", cfg::kKeyboardAddr);
    }

    checkWifi();

    M5.Display.drawFastHLine(kLeft, kTypingTop - 12, M5.Display.width() - 2 * kLeft, TFT_DARKGRAY);
    M5.Display.setFont(kUiFont);
    M5.Display.drawString("Печатайте — Ctrl+Space меняет раскладку, Ctrl+L очищает.", kLeft, kTypingTop - 34);
}

void loop()
{
    M5.update();

    static char typed[512] = {};
    static size_t typed_len = 0;
    static keymap::Layout shown_layout = keymap::Layout::Latin;
    bool dirty = false;

    hw::KeyEvent ev;
    while (g_keyboard.poll(ev)) {
        // Ctrl+L — очистить поле ввода.
        if (ev.ctrl && ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] == 'l') {
            typed_len = 0;
            typed[0] = '\0';
            dirty = true;
            continue;
        }

        if (ev.type == hw::KeyEvent::Type::Char) {
            const size_t n = strlen(ev.utf8);
            if (typed_len + n < sizeof(typed) - 1) {
                memcpy(typed + typed_len, ev.utf8, n);
                typed_len += n;
                typed[typed_len] = '\0';
            }
            dirty = true;
        } else if (ev.special == keymap::Special::Backspace) {
            if (typed_len > 0) {
                // Отступаем на один UTF-8 символ назад.
                --typed_len;
                while (typed_len > 0 && (typed[typed_len] & 0xC0) == 0x80)
                    --typed_len;
                typed[typed_len] = '\0';
            }
            dirty = true;
        } else if (ev.special == keymap::Special::Enter) {
            typed_len = 0;
            typed[0] = '\0';
            dirty = true;
        }
    }

    if (dirty) {
        M5.Display.fillRect(kLeft, kTypingTop, M5.Display.width() - 2 * kLeft, 200, TFT_BLACK);
        M5.Display.setFont(kUiFont);
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        // Переносим по ширине экрана, чтобы длинная строка была видна целиком.
        constexpr size_t kMaxLines = 6;
        size_t offsets[kMaxLines];
        const int width_chars = (M5.Display.width() - 2 * kLeft) / 16;
        const size_t lines = wrap_text(typed, width_chars, offsets, kMaxLines);
        for (size_t i = 0; i < lines; ++i) {
            const size_t end = (i + 1 < lines) ? offsets[i + 1] : strlen(typed);
            const size_t len = end > offsets[i] ? end - offsets[i] : 0;
            char line_buf[160];
            const size_t copy = len < sizeof(line_buf) - 1 ? len : sizeof(line_buf) - 1;
            memcpy(line_buf, typed + offsets[i], copy);
            line_buf[copy] = '\0';
            M5.Display.drawString(line_buf, kLeft, kTypingTop + i * kLineHeight);
        }
    }

    // Индикатор раскладки — перерисовываем только при смене.
    if (g_keyboard.layout() != shown_layout) {
        shown_layout = g_keyboard.layout();
        M5.Display.fillRect(M5.Display.width() - 200, kTypingTop - 40, 190, 24, TFT_BLACK);
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.drawString(shown_layout == keymap::Layout::Russian ? "раскладка: РУС" : "раскладка: ENG",
            M5.Display.width() - 200, kTypingTop - 40);
    }

    delay(10);
}
