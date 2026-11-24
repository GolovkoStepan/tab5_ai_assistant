// Декодер Transfer-Encoding: chunked — побайтовый автомат.
//
// Формат (RFC 9112, 7.1):
//   <hex-размер>[;расширение]\r\n
//   <данные ровно размером байт>\r\n
//   …
//   0\r\n
//   [трейлеры]\r\n
//
// Читаем байт за байтом: чанки приходят кусками произвольной длины, и границы
// регулярно попадают внутрь заголовка длины («1f» разрезан на «1» и «f»).
// Накопительный разбор здесь был бы источником ошибок на ровном месте.
//
// Данные отдаются приёмнику сразу, не собираясь в промежуточный буфер: чанк
// бывает на несколько килобайт, а лишняя копия — это и память, и задержка
// перед первой буквой ответа.

#include "util/chunked.h"

namespace http {
namespace {

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

}  // namespace

void ChunkedDecoder::reset()
{
    _state = State::Size;
    _size = 0;
    _remaining = 0;
    _size_digits = false;
    _trailer_line_empty = true;
    _payload_bytes = 0;
    _chunks = 0;
}

bool ChunkedDecoder::feed(const char* data, size_t len)
{
    if (data == nullptr) return _state != State::Done && _state != State::Error;

    for (size_t i = 0; i < len; ++i) {
        if (_state == State::Done || _state == State::Error) break;
        handle_byte(data[i]);
    }

    return _state != State::Done && _state != State::Error;
}

void ChunkedDecoder::handle_byte(char ch)
{
    switch (_state) {
    case State::Size: {
        const int v = hex_value(ch);

        if (v >= 0) {
            // 8 hex-цифр — это 4 ГБ; чанк больше — заведомо мусор в потоке.
            if (_size > 0x0FFFFFFul) {
                _state = State::Error;
                return;
            }
            _size = _size * 16 + static_cast<unsigned long>(v);
            _size_digits = true;
            return;
        }

        if (ch == ';') {
            if (!_size_digits) {
                _state = State::Error;
                return;
            }
            _state = State::Extension;
            return;
        }

        if (ch == '\r') {
            if (!_size_digits) {
                _state = State::Error;
                return;
            }
            _state = State::SizeLf;
            return;
        }

        _state = State::Error;
        return;
    }

    case State::Extension:
        // Расширение чанка не нужно: пропускаем до конца строки.
        if (ch == '\r') {
            _state = State::SizeLf;
        } else if (ch == '\n') {
            _state = State::Error;  // CR обязателен
        }
        return;

    case State::SizeLf:
        if (ch != '\n') {
            _state = State::Error;
            return;
        }

        _size_digits = false;
        if (_size == 0) {
            // Терминальный чанк: дальше только трейлеры и пустая строка.
            _state = State::Trailer;
            _trailer_line_empty = true;
            return;
        }

        _remaining = _size;
        _size = 0;
        ++_chunks;
        _state = State::Data;
        return;

    case State::Data:
        if (_sink != nullptr) _sink(&ch, 1, _ctx);
        ++_payload_bytes;
        if (--_remaining == 0) _state = State::DataCr;
        return;

    case State::DataCr: _state = (ch == '\r') ? State::DataLf : State::Error; return;

    case State::DataLf:
        if (ch != '\n') {
            _state = State::Error;
            return;
        }
        _state = State::Size;
        return;

    case State::Trailer:
        // Трейлер — строка `имя: значение`. Нас интересует только пустая
        // строка, которая завершает поток.
        if (ch == '\r') {
            _state = State::TrailerLf;
            return;
        }
        if (ch == '\n') {
            _state = State::Done;
            return;
        }
        _trailer_line_empty = false;
        return;

    case State::TrailerLf:
        if (ch != '\n') {
            _state = State::Error;
            return;
        }
        _state = _trailer_line_empty ? State::Done : State::Trailer;
        _trailer_line_empty = true;
        return;

    case State::Done:
    case State::Error: return;
    }
}

}  // namespace http
