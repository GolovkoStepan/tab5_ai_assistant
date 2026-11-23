#pragma once

// Чистые функции форматирования — без зависимостей от Arduino.
// Используются и в прошивке, и в юнит-тестах на хосте.

#include <stddef.h>

/// Форматирует размер в человекочитаемый вид: "512 Б", "1.5 КБ", "3.0 МБ", "4.0 ГБ".
/// @param bytes   размер в байтах
/// @param out     буфер для результата
/// @param out_len размер буфера, рекомендуется не меньше 16
void format_bytes(unsigned long long bytes, char* out, size_t out_len);

/// Форматирует счётчик токенов: "480", "12.3k", "1.2M".
/// @param tokens  количество токенов
/// @param out     буфер для результата
/// @param out_len размер буфера, рекомендуется не меньше 16
void format_tokens(unsigned long tokens, char* out, size_t out_len);

/// Обрезает текст до заданной ширины в символах (не байтах), добавляя «…».
/// UTF-8 не разрезается посередине последовательности.
/// @param text    исходный текст
/// @param max_chars максимальное число символов результата, включая многоточие
/// @param out     буфер для результата
/// @param out_len размер буфера
void truncate_text(const char* text, size_t max_chars, char* out, size_t out_len);

/// Разбивает текст на строки по ширине в символах, не разрывая слова.
/// Возвращает число строк и заполняет offsets[] байтовыми смещениями начал строк.
/// @param text    исходный текст
/// @param width   максимальная ширина строки в символах
/// @param offsets массив для смещений начал строк
/// @param max_lines максимальное число строк
/// @return фактическое число строк (не больше max_lines)
size_t wrap_text(const char* text, size_t width, size_t* offsets, size_t max_lines);
