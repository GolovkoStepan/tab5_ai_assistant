// HTTPS-клиент к OpenAI-совместимому API.
//
// Всё, что можно было проверить на хосте, здесь отсутствует: сборка запроса и
// разбор ответа — util/api_json, поток — util/sse и util/chunked, адрес —
// util/url. Этот файл — сокет, TLS, HTTPClient и ничего больше.
//
// Четыре грабли, на которых легко потерять полдня:
//
//  1. `HTTPClient::getStreamPtr()` отдаёт сырой сокет БЕЗ декодирования
//     `Transfer-Encoding: chunked`. RouterAI отвечает стримингом именно так
//     (проверено на живом API: HTTP/1.1, Transfer-Encoding: chunked), поэтому
//     чанки разбирает util/chunked. Без него первым в потоке придёт «34\r\n».
//
//  2. Читать в блокирующем цикле нельзя: экран замер бы на всё время ответа.
//     tick() забирает только то, что уже пришло (available()), и сразу
//     возвращает управление.
//
//  3. Таймаут считается по «тишине», а не по общей длительности. При
//     reasoning_effort != "none" модель сначала думает: пауза до первого
//     видимого символа доходит до полутора секунд.
//
//  4. Объекты HTTPClient и NetworkClientSecure не должны быть локальными в
//     tick(): указатель на локальный HTTPClient пережил бы свой объект, и
//     следующий tick() читал бы освобождённую память.

#include "hw/api_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "util/chunked.h"
#include "util/url.h"

namespace hw {
namespace {

/// Таймаут на подключение и TLS-рукопожатие.
constexpr uint32_t kConnectTimeoutMs = 30000;

/// Сколько ждём «тишины» в потоке, прежде чем считать соединение оборванным.
/// Ответ с рассуждениями идёт рывками, но паузы между чанками — доли секунды.
constexpr uint32_t kIdleTimeoutMs = 60000;

/// Предельная длительность одного запроса. Страховка от вечного соединения:
/// даже если сервер присылает по байту в минуту, запрос конечен.
constexpr uint32_t kTotalTimeoutMs = 300000;

/// Максимальный размер тела ошибки, которое собираем для показа.
constexpr size_t kMaxErrorBody = 512;

/// Сколько раз повторяем запрос при 429 и 5xx.
///
/// Две повторные попытки — компромисс: одна не переживает короткий всплеск
/// нагрузки, а каждая следующая пауза удваивается, и пять попыток означали бы
/// ожидание в несколько минут. Дольше держать пользователя в «жду» бессмысленно:
/// он уже нажал кнопку и ждёт ответа.
constexpr int kMaxRetries = 2;

/// Пауза перед первым повтором. Дальше удваивается.
constexpr uint32_t kRetryBaseMs = 2000;

/// Предельная пауза из `Retry-After`. Сервер вправе попросить и час, но ждать
/// час на карманном устройстве никто не станет — лучше показать ошибку.
constexpr uint32_t kRetryMaxMs = 30000;

}  // namespace

bool ApiClient::start(
    const char* base_url, const char* api_key, const char* body, Listener* listener, char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (busy()) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "предыдущий запрос ещё идёт");
        return false;
    }

    _listener = listener;
    _error[0] = '\0';
    _http_code = 0;
    _finish_reason[0] = '\0';
    _input_tokens = 0;
    _output_tokens = 0;
    _reasoning_tokens = 0;
    _usage_seen = false;

    // Накопитель вызовов — в PSRAM: 8 вызовов по 2 КБ аргументов не помещаются
    // на стеке задачи интерфейса.
    if (_calls == nullptr) {
        _calls = static_cast<tools::Accumulator*>(
            static_cast<void*>(heap_caps_malloc(sizeof(tools::Accumulator), MALLOC_CAP_SPIRAM)));
        if (_calls == nullptr) _calls = static_cast<tools::Accumulator*>(malloc(sizeof(tools::Accumulator)));
        if (_calls == nullptr) {
            if (err && err_len > 0) snprintf(err, err_len, "не хватило памяти на вызовы инструментов");
            return false;
        }
        new (_calls) tools::Accumulator();
    }
    _calls->reset();

    // Источники веб-поиска — тоже в PSRAM: пять записей по ~600 байт это 3 КБ,
    // а рядом TLS и история чата.
    if (_sources == nullptr) {
        _sources = static_cast<web::List*>(static_cast<void*>(heap_caps_malloc(sizeof(web::List), MALLOC_CAP_SPIRAM)));
        if (_sources == nullptr) _sources = static_cast<web::List*>(malloc(sizeof(web::List)));
        if (_sources == nullptr) {
            if (err && err_len > 0) snprintf(err, err_len, "не хватило памяти на источники поиска");
            return false;
        }
        new (_sources) web::List();
    }
    _sources->clear();
    _sources_seen = false;

    // Повторы считаются от начала запроса, а не от начала попытки.
    _attempt = 0;
    _retry_at_ms = 0;

    // --- Адрес -------------------------------------------------------------
    url::Parts parts;
    if (!url::parse(base_url, parts, err, err_len)) return false;
    if (!url::append_path(parts, "/chat/completions")) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "адрес API слишком длинный");
        return false;
    }

    snprintf(_host, sizeof(_host), "%s", parts.host);
    snprintf(_path, sizeof(_path), "%s", parts.path);
    _port = parts.port;
    _tls = parts.tls;
    snprintf(_api_key, sizeof(_api_key), "%s", api_key ? api_key : "");

    if (_api_key[0] == '\0') {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "не задан ключ API");
        return false;
    }

    // --- Тело запроса в PSRAM ---------------------------------------------
    // Тело с историей легко достигает десятков килобайт. В куче по умолчанию
    // его может не оказаться рядом с TLS, которому нужно ~32 КБ внутренней RAM.
    const size_t body_len = strlen(body ? body : "");
    if (body_len == 0) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "пустое тело запроса");
        return false;
    }

    _body = static_cast<char*>(heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM));
    if (_body == nullptr) {
        // PSRAM кончилась — пробуем обычную кучу, чем отказывать совсем.
        _body = static_cast<char*>(malloc(body_len + 1));
    }
    if (_body == nullptr) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "не хватило памяти на тело запроса");
        return false;
    }
    memcpy(_body, body, body_len + 1);
    _body_len = body_len;

    if (!ensure_sse(err, err_len)) {
        free(_body);
        _body = nullptr;
        return false;
    }

    // Декодер живёт вместе с объектом, а поток у каждого запроса свой.
    _chunked.reset();
    _use_chunked = false;

    _started_ms = millis();
    _last_byte_ms = _started_ms;
    _state = State::Connecting;
    return true;
}

