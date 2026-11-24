#pragma once

// Декодер Transfer-Encoding: chunked.
//
// Зачем свой: SSE-ответ RouterAI приходит с `Transfer-Encoding: chunked`
// (проверено на живом API по HTTP/1.1). `HTTPClient::getStreamPtr()` отдаёт
// сокет, но НЕ декодирует чанки — этим занимается только writeToStream(),
// которая копирует данные в поток-приёмник и не отдаёт их по частям. Если
// читать сокет напрямую, в поток попадут шестнадцатеричные длины чанков и
// лишние \r\n, и SSE-парсер увидит `1f\r\ndata: {...}` вместо `data: {...}`.
//
// Чистая логика: ни сокетов, ни Arduino — проверяется на хосте.

#include <stddef.h>

namespace http {

/// Кусок полезных данных после декодирования. Буфер действителен только на
/// время вызова.
using Sink = void (*)(const char* data, size_t len, void* ctx);

class ChunkedDecoder {
public:
    ChunkedDecoder(Sink sink, void* ctx) : _sink(sink), _ctx(ctx) {}

    /// Сброс перед новым потоком: декодер живёт дольше одного запроса.
    void reset();

    /// Скормить очередной кусок сырого потока.
    ///
    /// @return false, если читать дальше бессмысленно: поток завершён
    ///         терминальным чанком либо формат нарушен
    bool feed(const char* data, size_t len);

    /// Поток завершён терминальным чанком.
    bool done() const { return _state == State::Done; }

    /// Формат нарушен: пришло то, что не является заголовком чанка.
    bool broken() const { return _state == State::Error; }

    /// Сколько полезных байт выдано приёмнику.
    size_t payload_bytes() const { return _payload_bytes; }

    /// Сколько чанков разобрано (не считая терминального).
    unsigned chunks() const { return _chunks; }

private:
    enum class State : unsigned char {
        /// Читаем шестнадцатеричную длину чанка.
        Size,
        /// Расширение чанка после `;` — до конца строки.
        Extension,
        /// Ожидаем \n после \r в строке длины.
        SizeLf,
        /// Читаем данные чанка.
        Data,
        /// Ожидаем \r после данных.
        DataCr,
        /// Ожидаем \n после данных.
        DataLf,
        /// Читаем трейлеры после терминального чанка.
        Trailer,
        /// Ожидаем \n после \r в трейлере.
        TrailerLf,
        Done,
        Error,
    };

    void handle_byte(char ch);

    Sink _sink{nullptr};
    void* _ctx{nullptr};

    State _state{State::Size};

    /// Накопленная длина текущего чанка.
    unsigned long _size{0};
    /// Сколько байт чанка ещё осталось отдать.
    unsigned long _remaining{0};
    /// В длине уже была хоть одна цифра: строка без цифр — ошибка.
    bool _size_digits{false};
    /// Текущая строка трейлера пуста (значит, это финальный \r\n).
    bool _trailer_line_empty{true};

    size_t _payload_bytes{0};
    unsigned _chunks{0};
};

}  // namespace http
