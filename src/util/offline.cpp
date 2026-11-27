// Правило показа баннера офлайн-режима — без железа.
//
// Подробности — в include/util/offline.h.

#include "util/offline.h"

namespace offline {

bool banner_visible(bool connected, bool connecting, bool has_message)
{
    if (connected) return false;
    // Подключение идёт — это не «нет сети», а «сеть появляется». Обещать
    // офлайн в эти секунды значит соврать там, где человек как раз ждёт.
    if (connecting) return false;
    // В полосе уже есть сообщение (ошибка запроса, подсказка) — оно важнее:
    // две строки в одном месте спорили бы друг с другом.
    if (has_message) return false;
    return true;
}

const char* banner_text()
{
    // Перечислено, что доступно: архив и история целиком на карте, и это
    // ровно то, ради чего устройство берут без интернета — найти записанное
    // раньше. Про «нет сети» сказано в конце, чтобы первой строкой шло
    // действие, а не проблема.
    return "Без сети: доступны архив (Ctrl+O) и история (Ctrl+T) · Ctrl+W — подключиться";
}

}  // namespace offline