bool ApiClient::ensure_sse(char* err, size_t err_len)
{
    if (_sse != nullptr) return true;
    _sse = new sse::Parser(&ApiClient::on_sse_event, this);
    if (_sse == nullptr) {
        if (err && err_len > 0) snprintf(err, err_len, "%s", "не хватило памяти на парсер");
        return false;
    }
    return true;
}

void ApiClient::on_chunk_data(const char* data, size_t len, void* ctx)
{
    auto* self = static_cast<ApiClient*>(ctx);
    if (self->_sse != nullptr) self->_sse->feed(data, len);
}

void ApiClient::on_sse_event(const char* data, size_t len, void* ctx)
{
    static_cast<ApiClient*>(ctx)->handle_event(data, len);
}

void ApiClient::handle_event(const char* data, size_t len)
{
    api::Chunk chunk;
    // Буферы статические: дельта обычно в несколько байт, но сервер вправе
    // прислать кусок до размера события SSE (8 КБ), и обрезка до 512 молча
    // съела бы часть ответа. Стек задачи при этом остаётся свободным.
    static char text[4096];
    static char reasoning[4096];

    if (!api::parse_chunk(data, len, chunk, text, sizeof(text), reasoning, sizeof(reasoning))) {
        // Незнакомый чанк не повод обрывать ответ: сервер может добавить поле,
        // о котором мы не знаем.
        return;
    }

    if (chunk.has_reasoning && _listener != nullptr) _listener->on_reasoning(reasoning, chunk.reasoning_len);
    if (chunk.has_text && _listener != nullptr) _listener->on_text(text, chunk.text_len);

    // Вызовы инструментов приходят кусками; кормим накопитель каждым чанком.
    // Склеивать обязательно: короткий вызов приезжает за десяток дельт (Р-025).
    if (_calls != nullptr) _calls->feed(data, len);

    // Источники веб-поиска (Р-036). Обычно приходят одним чанком, но
    // накопитель дедуплицирует по URL — полагаться на «одним чанком» нельзя.
    if (_sources != nullptr) {
        if (!_sources_seen && web::mentions_annotations(data, len)) _sources_seen = true;
        web::feed(data, len, *_sources);
    }

    if (chunk.has_finish) {
        snprintf(_finish_reason, sizeof(_finish_reason), "%s", chunk.finish_reason);
    }

    if (chunk.has_usage) {
        _usage_seen = true;
        _input_tokens = static_cast<unsigned long>(chunk.input_tokens);
        _output_tokens = static_cast<unsigned long>(chunk.output_tokens);
        _reasoning_tokens = static_cast<unsigned long>(chunk.reasoning_tokens);
        if (_listener != nullptr) {
            _listener->on_usage(chunk.input_tokens, chunk.output_tokens, chunk.reasoning_tokens);
        }
    }
}

