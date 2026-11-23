#pragma once

// Глобальные константы и настройки по умолчанию.
// Пользовательские значения (Wi-Fi, ключи API) хранятся в NVS, а не здесь.

#include <stdint.h>

namespace cfg {

/// Пути внутри SD-карты. Все имена файлов — ASCII-слаги, заголовки хранятся
/// во фронтматтере внутри файла (см. docs/ARCHITECTURE.md).
///
/// Внимание: путь указан **относительно точки монтирования**, без префикса.
/// `SD_MMC.begin("/sdcard")` монтирует файловую систему в `/sdcard`, поэтому
/// `SD_MMC.open("/config.json")` открывает `/sdcard/config.json`. Если написать
/// полный путь, получится `/sdcard/sdcard/...` и fopen упадёт — проверено на
/// живой плате.
constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kNotesDir = "/notes";
constexpr const char* kChatsDir = "/chats";
constexpr const char* kConfigPath = "/config.json";

/// Железо: клавиатура A164 — отдельная I2C-шина, не пересекается с G31/G32.
constexpr int8_t kKeyboardSda = 0;
constexpr int8_t kKeyboardScl = 1;
constexpr int8_t kKeyboardInt = 50;
constexpr uint8_t kKeyboardAddr = 0x6D;

/// Пороги предупреждения о разряде, проценты.
constexpr int kBatteryWarnPercent = 20;
constexpr int kBatteryCriticalPercent = 10;
constexpr int kBatteryShutdownPercent = 5;

/// Ограничения агентского цикла.
constexpr int kMaxAgentSteps = 8;
constexpr size_t kMaxToolResultBytes = 4096;

}  // namespace cfg
