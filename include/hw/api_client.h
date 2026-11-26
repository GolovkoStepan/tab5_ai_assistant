#pragma once

// HTTPS-клиент к OpenAI-совместимому API.
//
// Это единственный модуль, который нельзя проверить на хосте: TLS, сокет и
// HTTP живут только на плате. Поэтому вся логика, которую можно вынести,
// вынесена: сборка запроса и разбор ответа — в util/api_json, разбор потока —
// в util/sse и util/chunked, адрес — в util/url. Здесь остаются только
// сокет, HTTPClient и таймауты.
//
// Особенности, проверенные на живом RouterAI:
//   * ответ на stream:true приходит с `Transfer-Encoding: chunked`, и
//     `HTTPClient::getStreamPtr()` чанки НЕ декодирует — без util/chunked в
//     поток попадут шестнадцатеричные длины;
//   * `usage` приходит в стриминге последним чанком, имена полей —
//     `prompt_tokens`/`completion_tokens`;
//   * при `reasoning_effort != "none"` модель сначала стримит рассуждения в
//     `delta.reasoning`, и текст ответа начинается только через ~1.5 с.

#include <stddef.h>
#include <stdint.h>

#include "util/api_json.h"
#include "util/chunked.h"
#include "util/sse.h"
#include "util/tool_calls.h"
#include "util/web_sources.h"

/// Сетевые типы Arduino скрыты за указателями: иначе заголовок пришлось бы
/// включать в каждый экран, который знает про клиент. Объявлены в глобальной
/// области, как и в библиотеках Arduino, — внутри namespace hw это были бы
/// другие типы, и компилятор не смог бы их удалить.
class NetworkClient;
class NetworkClientSecure;
class HTTPClient;
class NetworkClient;

namespace hw {

class ApiClient {
public:
    /// Состояние одного запроса.
    enum class State : uint8_t {
        Idle,
        Connecting,  ///< TLS и отправка заголовков
        Streaming,   ///< читаем чанки и выдаём события
        RetryWait,   ///< ждём и повторим запрос (429/5xx)
        Done,        ///< ответ получен целиком
        Failed,      ///< ошибка, текст в error()
    };

    /// Получатели событий стриминга. Вызываются из tick(), то есть в
    /// UI-задаче — рисовать из них можно.
    class Listener {
    public:
        virtual ~Listener() = default;

        /// Пришёл кусок текста ответа.
        virtual void on_text(const char* text, size_t len) = 0;

        /// Пришёл кусок рассуждений модели. Вызывается до первого on_text.
        /// Реализация по умолчанию ничего не делает: экран может не показывать
        /// рассуждения вовсе.
        virtual void on_reasoning(const char*, size_t) {}

        /// Пришёл блок usage (обычно в конце ответа).
        virtual void on_usage(long input_tokens, long output_tokens, long reasoning_tokens)
        {
            (void)input_tokens;
            (void)output_tokens;
            (void)reasoning_tokens;
        }

        /// Ответ завершён. reason — "stop", "length", "tool_calls" и т. п.
        ///
        /// Вызывается ПОСЛЕ on_tool_calls(): к этому моменту слушатель уже
        /// знает, просила ли модель инструменты, и решает, закрывать ли ход.
        virtual void on_finished(const char* reason) = 0;

        /// Модель просит вызвать инструменты.
        ///
        /// Вызывается ДО on_finished(): к этому моменту поток закончился,
        /// аргументы склеены целиком. Список живёт до следующего start(), так
        /// что выполнять надо сразу или скопировать.
        ///
        /// Только читает: транспорт ничего не знает про файлы и не выполняет
        /// инструменты сам (Р-026).
        virtual void on_tool_calls(const tools::Accumulator& calls) { (void)calls; }

        /// Источники веб-поиска из `annotations` (Р-036).
        ///
        /// Вызывается ДО on_finished(), как и on_tool_calls: список полон
        /// только к концу потока, а слушатель до этого момента не знает, что
        /// ответ закончен. Живёт до следующего start() — копировать сразу.
        ///
        /// Вызывается, только если источники есть: «поиска не было» и «поиск
        /// ничего не нашёл» различаются тем, что в первом случае вызова нет
        /// вовсе, а во втором есть поле `annotations` с пустым массивом —
        /// его видно через web::mentions_annotations().
        virtual void on_web_sources(const web::List& sources) { (void)sources; }