void ApiClient::begin_connect()
{
    // State::Connecting — это «пора подключаться». Начало каждой попытки.
    _started_ms = millis();
    _last_byte_ms = _started_ms;

    // Повтор — это новый ответ с нуля: остатки прошлой попытки (usage,
    // finish_reason) не должны смешаться с ним.
    _finish_reason[0] = '\0';
    _usage_seen = false;
    _input_tokens = 0;
    _output_tokens = 0;
    _reasoning_tokens = 0;
    _http_code = 0;

    // Тип клиента — по схеме адреса. `http://` разрешён валидацией настроек и
    // нужен для проверки на своём сервере; TLS-клиент на обычном порту просто
    // не смог бы соединиться, и это выглядело бы как «сервер недоступен».
    //
    // Указатели РАЗДЕЛЬНЫЕ, и это не придирка: у NetworkClient деструктор не
    // виртуальный (`~NetworkClient();`), поэтому `delete` TLS-объекта через
    // базовый указатель — неопределённое поведение, а на практике деструктор
    // NetworkClientSecure не вызовется и TLS-контекст утечёт. В тесной
    // внутренней RAM платы такая утечка на каждом запросе кончилась бы
    // отказом соединений.
    if (_tls) {
        _tls_client = new NetworkClientSecure();
        if (_tls_client == nullptr) {
            fail("не хватило памяти на соединение");
            return;
        }
        // Проверяем сертификат по встроенному набору корневых CA. Без проверки
        // любой посредник читал бы ключ API в открытом виде.
        _tls_client->setHandshakeTimeout(kConnectTimeoutMs / 1000);
        _tls_client->useBuiltinCACertBundle();
        _tls_client->setConnectionTimeout(kConnectTimeoutMs);
    } else {
        _plain_client = new NetworkClient();
        if (_plain_client == nullptr) {
            fail("не хватило памяти на соединение");
            return;
        }
        _plain_client->setConnectionTimeout(kConnectTimeoutMs);
    }

    _http = new HTTPClient();
    if (_http == nullptr) {
        fail("не хватило памяти на HTTP-клиент");
        return;
    }

    // Парсер мог быть удалён teardown() перед повтором, а ответ на этот раз
    // может прийти успешный — тогда его надо разбирать. Раньше повтор уходил с
    // пустым телом, до разбора дело не доходило, и отсутствие парсера не
    // проявлялось (Р-042).
    char sse_err[64];
    if (!ensure_sse(sse_err, sizeof(sse_err))) {
        fail(sse_err);
        return;
    }

    _http->setConnectTimeout(static_cast<int32_t>(kConnectTimeoutMs));
    // Внимание: HTTPClient::setTimeout() принимает МИЛЛИСЕКУНДЫ, а не секунды
    // (внутри — NetworkClient::setTimeout(timeout_ms) и select). Перевод в
    // секунды в этой функции — ошибка в 1000 раз: со значением 60 запрос рвался
    // на чтении строки статуса через 60 мс и выглядел как «сеть: read Timeout».
    // Тип параметра uint16_t, поэтому 60 000 помещается, а минута — предел.
    _http->setTimeout(static_cast<uint16_t>(kIdleTimeoutMs));

    // HTTPClient по умолчанию не запоминает заголовки ответа, а нам нужно
    // знать, пришёл ли Transfer-Encoding: chunked. Запрашиваем явно.
    static const char* kCollected[] = {"Transfer-Encoding", "Content-Type", "Retry-After"};
    _http->collectHeaders(kCollected, 3);

    NetworkClient* client = _tls ? static_cast<NetworkClient*>(_tls_client) : _plain_client;
    if (!_http->begin(*client, _host, _port, _path, _tls)) {
        fail("не удалось начать соединение");
        return;
    }

    _http->addHeader("Content-Type", "application/json");
    _http->addHeader("Authorization", String("Bearer ") + _api_key);
    // Accept явно: без него сервер может ответить не-SSE.
    _http->addHeader("Accept", "text/event-stream");
    // Без gzip: нужен miniz, а внутренняя RAM дороже трафика. Для стриминга
    // сжатие ещё и мешает читать ответ по мере прихода.
    _http->addHeader("Accept-Encoding", "identity");

    const int code = _http->POST(reinterpret_cast<uint8_t*>(_body), _body_len);

    if (code <= 0) {
        // Тело больше не понадобится: повторять ошибку сети нечем — она
        // приходит до отправки, и `schedule_retry` здесь не вызывается.
        free(_body);
        _body = nullptr;
        _body_len = 0;
        char text[160];
        snprintf(text, sizeof(text), "сеть: %s", _http->errorToString(code).c_str());
        fail(text);
        return;
    }

    // Тело освобождает только finish(): при 429/5xx будет повтор, и ему нужен
    // тот же запрос. Раньше `free(_body)` стоял прямо здесь, и вторая попытка
    // отправляла ПУСТОЕ тело — сервер отвечал 400, а пользователь видел
    // «сервер отклонил запрос» вместо повтора. Найдено на фальшивом сервере
    // (scripts/fake_api.py): в логе вторая попытка приходила с `ключи: `.

    if (code >= 400) {
        // Тело ошибки маленькое (сотни байт) и приходит с Content-Length.
        // getString() разбирает и его, и chunked — руками читать сокет
        // незачем, а ошибиться там легко.
        _http_code = code;
        const String error_body = _http->getString();
        _error_body_len = error_body.length();
        if (_error_body_len > kMaxErrorBody - 1) _error_body_len = kMaxErrorBody - 1;
        memcpy(_error_body, error_body.c_str(), _error_body_len);
        _error_body[_error_body_len] = '\0';

        // 429 и 5xx — временные: сервер перегружен или у него сбой. Это не
        // ошибка запроса, и повторять его имеет смысл. Тело ошибки сохраняем
        // заранее: если повторы не помогут, покажем именно его.
        unsigned long delay_ms = 0;
        if (schedule_retry(code, &delay_ms)) {
            teardown();
            _retry_at_ms = millis() + delay_ms;
            _state = State::RetryWait;
            if (_listener != nullptr) _listener->on_retry(_attempt, static_cast<int>(delay_ms / 1000));
            return;
        }

        char text[160];
        api::parse_error(_error_body, _error_body_len, _http_code, text, sizeof(text));

        // Если повторы уже были, к причине добавляем их число: иначе «Слишком
        // много запросов» после трёх попыток выглядит как один отказ, и
        // непонятно, что клиент вообще пытался.
        if (_attempt > 0) {
            char with_attempts[200];
            snprintf(with_attempts, sizeof(with_attempts), "%s (попыток: %d)", text, _attempt + 1);
            fail(with_attempts);
            return;
        }

        fail(text);
        return;
    }

    // Ответ может быть как chunked (обычный случай для стриминга), так и с
    // Content-Length. Декодер чанков нужен только в первом случае, иначе он
    // принял бы JSON за заголовок чанка.
    const String transfer_encoding = _http->header("Transfer-Encoding");
    _use_chunked = transfer_encoding.indexOf("chunked") >= 0;

    // Если сервер проигнорировал stream:true и ответил обычным JSON, SSE-парсер
    // не увидит ни одного события, и экран провисит до таймаута. Проверяем тип
    // содержимого сразу: это дешевле, чем ждать минуту.
    const String content_type = _http->header("Content-Type");
    if (content_type.indexOf("event-stream") < 0) {
        handle_full_response();
        return;
    }

    _state = State::Streaming;
    _last_byte_ms = millis();
}

