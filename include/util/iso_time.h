#pragma once

// Время сообщения в ISO-8601 и разбор таймзоны из настроек.
//
// В config.json таймзона хранится в формате POSIX ("MSK-3"), а в файле треда
// нужно время с явным смещением ("2026-11-24T12:00:00+03:00"). Без смещения
// тред, открытый через год в другом часовом поясе, показал бы неверное время.
//
// Чистые функции без Arduino: часы читает вызывающий, здесь только текст.

#include <stddef.h>

namespace isotime {

/// Разбирает смещение из строки POSIX-таймзоны.
///
/// POSIX считает знак наоборот: "MSK-3" — это UTC+3, а "UTC+5" — UTC-5.
/// Формат: необязательное имя (буквы, цифры, `<...>`), затем число со знаком.
///
/// @param tz            строка из настроек, например "MSK-3"
/// @param out_minutes   смещение в минутах от UTC
/// @return false, если смещения в строке нет или оно вне диапазона ±14 часов
bool parse_tz_offset(const char* tz, int* out_minutes);

/// Собирает время в ISO-8601 с минутами и смещением.
///
/// @param out       буфер (хватает 32 байт)
/// @param out_len   его размер
/// @param year      год, например 2026
/// @param month     1..12
/// @param day       1..31
/// @param hour      0..23
/// @param minute    0..59
/// @param second    0..59
/// @param tz_minutes смещение от UTC в минутах (например, 180 для MSK)
/// @return число байт без завершающего нуля, 0 если поля вне диапазона или
///         буфер мал
size_t format_iso8601(
    char* out, size_t out_len, int year, int month, int day, int hour, int minute, int second, int tz_minutes);

/// Проверяет, что поля даты и времени осмысленны.
///
/// Нужно перед записью: RTC без батарейки отдаёт мусор или -1, и «1970-00-00»
/// в файле треда хуже, чем отсутствие времени.
bool is_valid(int year, int month, int day, int hour, int minute, int second);

}  // namespace isotime