        /// Ошибка. Текст уже человекочитаемый.
        virtual void on_error(const char* message) = 0;

        /// Сервер ответил 429 или 5xx, запрос будет повторён через delay_s.
        ///
        /// Нужно, чтобы экран сказал «жду» вместо «думаю»: пауза в полминуты
        /// без объяснения выглядит как зависание, а Esc в это время работает.
        virtual void on_retry(int attempt, int delay_s)
        {
            (void)attempt;
            (void)delay_s;
        }
    };

    /// Начать запрос. Возвращает сразу: подключение и чтение идут в tick(),
    /// иначе экран замирал бы на время TLS-рукопожатия (до 30 с).
    ///
    /// @param base_url адрес из настроек, вида https://routerai.ru/api/v1
    /// @param api_key  ключ из NVS
    /// @param body     уже собранное тело запроса (util/api_json)
    /// @param listener получатель событий
    /// @param err      буфер для причины отказа начать запрос
    bool start(
        const char* base_url, const char* api_key, const char* body, Listener* listener, char* err, size_t err_len);

    /// Продвинуть запрос. Вызывать в цикле, пока state() не станет Done или
    /// Failed. Читает готовые данные и выдаёт события слушателю.
    void tick();

    /// Текущее состояние.
    State state() const { return _state; }

    /// Текст ошибки, если state() == Failed.
    const char* error() const { return _error; }

    /// Прервать запрос. Уже полученный текст остаётся у слушателя: частичный
    /// ответ сохраняется с пометкой.
    void cancel();

    /// Идёт ли запрос прямо сейчас (включая ожидание повтора).
    bool busy() const
    {
        return _state == State::Connecting || _state == State::Streaming || _state == State::RetryWait;
    }

    /// Сколько раз запрос уже повторяли. 0 — с первой попытки.
    int attempts() const { return _attempt; }

    /// HTTP-код последнего ответа (0 — ответа не было).
    int http_code() const { return _http_code; }

    /// Источники веб-поиска последнего ответа. Живут до следующего start().
    ///
    /// Отдельный доступ нужен, потому что «поиск ничего не нашёл» — это не
    /// пустой список, а ответ с полем `annotations` и пустым массивом; такое
    /// видно только здесь, в колбэк оно не попадает.
    const web::List& web_sources() const { return _sources != nullptr ? *_sources : _empty_sources; }

    /// Был ли в ответе хоть один блок `annotations`. false — поиск не
    /// выполнялся вовсе (плагин выключен или модель не сочла нужным искать).
    bool web_search_ran() const { return _sources_seen; }

private:
    /// Обработчик одного события SSE из парсера. Статический, потому что
    /// util/sse не знает про классы.
    static void on_sse_event(const char* data, size_t len, void* ctx);

    /// Обработчик раскодированных данных из chunked-декодера: кормит их
    /// SSE-парсеру.
    static void on_chunk_data(const char* data, size_t len, void* ctx);

    /// Закрыть соединение, не меняя состояние и не уведомляя слушателя.
    /// Нужно перед повтором: старый сокет уже не годится, а запрос не окончен.
    void teardown();

    /// Запланировать повтор после 429/5xx. false — повторять нельзя.
    bool schedule_retry(int http_code, unsigned long* out_delay_ms);

    /// Закрыть соединение и освободить буфер.
    void finish(State state);
    void fail(const char* message);
    void handle_event(const char* data, size_t len);
    /// Подключиться и отправить запрос. Вызывается из tick(), чтобы экран не
    /// замирал на время TLS-рукопожатия.
    void begin_connect();