void ApiClient::handle_full_response()
{
    const String body = _http->getString();

    // Ответ без стриминга приходит целиком, и он бывает длинным: 512 байт на
    // стеке обрезали бы его молча, а модель «забыла» бы концовку. Буфер —
    // в PSRAM, размером под максимум сообщения треда.
    constexpr size_t kFullCap = 12 * 1024 + 1;
    char* text = static_cast<char*>(heap_caps_malloc(kFullCap, MALLOC_CAP_SPIRAM));
    if (text == nullptr) text = static_cast<char*>(malloc(kFullCap));

    if (text == nullptr) {
        fail("не хватило памяти на ответ модели");
        return;
    }

    api::Chunk chunk;
    char reasoning[512];
    const bool parsed =
        api::parse_full_response(body.c_str(), body.length(), chunk, text, kFullCap, reasoning, sizeof(reasoning));

    if (parsed) {
        if (_calls != nullptr) _calls->feed(body.c_str(), body.length());
        if (_sources != nullptr) {
            if (!_sources_seen && web::mentions_annotations(body.c_str(), body.length())) _sources_seen = true;
            web::feed(body.c_str(), body.length(), *_sources);
        }
        if (chunk.has_reasoning && _listener != nullptr) _listener->on_reasoning(reasoning, chunk.reasoning_len);
        if (chunk.has_text && _listener != nullptr) _listener->on_text(text, chunk.text_len);
        if (chunk.has_finish) snprintf(_finish_reason, sizeof(_finish_reason), "%s", chunk.finish_reason);
        if (chunk.has_usage && _listener != nullptr) {
            _listener->on_usage(chunk.input_tokens, chunk.output_tokens, chunk.reasoning_tokens);
        }
    }

    free(text);

    if (!parsed) {
        fail("ответ сервера не похож на ответ модели");
        return;
    }

    finish(State::Done);
}

