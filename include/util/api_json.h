#pragma once

// Сборка запроса к API и разбор ответа — чистые функции без Arduino.
//
// Всё, что можно проверить на хосте, живёт здесь: формат тела запроса, разбор
// чанков SSE-потока, разбор ошибок. Сетевую часть (TLS, чтение потока) делает
// hw/api_client, и там ошибок почти не бывает — почти все дефекты стриминга
// находятся в разборе, а его гоняем тестами.
//
// Особенности RouterAI, учтённые здесь:
//   - `usage` приходит с именами `input_tokens`/`output_tokens`, тогда как
//     OpenAI-совместимые клиенты ждут `prompt_tokens`/`completion_tokens`.
//     Читаем оба варианта, иначе счётчик токенов молча показывал бы нули.
//   - `[DONE]` — маркер конца потока, его разбирает util/sse.
//   - ошибка приходит как `{"error": "текст"}` либо `{"error":{"message":…}}`.

#include <stddef.h>
#include <stdint.h>

namespace api {

/// Роль сообщения в диалоге. Соответствует API.
enum class Role : uint8_t {
    System,
    User,
    Assistant,
    Tool,
};

/// Одно сообщение для отправки модели. Строки не копируются: владелец —
/// вызывающий (ChatStore).
struct OutMessage {
    Role role{Role::User};
    const char* content{nullptr};
    /// Для role == Tool: идентификатор вызова инструмента, на который это
    /// сообщение отвечает. Для остальных ролей — nullptr.
    const char* tool_call_id{nullptr};
    /// Для role == Tool: имя инструмента.
    const char* name{nullptr};
    /// Для role == Assistant: вызовы инструментов, которые модель сделала в
    /// этом сообщении, — массив JSON как есть (`[{"id":…,"function":{…}}]`).
    ///
    /// Сервер не сверяет `tool_call_id` (проверено, Р-025), но сообщение
    /// ассистента с вызовами возвращать надо: без него модель не понимает,
    /// на что именно отвечают сообщения role=tool.
    const char* tool_calls_json{nullptr};
};

/// Параметры запроса.
struct RequestOptions {
    const char* model{nullptr};
    /// 0.0..2.0, передаётся как есть одним знаком после запятой.
    uint8_t temperature_tenths{3};
    /// "none", "low", "medium", "high"; nullptr — не передавать.
    const char* reasoning_effort{nullptr};
    /// Стриминг включён. Для чата всегда true.
    bool stream{true};
    /// Верхний предел выходных токенов; 0 — не указывать.
    unsigned max_tokens{0};
    /// Описание инструментов — массив JSON как есть, из
    /// tools::Registry::build_tools_json(). nullptr — поле не передавать.
    ///
    /// Передавать надо в КАЖДОМ запросе цикла, включая ход с результатами
    /// инструментов: без `tools` модель иногда печатает внутреннюю разметку
    /// DSML прямо в текст ответа (Р-025).
    const char* tools_json{nullptr};
    /// Поле `plugins` для веб-поиска — массив JSON как есть, из
    /// web::build_plugins_json(). nullptr — поиск не выполняется.
    ///
    /// Ставится ТОЛЬКО в поисковом подзапросе по вызову `web_search` (Р-040):
    /// в основном запросе плагина нет никогда. Иначе сервер искал бы по тексту
    /// любой реплики, включая «привет», и брал бы за это ~2,1 ₽ — именно так
    /// было до Р-040, и это было ошибкой проектирования.
    const char* plugins_json{nullptr};
};

/// Собирает тело POST-запроса к /chat/completions.
///
/// Буфер выделяет вызывающий — в прошивке он в PSRAM: тело с историей легко
/// достигает десятков килобайт, а внутренней RAM под TLS и без того мало.
///
/// @return число байт без завершающего нуля, 0 если буфер мал
size_t build_request(const OutMessage* msgs, size_t count, const RequestOptions& opt, char* out, size_t out_len);

/// Разобранный кусок стриминга.
struct Chunk {
    /// Есть новый текст ответа (может быть пустым — тогда has_text false).
    bool has_text{false};
    size_t text_len{0};
    /// Пришёл кусок рассуждений модели (delta.reasoning).
    ///
    /// Проверено на живом RouterAI: при `reasoning_effort` != "none" модель
    /// сначала стримит рассуждения (поле `reasoning`), и только потом текст
    /// ответа. Пауза перед первым словом — до полутора секунд. Без индикатора
    /// «думаю…» экран выглядит зависшим.
    bool has_reasoning{false};
    size_t reasoning_len{0};
    /// Причина завершения: "stop", "length", "tool_calls", "content_filter".
    bool has_finish{false};
    char finish_reason[24]{};
    /// Пришёл блок usage (обычно в последнем чанке).
    bool has_usage{false};
    long input_tokens{0};
    long output_tokens{0};
    /// Токены рассуждений: тарифицируются как выходные, и при
    /// reasoning_effort != "none" их бывает больше, чем самого ответа.
    long reasoning_tokens{0};
};

/// Разбирает одно событие SSE (JSON-объект чанка).
///
/// @param json          полезная нагрузка события
/// @param len           её длина
/// @param out           результат: текст ответа и служебные поля
/// @param text_out      буфер, куда копируется текст дельты; может быть nullptr,
///                      если текст не нужен (например, на экране только индикатор)
/// @param text_cap      размер буфера
/// @param reasoning_out буфер для куска рассуждений; nullptr — не нужно копить
/// @param reasoning_cap его размер
/// @return false, если JSON не разобрался. Это не фатально: незнакомый чанк
///         лучше пропустить, чем оборвать ответ.
bool parse_chunk(const char* json, size_t len, Chunk& out, char* text_out, size_t text_cap,
    char* reasoning_out = nullptr, size_t reasoning_cap = 0);

/// Разбирает ответ БЕЗ стриминга целиком (`choices[0].message`).
///
/// Зачем, если чат всегда просит stream:true: если сервер или прокси
/// проигнорирует флаг и вернёт обычный JSON, SSE-парсер не увидит ни одного
/// события, и экран провисит «думаю…» до таймаута. Лучше разобрать ответ и
/// показать его как обычно.
///
/// Заполняет в out `has_text`, `has_reasoning`, `has_finish`, `has_usage`
/// по тем же правилам, что и parse_chunk.
///
/// @return false, если тело не похоже на ответ модели
bool parse_full_response(const char* json, size_t len, Chunk& out, char* text_out, size_t text_cap,
    char* reasoning_out = nullptr, size_t reasoning_cap = 0);

/// Достаёт человекочитаемый текст ошибки из тела ответа.
///
/// @param out буфер для сообщения
/// @return true, если удалось что-то извлечь; иначе в out записан HTTP-код
bool parse_error(const char* json, size_t len, int http_code, char* out, size_t out_len);

/// Человекочитаемая ошибка по HTTP-коду, когда тела нет или оно бесполезно.
const char* http_error_text(int http_code);

/// Исчерпан лимит трат/квота по ключу — то есть повторять запрос бессмысленно.
///
/// Отдельно от кода ответа, потому что RouterAI отвечает **429** и на «слишком
/// часто», и на «кончились деньги»:
///
/// ```
/// {"error":{"message":"API key weekly spending limit exceeded: 100,22 руб. / 100,00 руб.","code":429}}
/// ```
///
/// Первое лечится паузой, второе — только пополнением баланса. Пока код
/// считал любой 429 временным, устройство дважды ждало и повторяло запрос,
/// тратя полминуты, а затем показывало английскую строку сервера про
/// «weekly spending limit» — по ней невозможно понять, что делать.
///
/// Узнаём по тексту тела: HTTP-код у обоих случаев один.
bool quota_exhausted(int http_code, const char* json, size_t len);

}  // namespace api