    /// Создать SSE-парсер, если его нет.
    ///
    /// Нужен на каждой попытке, а не один раз в start(): перед повтором
    /// teardown() удаляет парсер вместе с соединением. Раньше повтор уходил с
    /// пустым телом и до разбора не доходил, поэтому отсутствие парсера не
    /// проявлялось (Р-042).
    /// @param err буфер для причины отказа
    /// @param err_len размер буфера
    /// @return false, если памяти не хватило
    bool ensure_sse(char* err, size_t err_len);
    /// Разобрать ответ, пришедший не потоком (сервер проигнорировал stream).
    void handle_full_response();

    Listener* _listener{nullptr};

    State _state{State::Idle};
    char _error[160]{};
    /// HTTP-код ответа: нужен, чтобы подставить понятный текст, когда тело
    /// ошибки пустое или это HTML от прокси.
    int _http_code{0};

    /// Парсер SSE и декодер чанков создаются в start(): у них есть указатели
    /// на this, и жить дольше запроса они не должны.
    sse::Parser* _sse{nullptr};
    http::ChunkedDecoder _chunked{&ApiClient::on_chunk_data, this};

    /// Соединения и HTTP-объект. Живут от start() до finish().
    ///
    /// Два указателя, а не один базовый: `http://` в адресе API разрешён и
    /// нужен для проверки на своём сервере, но у NetworkClient деструктор не
    /// виртуальный. `delete` TLS-объекта через базовый указатель не вызвал бы
    /// ~NetworkClientSecure и утёк бы TLS-контекстом на каждом запросе.
    NetworkClientSecure* _tls_client{nullptr};
    NetworkClient* _plain_client{nullptr};
    HTTPClient* _http{nullptr};

    /// Буфер тела запроса: тело с историей легко достигает десятков килобайт,
    /// поэтому он в PSRAM, а не в куче по умолчанию.
    char* _body{nullptr};
    size_t _body_len{0};

    /// Накопитель вызовов инструментов на один ответ.
    ///
    /// 8 вызовов по 2 КБ аргументов — 16 КБ, поэтому он в PSRAM: на стеке
    /// задачи интерфейса столько нет.
    tools::Accumulator* _calls{nullptr};

    /// Источники веб-поиска, накопленные из `annotations` за ответ.
    ///
    /// Пять источников по ~600 байт — 3 КБ; на стеке задачи интерфейса это
    /// слишком дорого, а в куче по умолчанию отнимает место у TLS. PSRAM.
    web::List* _sources{nullptr};
    /// Было ли в ответе поле `annotations` хоть раз. Отличает «поиск ничего
    /// не нашёл» от «поиска не было».
    bool _sources_seen{false};
    /// Пустой список на случай, если память под источники не выделилась.
    web::List _empty_sources{};

    /// Ответ приходит с `Transfer-Encoding: chunked` (обычный случай для SSE).
    /// Если сервер отдал Content-Length, чанки разбирать нельзя: JSON в этом
    /// случае выглядит как некорректный заголовок чанка.
    bool _use_chunked{false};

    /// Копия настроек: адрес и ключ должны жить, пока идёт запрос.
    char _host[128]{};
    char _path[224]{};
    uint16_t _port{443};
    bool _tls{true};
    char _api_key[208]{};

    /// Буфер чтения из сокета. 1 КБ: чанки приходят мелкими, а больший буфер
    /// только отнимает внутреннюю RAM у TLS.
    char _read_buf[1024]{};

    /// Сколько байт тела ошибки уже собрано.
    char _error_body[512]{};
    size_t _error_body_len{0};

    /// Последний finish_reason из потока: "stop", "length" и т. п.
    char _finish_reason[24]{};

    /// Сколько миллисекунд прошло с начала запроса — для таймаута.
    unsigned long _started_ms{0};
    /// Время последнего полученного байта: таймаут считается по «тишине», а
    /// не по общей длительности — рассуждения бывают долгими.
    unsigned long _last_byte_ms{0};

    /// Сколько всего попыток сделано и когда начинать следующую.
    int _attempt{0};
    unsigned long _retry_at_ms{0};

    unsigned long _input_tokens{0};
    unsigned long _output_tokens{0};
    unsigned long _reasoning_tokens{0};
    bool _usage_seen{false};
};

}  // namespace hw