void ApiClient::tick()
{
    if (_state == State::RetryWait) {
        // Пауза не блокирует: экран продолжает работать, Esc отменяет запрос.
        if (static_cast<long>(millis() - _retry_at_ms) < 0) return;
        _state = State::Connecting;
        return;
    }

    if (_state == State::Connecting) {
        begin_connect();
        return;
    }
    if (_state != State::Streaming) return;

    const unsigned long now = millis();

    NetworkClient* stream = _http != nullptr ? _http->getStreamPtr() : nullptr;
    if (stream == nullptr) {
        fail("соединение потеряно");
        return;
    }

    // Читаем только готовое: available() не блокирует.
    bool got_data = false;
    while (stream->available() > 0) {
        const int n = stream->read(reinterpret_cast<uint8_t*>(_read_buf), sizeof(_read_buf));
        if (n <= 0) break;

        got_data = true;
        _last_byte_ms = now;

        if (_use_chunked) {
            _chunked.feed(_read_buf, static_cast<size_t>(n));
        } else if (_sse != nullptr) {
            _sse->feed(_read_buf, static_cast<size_t>(n));
        }
    }

    if (_sse != nullptr && _sse->done()) {
        finish(State::Done);
        return;
    }

    // Терминальный чанк — тоже конец потока, даже если [DONE] потерялся.
    if (_use_chunked && _chunked.done()) {
        finish(State::Done);
        return;
    }
    if (_use_chunked && _chunked.broken()) {
        fail("нарушен формат потока");
        return;
    }

    // Молчание дольше таймаута — обрыв. Проверяем только когда данных нет,
    // иначе долгая пауза между чанками приняла бы живой ответ за обрыв.
    if (!got_data && now - _last_byte_ms > kIdleTimeoutMs) {
        fail("сервер молчит дольше минуты");
        return;
    }
    if (now - _started_ms > kTotalTimeoutMs) {
        fail("ответ не уложился в пять минут");
        return;
    }

    // Соединение закрылось, а терминального чанка так и не было: ответ
    // оборван. Частичный текст у слушателя уже есть — он его сохранит.
    if (!got_data && !stream->connected() && stream->available() <= 0) {
        finish(State::Done);
    }
}

void ApiClient::fail(const char* message)
{
    snprintf(_error, sizeof(_error), "%s", message);
    if (_listener != nullptr) _listener->on_error(_error);
    finish(State::Failed);
}

void ApiClient::teardown()
{
    if (_http != nullptr) {
        _http->end();
        delete _http;
        _http = nullptr;
    }
    if (_tls_client != nullptr) {
        _tls_client->stop();
        delete _tls_client;
        _tls_client = nullptr;
    }
    if (_plain_client != nullptr) {
        _plain_client->stop();
        delete _plain_client;
        _plain_client = nullptr;
    }
    delete _sse;
    _sse = nullptr;
    // Декодер и SSE-парсер начинают следующую попытку с чистого листа: без
    // этого остатки прошлого потока попали бы в разбор.
    _chunked.reset();
    _use_chunked = false;
    _finish_reason[0] = '\0';
}

bool ApiClient::schedule_retry(int http_code, unsigned long* out_delay_ms)
{
    // Лимит трат по ключу тоже приходит как 429, но повторять его бессмысленно:
    // пауза не вернёт деньги, а пользователь получит полминуты ожидания и то
    // же самое. Отличить помогает текст тела (Р-036, найденный на плате
    // дефект: устройство дважды повторяло запрос при исчерпанном ключе).
    if (api::quota_exhausted(http_code, _error_body, _error_body_len)) return false;

    // 429 — «слишком часто», 5xx — сбой на стороне сервера. 4xx кроме 429
    // повторять бессмысленно: запрос сам по себе неверен (ключ, модель, JSON),
    // и вторая попытка даст тот же ответ.
    const bool temporary = (http_code == 429) || (http_code >= 500 && http_code <= 599);
    if (!temporary) return false;
    if (_attempt >= kMaxRetries) return false;

    ++_attempt;

    // Retry-After сервер присылает в секундах (иногда как дату — это не
    // разбираем: неверная дата дала бы паузу в годы). Значение принимаем
    // только разумное.
    unsigned long delay_ms = 0;
    bool from_header = false;

    if (_http != nullptr) {
        const String retry_after = _http->header("Retry-After");
        if (retry_after.length() > 0) {
            const long seconds = retry_after.toInt();
            if (seconds > 0) {
                const unsigned long requested = static_cast<unsigned long>(seconds) * 1000UL;
                delay_ms = requested > kRetryMaxMs ? kRetryMaxMs : requested;
                from_header = true;
            }
        }
    }

    // Без Retry-After — экспоненциальная пауза: 2 с, затем 4 с.
    if (!from_header) {
        delay_ms = kRetryBaseMs;
        for (int i = 1; i < _attempt; ++i) {
            delay_ms *= 2;
            if (delay_ms >= kRetryMaxMs) {
                delay_ms = kRetryMaxMs;
                break;
            }
        }
    }

    if (out_delay_ms != nullptr) *out_delay_ms = delay_ms;
    return true;
}

void ApiClient::finish(State state)
{
    teardown();

    if (_body != nullptr) {
        free(_body);
        _body = nullptr;
        _body_len = 0;
    }

    _state = state;

    if (state == State::Done && _listener != nullptr) {
        // Вызовы — ДО on_finished: к этому моменту аргументы склеены целиком, а
        // слушатель ещё не решил, что ответ закончен. Ориентироваться на
        // finish_reason нельзя: наблюдали и "stop" при непустом наборе.
        if (_calls != nullptr && _calls->any()) _listener->on_tool_calls(*_calls);

        // Источники — тоже до on_finished: к этому моменту список собран, а
        // слушатель ещё не решил, что ответ закончен.
        //
        // Вызываем и при пустом списке, если поле `annotations` в ответе было:
        // «поиск ничего не нашёл» — это не то же самое, что «поиска не было»,
        // и экран должен сказать об этом словами, а не молчанием.
        if (_sources != nullptr && (_sources->count > 0 || _sources_seen)) _listener->on_web_sources(*_sources);

        // finish_reason мог не прийти, если поток закрылся без него: считаем
        // ответ завершённым нормально. Обрыв отличается тем, что слушатель
        // получил меньше текста, чем ожидал, — это его дело.
        _listener->on_finished(_finish_reason[0] != '\0' ? _finish_reason : "stop");
    }

    // Накопитель не освобождаем: он нужен следующему шагу агентского цикла, а
    // выделять и освобождать 34 КБ PSRAM на каждый запрос — готовые дырки в
    // куче посреди диалога. Держим один на весь клиент.
    if (_calls != nullptr) _calls->reset();
}

void ApiClient::cancel()
{
    if (!busy()) return;
    finish(State::Failed);
}

}  // namespace hw
