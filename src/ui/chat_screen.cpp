// Экран чата: лента, ввод, стриминг.
//
// Стриминг устроен так: сеть отдаёт куски текста по мере прихода, экран копит
// их в буфере ответа и перерисовывает **область ленты**, а не весь экран.
// Полный кадр — только при входе и при смене режима: перерисовка 1280x720 на
// каждый токен давала на этой плате заметное мерцание.
//
// Ленту приходится перерисовывать целиком при каждом куске, и это осознанно:
// текст переносится по словам, и «дописать» кусок в конец нельзя — при
// достижении края он сдвигает все строки абзаца. Область ленты ограничена
// 510 пикселями по высоте, а не всем экраном.

#include "ui/chat_screen.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "app_config.h"
#include "ui/draw.h"
#include "util/chat_history.h"
#include "util/chat_measure.h"
#include "util/chat_view.h"
#include "util/format.h"
#include "util/iso_time.h"

namespace ui {
namespace {

// --- Геометрия 1280x720 (поворот 3) -----------------------------------------
constexpr int kMarginX = 16;
constexpr int kTitleY = 8;
constexpr int kFeedY = 48;
constexpr int kFeedH = 510;
constexpr int kLineH = 22;
constexpr int kInputY = 566;
constexpr int kInputH = 46;
constexpr int kStatusY = 620;
constexpr int kErrorY = 652;
constexpr int kHintY = 692;

/// Ширина полоски-маркера слева от сообщения.
constexpr int kBarW = 4;
/// Отступ текста от маркера.
constexpr int kTextX = kMarginX + kBarW + 10;

/// Полоса прокрутки в правом поле ленты.
///
/// Раньше о скрытом содержимом говорили надписи «↑ выше» / «↓ ниже», но они
/// рисовались ПИКСЕЛЯМИ внутри ленты, а ленту при прокрутке двигает copyRect.
/// Пока шёл сдвиг, надпись успевала оказаться и на старом месте, и на новом —
/// на живой плате это выглядело как мерцающие двойные стрелки. Полоса живёт в
/// правом поле, сдвиг её не касается (ширина копирования ограничена правым
/// краем поля текста),
/// поэтому двоиться нечему. Заодно она показывает, где мы находимся в треде,
/// чего стрелки не умели.
constexpr int kScrollW = 6;
constexpr int kScrollGap = 6;

/// Максимум строк в одном сообщении. Ответ на 12 КБ даёт около 900 строк, а
/// массив под них на 200 сообщений не нужен: всё, что выше, не влезет в ленту.
constexpr uint16_t kMaxLinesPerMessage = 400;

/// Строк под подпись роли у каждого сообщения.
constexpr uint16_t kLabelLines = 1;

/// Бюджет истории, уходящей в запрос. Контекст модели огромен, но платим мы за
/// каждый входной токен, а тело запроса лежит в PSRAM и передаётся целиком.
constexpr size_t kHistoryBudgetBytes = 48 * 1024;

/// Системная инструкция. Без неё модель начинает отвечать по-английски и
/// простынями: у неё нет никаких признаков того, что это карманный помощник с
/// экраном на пять дюймов.
///
/// Про эмодзи здесь не прихоть. Шрифт экрана — `efontJA_16`, в нём нет ни
/// одного эмодзи (проверено разбором таблицы глифов: U+1F600, U+2705, U+2764,
/// U+263A, U+1F44D — глифов нет). Отсутствующий символ M5GFX рисует не пустым
/// местом, а рамкой-«квадратом» (IFont::drawCharDummy), поэтому ответ модели с
/// эмодзи выглядит на плате как «☐☐☐». Убрать квадраты на стороне прошивки
/// нельзя: рисовать эмодзи нечем, нужен растровый шрифт с картинками.
constexpr const char* kSystemPrompt =
    "Ты — карманный ИИ-помощник на устройстве M5Stack TAB5 с небольшим экраном. "
    "Отвечай по-русски, кратко и по делу. Инструкции давай нумерованными шагами. "
    "Не выдумывай факты: если не уверен — прямо скажи об этом. "
    "Не используй эмодзи и другие пиктограммы: шрифт экрана их не поддерживает, "
    "вместо них рисуются пустые квадраты.";

constexpr uint16_t kColorUser = TFT_CYAN;
constexpr uint16_t kColorAssistant = 0x07E0;  // зелёный
constexpr uint16_t kColorPartial = TFT_ORANGE;
constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorFrame = 0x18E3;

/// Подпись роли для ленты.
const char* role_label(api::Role role, bool partial)
{
    if (partial) return "модель · ответ неполный";
    switch (role) {
    case api::Role::User: return "вы";
    case api::Role::Assistant: return "модель";
    case api::Role::Tool: return "инструмент";
    case api::Role::System: return "система";
    }
    return "";
}

uint16_t role_color(api::Role role, bool partial)
{
    if (partial) return kColorPartial;
    return role == api::Role::User ? kColorUser : kColorAssistant;
}

bool is_char(const hw::KeyEvent& ev)
{
    return ev.type == hw::KeyEvent::Type::Char && ev.utf8[0] != '\0';
}

/// Строковые поля истории: статические, а не на стеке. Двести сообщений по
/// 40 байт — восемь килобайт, а стек задачи цикла этим не одарить.
history::Message g_hist[hw::ChatStore::kMaxMessages];
api::OutMessage g_out[hw::ChatStore::kMaxMessages + 1];

/// Смещения начал строк при переносе текста.
size_t g_offsets[kMaxLinesPerMessage + 2];

/// Буфер одной строки ленты для отрисовки. Одна строка — не больше
/// chars_per_line() символов, но символ UTF-8 занимает до 4 байт, поэтому
/// буфер заметно больше числа символов. Статический, а не на стеке: строки
/// рисуются в цикле по десяткам, и 1 КБ на кадр стека задачи интерфейса —
/// лишнее.
char g_line_buf[1024];

}  // namespace

// --- Получатель событий стриминга -------------------------------------------

namespace {

/// Пробрасывает события сетевого клиента на экран. Отдельный класс, потому что
/// hw::ApiClient не знает про ui и не должен знать.
class AnswerSink : public hw::ApiClient::Listener {
public:
    explicit AnswerSink(ChatScreen* screen) : _screen(screen) {}

    void on_text(const char* text, size_t len) override { _screen->on_answer_text(text, len); }
    void on_reasoning(const char*, size_t) override { _screen->on_answer_reasoning(); }
    void on_usage(long input_tokens, long output_tokens, long) override
    {
        _screen->on_answer_usage(input_tokens, output_tokens);
    }
    void on_finished(const char* reason) override { _screen->on_answer_finished(reason); }
    void on_retry(int attempt, int delay_s) override { _screen->on_answer_retry(attempt, delay_s); }
    void on_error(const char* message) override { _screen->on_answer_error(message); }

private:
    ChatScreen* _screen;
};

AnswerSink* g_sink = nullptr;

}  // namespace

// --- Жизненный цикл ---------------------------------------------------------

void ChatScreen::init(hw::ApiClient& api, hw::ChatStore& store, hw::SettingsStore& settings, hw::WifiManager& wifi)
{
    _api = &api;
    _store = &store;
    _settings = &settings;
    _wifi = &wifi;

    // Тело запроса в PSRAM: история легко даёт десятки килобайт, а рядом с TLS
    // во внутренней RAM столько не найти.
    _body = static_cast<char*>(heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM));
    if (_body == nullptr) _body = static_cast<char*>(malloc(kBodyCap));

    // Буфер счётчиков строк обнуляем сразу: если измерение по какой-то причине
    // не дошло до сообщения, ноль оставит в ленте одну строку вместо мусорного
    // числа из кучи — это заметно, но не превращает окно ленты в хаос.
    _line_counts = static_cast<uint16_t*>(calloc(hw::ChatStore::kMaxMessages, sizeof(uint16_t)));

    if (g_sink == nullptr) g_sink = new AnswerSink(this);
}

void ChatScreen::onEnter()
{
    _error[0] = '\0';

    // Если запрос уже идёт (мы вернулись на экран из настроек), ничего не
    // перезагружаем: иначе ответ модели писался бы в чужой тред.
    if (_mode == Mode::Idle && _store->slug()[0] == '\0') {
        // После перезагрузки показываем конец последнего разговора: пустой
        // экран при живых тредах на карте выглядел бы как потеря данных.
        if (!_store->load_latest()) {
            char slug[48];
            if (_store->create_new(slug, sizeof(slug))) Serial.printf("Чат: новый тред %s\n", slug);
        }
    }

    _scroll = 0;
    invalidate_measure();  // тред мог смениться, ширина могла измениться
    _feed_valid = false;   // лента на экране относится к прежнему треду
    _redraw_all = true;
}

void ChatScreen::onExit()
{
    // Запрос НЕ прерываем: уход в настройки не должен обрывать ответ.
    // Недописанное сохранится, когда ответ завершится (tick вызывается на
    // любом экране). Сохраняем только то, что уже не изменится.
    if (_mode == Mode::Idle && _store->has_unsaved()) {
        char err[96];
        _store->flush_last(err, sizeof(err));
    }

    // Взведённое подтверждение сброса не должно пережить уход с экрана.
    _clear_armed = false;
}

void ChatScreen::clear_thread(bool confirmed)
{
    if (_mode != Mode::Idle) return;

    // Сколько реплик потеряется — в предупреждении, чтобы нажатие было
    // осознанным: «пустой тред» и «тред на сорок сообщений» это разные вещи.
    const size_t n = _store->count();
    if (n == 0) {
        // Чистить нечего: файл и так пуст. Говорим об этом вместо вопроса,
        // иначе пользователь жмёт второе Ctrl+R и ждёт, что что-то изменится.
        _store->clear(nullptr, 0);
        reset_after_thread_change();
        fail("Тред и так пуст");
        return;
    }

    if (!confirmed) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Ctrl+R ещё раз — очистить тред и удалить %u сообщений с SD", (unsigned)n);
        fail(msg);
        _clear_armed = true;
        return;
    }

    char err[96];
    if (!_store->clear(err, sizeof(err))) {
        fail(err[0] != '\0' ? err : "Не удалось очистить тред");
        return;
    }

    Serial.printf("Чат: тред %s очищен\n", _store->slug());
    reset_after_thread_change();
    fail("Тред очищен");
}

void ChatScreen::new_thread()
{
    if (_mode != Mode::Idle) return;

    char slug[48];
    if (!_store->create_new(slug, sizeof(slug))) {
        fail("Не удалось создать тред на SD");
        return;
    }

    Serial.printf("Чат: новый тред %s\n", slug);
    reset_after_thread_change();
}

bool ChatScreen::open_thread(const char* slug)
{
    if (slug == nullptr || slug[0] == '\0') return false;

    // Идёт ответ — переключать тред нельзя: недописанный ответ прилип бы к
    // чужому разговору. Тот же запрет, что у Ctrl+N.
    if (_mode != Mode::Idle) {
        fail("Дождитесь ответа или нажмите Esc");
        return false;
    }

    // Тот же тред — перезагружать нечего. Чтение файла заново вернуло бы ленту
    // к состоянию на карте и потеряло бы недописанный ответ, который ещё не
    // сброшен на SD.
    if (strcmp(_store->slug(), slug) == 0) return true;

    // Недописанное (ответ, который уже не изменится) дописываем ДО смены
    // треда: flush_last() работает с открытым тредом, и после load() это был
    // бы уже чужой файл.
    if (_store->has_unsaved()) {
        char err[96];
        if (!_store->flush_last(err, sizeof(err))) {
            fail(err[0] != '\0' ? err : "Не удалось сохранить тред");
            return false;
        }
    }

    if (!_store->load(slug)) {
        fail("Не удалось открыть тред");
        return false;
    }

    Serial.printf("Чат: открыт тред %s\n", slug);
    reset_after_thread_change();
    return true;
}

void ChatScreen::reset_after_thread_change()
{
    // Общее для «нового треда» и «очистки треда»: и там, и там лента, поле,
    // прокрутка и измерения относятся к прежнему содержимому. Разъедутся —
    // получится тред из одного файла, а лента из другого.
    _error[0] = '\0';
    _scroll = 0;
    _input[0] = '\0';
    _input_len = 0;
    invalidate_measure();
    _feed_valid = false;
    _redraw_all = true;
}

void ChatScreen::enter_mode(Mode m)
{
    if (_mode == m) return;
    _mode = m;
    _redraw_status = true;

    // Подсказка в пустом поле ввода зависит от режима: «Вопрос…» или «Ждите
    // ответа». Без этой перерисовки после ответа в поле оставалось «Ждите
    // ответа», пока пользователь не начнёт печатать.
    if (_input_len == 0) _redraw_input = true;

    if (m == Mode::Sending) {
        _wait_started = millis();
        _dots = -1;
        _wait_sec = -1;
        _retry_left_s = -1;
    }

    // Ушли из «думаю» — сообщение о повторе больше не актуально. Без сброса
    // строка «сервер занят · повтор 2 через 0 с» застыла бы и после ответа.
    if (m != Mode::Sending) {
        _retry_attempt = 0;
        _retry_delay_s = 0;
        _retry_left_s = -1;
    }
}

void ChatScreen::fail(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text);
    _redraw_error = true;
}

// --- Клавиатура -------------------------------------------------------------

bool ChatScreen::onKey(const hw::KeyEvent& ev)
{
    // Подтверждение сброса живёт ровно до следующего события: запоминаем и
    // сразу снимаем. Так «взведённый» Ctrl+R не висит минутами и не срабатывает
    // от случайного нажатия через полчаса после предупреждения.
    const bool clear_was_armed = _clear_armed;
    _clear_armed = false;

    // Ctrl+N — всегда новый тред, даже во время ответа: это дешёвый способ
    // выйти из затянувшегося разговора. «т» — та же клавиша в ЙЦУКЕН.
    if (ev.ctrl && is_char(ev) && (strcmp(ev.utf8, "n") == 0 || strcmp(ev.utf8, "т") == 0)) {
        if (_mode == Mode::Idle) {
            new_thread();
        } else {
            fail("Дождитесь ответа или нажмите Esc");
        }
        return false;
    }

    // Ctrl+R — очистка открытого треда. «к» — та же клавиша в ЙЦУКЕН.
    //
    // Сброс необратим, поэтому первое нажатие только спрашивает, второе —
    // чистит.
    if (ev.ctrl && is_char(ev) && (strcmp(ev.utf8, "r") == 0 || strcmp(ev.utf8, "к") == 0)) {
        if (_mode != Mode::Idle) {
            fail("Дождитесь ответа или нажмите Esc");
            return false;
        }
        clear_thread(clear_was_armed);
        return false;
    }

    if (_mode != Mode::Idle) {
        // Пока идёт ответ, единственное осмысленное действие — прервать его.
        if (ev.type == hw::KeyEvent::Type::Special && ev.special == keymap::Special::Escape) {
            // Есть ли уже текст, решаем ДО finish_answer: он может выбросить
            // пустую реплику модели, и тогда сообщение «сохранён неполным»
            // было бы враньём — сохранять было нечего.
            const hw::ChatMessage* last = _store->last();
            const bool had_text = last != nullptr && last->content_len > 0;

            _answer.partial = true;
            if (_api != nullptr) _api->cancel();
            finish_answer(true, "cancel");

            if (had_text) {
                fail("Ответ прерван, сохранён неполным");
            } else {
                // Ответа не было вовсе. В треде остался вопрос без ответа, и
                // следующий ответ модели прилип бы к нему — в ленте вышло бы
                // «Назови цвет» → «Пять шагов». Откатываем вопрос и возвращаем
                // его в поле ввода: пользователь передумал, но набранное
                // терять незачем.
                restore_unsent_question();
            }
        }
        return false;
    }

    if (ev.type == hw::KeyEvent::Type::Special) {
        switch (ev.special) {
        case keymap::Special::Escape:
            // Esc на главном экране только сбрасывает ввод. Уводить с чата на
            // диагностическую матрицу клавиатуры по Esc нельзя: это основной
            // экран устройства, и «Esc выбросил меня на сетку клавиш» читается
            // как поломка. Диагностика — по Ctrl+D.
            if (_input_len > 0) {
                _input[0] = '\0';
                _input_len = 0;
                _redraw_input = true;
            }
            return false;

        case keymap::Special::Enter: send_question(); return false;

        case keymap::Special::Backspace: backspace(); return false;

        case keymap::Special::Space:
            // Пробел приходит спецклавишей, но в тексте это обычный символ.
            if (_input_len + 1 < sizeof(_input)) {
                _input[_input_len++] = ' ';
                _input[_input_len] = '\0';
                _redraw_input = true;
            }
            return false;

        case keymap::Special::Up: {
            const size_t total = total_lines();
            const size_t page = visible_lines();
            const size_t max_scroll = total > page ? total - page : 0;
            const size_t was = _scroll;
            if (ev.ctrl) {
                _scroll = max_scroll;
            } else {
                _scroll = (_scroll + 3 < max_scroll) ? _scroll + 3 : max_scroll;
            }
            // Перерисовку просим только если прокрутка реально сдвинулась.
            // На границе (стрелка зажата, лента уже отмотана до конца) кадр
            // ничем не отличался бы от предыдущего, а полная перерисовка ленты
            // стоит 130 мс — при автоповторе в 60 мс это и есть мерцание.
            if (_scroll != was) _redraw_feed = true;
            return false;
        }

        case keymap::Special::Down: {
            const size_t was = _scroll;
            if (ev.ctrl) {
                _scroll = 0;
            } else {
                _scroll = (_scroll > 3) ? _scroll - 3 : 0;
            }
            if (_scroll != was) _redraw_feed = true;
            return false;
        }

        case keymap::Special::Delete:
            // Del стирает ввод целиком: набирать длинный вопрос с ошибкой в
            // начале иначе пришлось бы посимвольно.
            if (_input_len > 0) {
                _input[0] = '\0';
                _input_len = 0;
                _redraw_input = true;
            }
            return false;

        default: return false;
        }
    }

    handle_typing_key(ev);
    return false;
}

void ChatScreen::handle_typing_key(const hw::KeyEvent& ev)
{
    if (!is_char(ev)) return;

    // Ctrl-комбинации не должны попадать в текст: Ctrl+Space переключает
    // раскладку, и пробел после него в поле ввода был бы мусором.
    if (ev.ctrl || ev.alt) return;

    const size_t n = strlen(ev.utf8);
    if (_input_len + n >= sizeof(_input)) {
        fail("Вопрос слишком длинный");
        return;
    }

    memcpy(_input + _input_len, ev.utf8, n);
    _input_len += n;
    _input[_input_len] = '\0';

    _error[0] = '\0';
    _redraw_error = true;
    _redraw_input = true;
}

void ChatScreen::backspace()
{
    if (_input_len == 0) return;

    // Удаляем целый UTF-8 символ: иначе кириллица рассыпалась бы на обломки.
    do {
        --_input_len;
    } while (_input_len > 0 && (static_cast<unsigned char>(_input[_input_len]) & 0xC0) == 0x80);

    _input[_input_len] = '\0';
    _redraw_input = true;
}

bool ChatScreen::onTouch(int x, int y)
{
    // Касание области ленты прокручивает её: верхняя половина — вверх,
    // нижняя — вниз. Так экран работает и без клавиатуры.
    if (y < kFeedY || y >= kFeedY + kFeedH) return false;

    const size_t was = _scroll;
    if (y < kFeedY + kFeedH / 2) {
        const size_t max_scroll = total_lines() > visible_lines() ? total_lines() - visible_lines() : 0;
        _scroll = (_scroll + visible_lines() / 2 < max_scroll) ? _scroll + visible_lines() / 2 : max_scroll;
    } else {
        _scroll = (_scroll > visible_lines() / 2) ? _scroll - visible_lines() / 2 : 0;
    }
    // Тап у границы ленты не должен ничего перерисовывать: кадр был бы
    // побайтово тем же, а полная перерисовка стоит 130 мс.
    if (_scroll != was) _redraw_feed = true;
    (void)x;
    return true;
}

// --- Отправка ---------------------------------------------------------------

bool ChatScreen::send_question()
{
    if (_mode != Mode::Idle) return false;
    if (_input_len == 0) {
        fail("Введите вопрос");
        return false;
    }
    if (_wifi == nullptr || !_wifi->connected()) {
        fail("Нет Wi-Fi: Ctrl+W — настройка сети");
        return false;
    }
    if (_settings == nullptr || _settings->secrets().api_key[0] == '\0') {
        fail("Ключ API не задан — настройки в config.json и NVS");
        return false;
    }
    if (_body == nullptr) {
        fail("Не хватило памяти под запрос");
        return false;
    }

    char ts[32];
    now_iso(ts, sizeof(ts));

    char err[128];
    if (!_store->append(api::Role::User, _input, ts[0] != '\0' ? ts : nullptr, 0, 0, err, sizeof(err))) {
        fail(err);
        return false;
    }

    // Вопрос ушёл в файл — очищаем поле только после успешной записи: иначе
    // при ошибке SD пользователь потерял бы набранный текст.
    _input[0] = '\0';
    _input_len = 0;
    _redraw_input = true;

    if (!_store->begin_answer(err, sizeof(err))) {
        fail(err);
        return false;
    }

    _answer.input_tokens = 0;
    _answer.output_tokens = 0;
    _answer.partial = false;
    _abort_overflow = false;
    _retry_attempt = 0;
    _retry_delay_s = 0;

    const size_t body_len = build_body();
    if (body_len == 0) {
        _store->drop_last();
        fail("История не собралась в запрос");
        return false;
    }

    enter_mode(Mode::Sending);

    if (!_api->start(_settings->config().api_base_url, _settings->secrets().api_key, _body, g_sink, err, sizeof(err))) {
        _store->drop_last();
        enter_mode(Mode::Idle);
        fail(err);
        return false;
    }

    _error[0] = '\0';
    _redraw_error = true;
    _scroll = 0;
    // Заголовок треда — это первое сообщение пользователя. Пока оно не было
    // в треде, в заголовке стояло «новый тред»; после этого вопроса его пора
    // перерисовать, иначе он останется таким до перезагрузки.
    _redraw_title = true;
    _redraw_feed = true;
    return true;
}

size_t ChatScreen::build_body()
{
    const size_t count = _store->count();
    if (count > hw::ChatStore::kMaxMessages) return 0;

    // Последнее сообщение — пустой ответ модели, который мы только что начали.
    // В запрос он попадать не должен.
    const size_t usable = (count > 0 && _store->at(count - 1).role == api::Role::Assistant) ? count - 1 : count;

    for (size_t i = 0; i < usable; ++i) {
        const hw::ChatMessage& m = _store->at(i);
        g_hist[i].role = m.role;
        g_hist[i].content = m.content;
        g_hist[i].content_len = m.content_len;
        g_hist[i].tool_call_id = nullptr;
        g_hist[i].name = nullptr;
    }

    const history::TrimResult trim = history::trim(g_hist, usable, kHistoryBudgetBytes);

    size_t n = 0;
    g_out[n].role = api::Role::System;
    g_out[n].content = kSystemPrompt;
    g_out[n].tool_call_id = nullptr;
    g_out[n].name = nullptr;
    ++n;

    for (size_t i = trim.begin; i < trim.end; ++i) {
        g_out[n].role = g_hist[i].role;
        g_out[n].content = g_hist[i].content;
        g_out[n].tool_call_id = g_hist[i].tool_call_id;
        g_out[n].name = g_hist[i].name;
        ++n;
    }

    if (trim.dropped > 0) {
        Serial.printf("Чат: в запрос ушло %u сообщений, отброшено %u\n", (unsigned)trim.kept, (unsigned)trim.dropped);
    }

    api::RequestOptions opt;
    const settings::Config& cfg = _settings->config();
    opt.model = cfg.api_model;
    opt.temperature_tenths = cfg.temperature_tenths;
    opt.stream = true;
    // Уровень размышлений из настроек. "none" выключает их совсем — это заметно
    // экономит выходные токены, за которые платим.
    static const char* const kEffort[] = {"none", "low", "medium", "high"};
    opt.reasoning_effort = kEffort[cfg.reasoning_effort <= 3 ? cfg.reasoning_effort : 1];

    return api::build_request(g_out, n, opt, _body, kBodyCap);
}

bool ChatScreen::append_answer(const char* text, size_t len)
{
    if (text == nullptr || len == 0) return true;

    // Текст копится сразу в тред: экран рисует ленту из ChatStore, и вторая
    // копия ответа была бы лишними 12 КБ и расхождением между двумя буферами.
    if (!_store->append_to_last(text, len)) {
        // Переполнение буфера треда — ответ помечен неполным уже в ChatStore.
        // Молча укороченный ответ выглядел бы как законченный.
        _answer.partial = true;
        return false;
    }
    return true;
}

void ChatScreen::restore_unsent_question()
{
    if (_store == nullptr) return;

    // Текст вопроса нужен ДО отката: после него сообщения в треде уже нет.
    const hw::ChatMessage* last = _store->last();
    if (last == nullptr || last->content == nullptr) return;

    char text[kInputCap];
    snprintf(text, sizeof(text), "%s", last->content);

    char err[96];
    if (!_store->undo_last_user(err, sizeof(err))) {
        // Файл обрезать не удалось: вопрос остался в треде. Это не потеря
        // данных, но и не повод молчать — иначе следующий ответ прилипнет
        // к вопросу, и пользователь не поймёт, почему.
        fail(err[0] != '\0' ? err : "Вопрос остался в треде без ответа");
        _redraw_feed = true;
        return;
    }

    // Вопрос вернулся в поле: пользователь отменил ответ, а не стёр мысль.
    const size_t n = strlen(text);
    if (n < sizeof(_input)) {
        memcpy(_input, text, n + 1);
        _input_len = n;
        _redraw_input = true;
    }

    fail("Запрос отменён до первого слова, вопрос вернулся в поле ввода");
    _redraw_feed = true;
    _redraw_title = true;
    _scroll = 0;
}

void ChatScreen::finish_answer(bool ok, const char* reason)
{
    if (_mode == Mode::Idle) return;

    const hw::ChatMessage* last = _store->last();
    const bool had_text = last != nullptr && last->content_len > 0;

    if (!had_text) {
        // Ответа нет вовсе: пустую реплику модели в тред не пишем.
        _store->drop_last();
        if (!ok) {
            fail(reason != nullptr ? reason : "Модель не ответила");
        } else if (reason != nullptr && strcmp(reason, "cancel") == 0) {
            fail("Запрос отменён до первого слова");
        } else {
            fail("Модель вернула пустой ответ");
        }
        enter_mode(Mode::Idle);
        // Сообщение уже успело нарисоваться подписью роли с пустым текстом.
        // Без перерисовки «модель» оставалась бы висеть в ленте до следующего
        // события, хотя сообщения в треде нет. Счётчики строк пересчитает
        // measure_messages(): тред стал короче, и кэш сбрасывается сам.
        _redraw_feed = true;
        _redraw_status = true;
        return;
    }

    char ts[32];
    now_iso(ts, sizeof(ts));

    if (!_store->finish_last(
            ts[0] != '\0' ? ts : nullptr, _answer.input_tokens, _answer.output_tokens, _answer.partial)) {
        _store->drop_last();
        fail("Ответ не поместился в тред");
        enter_mode(Mode::Idle);
        _redraw_feed = true;
        _redraw_status = true;
        return;
    }

    char err[128];
    if (!_store->flush_last(err, sizeof(err))) fail(err);

    if (!ok) {
        fail(reason != nullptr ? reason : "Ответ оборван");
    } else if (reason != nullptr && strcmp(reason, "length") == 0) {
        fail("Ответ обрезан лимитом токенов модели");
    }

    enter_mode(Mode::Idle);
    _redraw_feed = true;
    _redraw_status = true;
}

// --- Периодика --------------------------------------------------------------

void ChatScreen::tick()
{
    if (_api == nullptr) return;

    _api->tick();

    // Буфер ответа переполнился ещё внутри обработчика потока: там отменять
    // было нельзя, здесь — безопасно, поток уже вернул управление.
    if (_abort_overflow) {
        _abort_overflow = false;

        // Пока мы были внутри потока, запрос мог завершиться и сам (например,
        // нарушением формата). Тогда настоящая причина уже показана, и затмевать
        // её переполнением нельзя.
        if (_mode != Mode::Idle) {
            _answer.partial = true;
            _api->cancel();
            finish_answer(true, nullptr);
            fail("Ответ длиннее 12 КБ — сохранён неполным");
        }
        return;
    }

    // Заголовок показывает состояние сети (и адрес), а оно меняется
    // асинхронно: при старте сети ещё нет. Сравниваем готовую строку, а не
    // только факт подключения: адрес приходит позже самого подключения, и
    // однократной проверки «connected» не хватило бы.
    char net_now[64];
    if (_wifi != nullptr && _wifi->connected()) {
        const char* ip = _wifi->ip_text();
        snprintf(net_now, sizeof(net_now), "%s", (ip != nullptr && ip[0] != '\0') ? ip : "есть");
    } else if (_wifi != nullptr && _wifi->connecting()) {
        snprintf(net_now, sizeof(net_now), "подключение");
    } else {
        snprintf(net_now, sizeof(net_now), "нет");
    }
    if (strcmp(net_now, _net_cache) != 0) {
        snprintf(_net_cache, sizeof(_net_cache), "%s", net_now);
        _redraw_title = true;
    }

    if (_mode == Mode::Idle) return;

    // Индикатор ожидания: точки и секунды. Перерисовываем только при смене
    // значения, иначе строка статуса мигала бы на каждой итерации цикла.
    const unsigned long elapsed = millis() - _wait_started;
    const int dots = (elapsed / 400) % 4;
    const int sec = static_cast<int>(elapsed / 1000);
    if (dots != _dots || sec != _wait_sec) {
        _dots = dots;
        _wait_sec = sec;
        _redraw_status = true;
    }

    // Обратный отсчёт до повтора идёт своим временем: счётчик секунд ожидания
    // общий и меняется не каждый тик, а строка «повтор через N с» должна
    // уменьшаться.
    if (_retry_delay_s > 0) {
        const int left_s = _retry_delay_s - static_cast<int>((millis() - _retry_started_ms) / 1000);
        if (left_s != _retry_left_s) {
            _retry_left_s = left_s;
            _redraw_status = true;
        }
    }
}

bool ChatScreen::dirty() const
{
    return _redraw_all || _redraw_title || _redraw_feed || _redraw_input || _redraw_status || _redraw_error;
}

// --- События ответа ---------------------------------------------------------

void ChatScreen::on_answer_text(const char* text, size_t len)
{
    const bool appended = append_answer(text, len);

    if (_mode == Mode::Sending) enter_mode(Mode::Streaming);

    // Ответ упёрся в потолок буфера треда. Ждать конца бессмысленно: остальной
    // текст девать некуда, а каждый токен оплачивается. Обрывать здесь нельзя —
    // мы внутри обработчика потока, и cancel() уничтожил бы парсер под работающим
    // feed(). Ставим флаг, прерываем в tick().
    if (!appended) {
        _abort_overflow = true;
        return;
    }

    // Ответ растёт — держим ленту внизу, если пользователь не отмотал её вверх.
    if (_scroll == 0) _redraw_feed = true;
}

void ChatScreen::on_answer_reasoning()
{
    // Рассуждения не показываем текстом: они длинные, черновые и оплачиваются
    // отдельно. Но сам факт, что модель работает, важен — индикатор уже идёт.
    if (_mode == Mode::Sending) _redraw_status = true;
}

void ChatScreen::on_answer_usage(long input_tokens, long output_tokens)
{
    // Считаем разницу, а не прибавляем целиком: провайдер может прислать
    // usage несколько раз за поток (некоторые присылают накопительный итог в
    // каждом чанке с choices:[]), и сессия посчиталась бы дважды. Сейчас в
    // ответе это одно число — но опираться на это нельзя.
    _session_input += input_tokens - _answer.input_tokens;
    _session_output += output_tokens - _answer.output_tokens;
    _answer.input_tokens = input_tokens;
    _answer.output_tokens = output_tokens;
    _redraw_status = true;
}

void ChatScreen::on_answer_finished(const char* reason)
{
    finish_answer(true, reason);
}

void ChatScreen::on_answer_error(const char* message)
{
    finish_answer(false, message);
}

void ChatScreen::on_answer_retry(int attempt, int delay_s)
{
    _retry_attempt = attempt;
    _retry_delay_s = delay_s;
    _retry_started_ms = millis();
    _redraw_status = true;
}

// --- Измерение текста -------------------------------------------------------

int ChatScreen::feed_text_width()
{
    return M5.Display.width() - kTextX - kMarginX;
}

size_t ChatScreen::chars_per_line()
{
    // Шрифт ставим явно: измерение может случиться до первой отрисовки (стрелка
    // прокрутки нажата раньше кадра), и тогда ширина померилась бы шрифтом по
    // умолчанию, а не тем, которым рисуется лента.
    M5.Display.setFont(ui::font());

    // Ширина глифа измеряется по кириллице: она в этом шрифте половинной
    // ширины и уж точно не уже латиницы. Ошибка в эту сторону оставляет
    // неровный правый край, а в обратную — вылезает за поля ленты.
    const int glyph = M5.Display.textWidth("ШШШШШШШШШШ") / 10;
    if (glyph <= 0) return 80;

    const size_t n = static_cast<size_t>(feed_text_width() / glyph);
    return n > 0 ? n : 1;
}

size_t ChatScreen::split_lines(const char* text, size_t width, size_t* offsets, size_t max_lines)
{
    return wrap_text(text, width, offsets, max_lines);
}

uint16_t ChatScreen::count_lines(const char* text, size_t width)
{
    // Пустой текст тоже занимает строку: wrap_text для него возвращает 1, и
    // рисование ленты рисует одну пустую строку под подписью роли. Если здесь
    // вернуть 0, измерение разойдётся с отрисовкой на строку — и лента поедет
    // ровно в тот момент, когда ответ ещё пуст (только что нажали Enter).
    const size_t n = split_lines(text, width, g_offsets, kMaxLinesPerMessage);
    return static_cast<uint16_t>(n > 0xFFFF ? 0xFFFF : n);
}

size_t ChatScreen::total_lines()
{
    // Досчитываем кэш: стрелка могла нажаться раньше, чем случилась первая
    // отрисовка ленты, и без этого прокрутка считала бы, что строк нет.
    measure_messages();
    return _total_lines;
}

size_t ChatScreen::visible_lines() const
{
    return chatview::lines_per_page(kFeedH, kLineH);
}

void ChatScreen::invalidate_measure()
{
    _measured_count = 0;
    _measured_last_len = 0;
    _total_lines = 0;
    // Счётчики строк больше не отвечают тому, что нарисовано: сдвигать ленту
    // по ним нельзя.
    _feed_valid = false;
}

void ChatScreen::measure_messages()
{
    if (_line_counts == nullptr) return;

    const size_t count = _store->count();
    if (count > hw::ChatStore::kMaxMessages) {
        invalidate_measure();
        return;
    }

    // Тред стал короче — это новый тред или загрузка, кэш целиком неверен.
    if (_measured_count > count) invalidate_measure();

    // С какого сообщения считать — решает чистая функция (util/chat_measure),
    // у неё есть тесты на хосте. Здесь только чтение состояния.
    //
    // Пересчёт последнего сообщения привязан к ИЗМЕНЕНИЮ ЕГО ДЛИНЫ, а не к
    // режиму экрана. Раньше было наоборот, и ответ, пришедший целиком за один
    // tick() (быстрая модель, медленная отрисовка), оставался неизмеренным:
    // к первому кадру экран уже снова Idle. На живой плате это выглядело так:
    // ответ в 5 КБ считался за 2 строки, лента обрывалась, а стрелки не
    // двигали её — total_lines выходил меньше высоты экрана.
    const size_t start = chatmeasure::first_to_measure(
        _measured_count, count, count > 0 ? _store->at(count - 1).content_len : 0, _measured_last_len);

    // Ширину меряем один раз на проход, а не на каждое сообщение.
    const size_t width = chars_per_line();

    for (size_t i = start; i < count; ++i) {
        const hw::ChatMessage& m = _store->at(i);
        size_t lines = kLabelLines + count_lines(m.content, width);
        if (lines > kMaxLinesPerMessage) lines = kMaxLinesPerMessage;
        _line_counts[i] = static_cast<uint16_t>(lines);
    }
    _measured_count = count;
    _measured_last_len = count > 0 ? _store->at(count - 1).content_len : 0;

    size_t sum = 0;
    for (size_t i = 0; i < count; ++i)
        sum += _line_counts[i];
    _total_lines = sum;
}

void ChatScreen::now_iso(char* out, size_t out_len) const
{
    if (out == nullptr || out_len == 0) return;
    out[0] = '\0';

    // Время берём из системных часов, а не из RTC напрямую: их выставляет
    // hw/clock_sync по NTP, и на незаряженном суперконденсаторе RTC может
    // отдавать мусор, пока сеть не поднялась. Если время ещё не осмысленное,
    // метки не будет вовсе — пустая строка честнее «1970-01-01».
    const time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);

    if (!isotime::is_valid(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)) {
        return;
    }

    // Смещение уже учтено localtime_r: TZ выставлена из настроек в
    // clock_sync::begin(). Осталось только записать его явно, чтобы тред,
    // открытый в другом поясе, показывал верное время.
    int tz_minutes = 0;
    if (_settings == nullptr || !isotime::parse_tz_offset(_settings->config().timezone, &tz_minutes)) {
        tz_minutes = 0;
    }

    isotime::format_iso8601(
        out, out_len, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, tz_minutes);
}

// --- Отрисовка --------------------------------------------------------------

void ChatScreen::draw()
{
    if (_redraw_all) {
        _redraw_all = false;
        _redraw_title = _redraw_feed = _redraw_input = false;
        _redraw_status = _redraw_error = false;

        draw_all();
        return;
    }

    if (_redraw_title) {
        _redraw_title = false;
        draw_title();
    }
    if (_redraw_feed) {
        _redraw_feed = false;
        draw_feed();
    }
    if (_redraw_input) {
        _redraw_input = false;
        draw_input_field();
    }
    if (_redraw_status) {
        _redraw_status = false;
        draw_status();
    }
    if (_redraw_error) {
        _redraw_error = false;
        draw_error();
    }
}

void ChatScreen::draw_all()
{
    M5.Display.fillScreen(TFT_BLACK);
    draw_title();
    draw_feed();
    draw_input_field();
    draw_status();
    draw_error();
    draw_hints();
}

void ChatScreen::draw_title()
{
    M5.Display.fillRect(0, 0, M5.Display.width(), kFeedY - 4, TFT_BLACK);

    // Статус сети слева от правого края показываем ВСЕГДА: пустое место
    // читается как «экран сломался», а «нет Wi-Fi» без адреса не отвечает на
    // вопрос «а сейчас-то связь есть?».
    char net[64];
    uint16_t net_color = kColorPartial;
    if (_wifi != nullptr && _wifi->connected()) {
        const char* ip = _wifi->ip_text();
        if (ip != nullptr && ip[0] != '\0') {
            snprintf(net, sizeof(net), "Wi-Fi %s", ip);
        } else {
            snprintf(net, sizeof(net), "Wi-Fi есть");
        }
        net_color = kColorAssistant;
    } else if (_wifi != nullptr && _wifi->connecting()) {
        snprintf(net, sizeof(net), "Wi-Fi: подключение…");
    } else {
        snprintf(net, sizeof(net), "нет Wi-Fi");
    }

    // Ширину статуса мерим, а не угадываем: адрес бывает длинным, и зашитый
    // отступ то оставлял бы пустую полосу, то наезжал на заголовок.
    M5.Display.setFont(ui::font());
    const int net_w = M5.Display.textWidth(net);
    const int title_room = M5.Display.width() - 2 * kMarginX - net_w - 16;

    char title[96];
    char name[48];
    if (_store->title(name, sizeof(name))) {
        snprintf(title, sizeof(title), "Чат · %s", name);
    } else {
        snprintf(title, sizeof(title), "Чат · новый тред");
    }

    // Обрезаем по ширине, а не по числу байт: вопрос бывает длинным, а
    // разрезанная посреди UTF-8 строка дала бы мусорный глиф.
    static char title_buf[128];
    ui::text(ui::fit_width(title, title_room, title_buf, sizeof(title_buf)), kMarginX, kTitleY, TFT_WHITE);
    ui::text_right(net, M5.Display.width() - kMarginX, kTitleY, net_color);
}

void ChatScreen::draw_scrollbar(size_t total, size_t max_lines)
{
    // Полоса живёт в правом поле, правее текста. Ширина копирования при сдвиге
    // ограничена feed_w - kMarginX, поэтому полоса не двоится и не мерцает.
    const int x = M5.Display.width() - kMarginX + kScrollGap / 2;
    const int track_top = kFeedY;
    const int track_h = kFeedH;

    M5.Display.fillRect(x, track_top, kScrollW, track_h, kColorFrame);

    if (total <= max_lines) return;  // всё помещается — ползунок не нужен

    // visible/max_scroll — доля треда в окне; остаток трека — длина ползунка.
    int thumb_h = static_cast<int>(static_cast<long>(track_h) * max_lines / total);
    if (thumb_h < kLineH) thumb_h = kLineH;  // иначе ползунок был бы микроскопическим

    const size_t max_scroll = total - max_lines;
    const int travel = track_h - thumb_h;
    const size_t offset = max_scroll > _scroll ? max_scroll - _scroll : 0;  // 0 — самый низ
    const int thumb_y = track_top + static_cast<int>(static_cast<long>(travel) * offset / max_scroll);

    M5.Display.fillRect(x, thumb_y, kScrollW, thumb_h, kColorDim);
}

void ChatScreen::draw_feed()
{
    measure_messages();

    const size_t count = _store->count();
    const size_t max_lines = visible_lines();
    const int feed_w = M5.Display.width();
    const int feed_bottom = kFeedY + kFeedH;

    // Клип по области ленты. Он обязательно нужен по вертикали: перенос по
    // числу символов приблизителен (в шрифте есть и полуширинные, и
    // полноширинные глифы), и без него строка вылезала бы в поле ввода.
    //
    // По горизонтали клип на всю ширину, а не по полям. С запасом в kMarginX
    // обрезалась бы и заливка строки (она идёт от края до края), и полосы по
    // 16 px слева и справа не обновлялись бы никогда — оставался бы мусор от
    // прошлого кадра. За пределы экрана текст и так не выйдет.
    M5.Display.setClipRect(0, kFeedY, feed_w, kFeedH);

    if (count == 0) {
        M5.Display.fillRect(0, kFeedY, feed_w, kFeedH, TFT_BLACK);
        ui::text("Пусто. Наберите вопрос и нажмите Enter.", kTextX, kFeedY + 8, kColorFaint);
        M5.Display.clearClipRect();
        _feed_valid = false;
        return;
    }

    const size_t total = _total_lines;

    // Прокрутка — самый частый случай перерисовки ленты: стрелка при
    // автоповторе шлёт событие каждые 60 мс. Перерисовывать ленту целиком
    // нельзя: кадр занимает 120–170 мс против 60 мс между событиями, и экран
    // никогда не успевает дорисоваться — это и читалось как мерцание.
    // Сдвигаем уже нарисованное и дорисовываем открывшуюся полоску.
    if (shift_feed(total, max_lines)) {
        M5.Display.clearClipRect();
        return;
    }

    const chatview::Window win = chatview::compute(_line_counts, count, max_lines, _scroll);
    if (win.first_msg < 0) {
        M5.Display.fillRect(0, kFeedY, feed_w, kFeedH, TFT_BLACK);
        M5.Display.clearClipRect();
        _feed_valid = false;
        return;
    }

    // Отдельной заливки всей ленты здесь нет намеренно: paint_feed закрашивает
    // свою полосу полностью — пустое место над строками, каждую строку и
    // остаток снизу. Сплошная заливка на 33 мс показывала пустой экран поверх
    // ещё не нарисованного текста, и это было видно как мерцание.
    paint_feed(win, kFeedY, feed_bottom);

    M5.Display.clearClipRect();

    draw_scrollbar(total, max_lines);

    remember_feed(total);
}

void ChatScreen::remember_feed(size_t total)
{
    _drawn_scroll = _scroll;
    _drawn_total = total;
    _feed_valid = true;
}

bool ChatScreen::shift_feed(size_t total, size_t max_lines)
{
    if (!_feed_valid) return false;
    if (_drawn_total != total) return false;
    if (_drawn_scroll == _scroll) return false;

    const size_t count = _store->count();
    const int dy = static_cast<int>(_scroll) - static_cast<int>(_drawn_scroll);
    const int shift = dy * kLineH;

    // Сдвиг имеет смысл, только когда лента занимает экран целиком: иначе
    // содержимое прижато к низу и всё равно перерисовывается целиком. И окно
    // не должно поменять «форму» — при разном lines_used строки лягут в другие
    // y, и сдвиг разъехался бы с содержимым.
    const chatview::Window old_win = chatview::compute(_line_counts, count, max_lines, _drawn_scroll);
    if (old_win.first_msg < 0 || old_win.lines_used != max_lines) return false;

    if (shift >= kFeedH || shift <= -kFeedH) return false;

    const chatview::Window win = chatview::compute(_line_counts, count, max_lines, _scroll);
    if (win.first_msg < 0 || win.lines_used != max_lines) return false;

    const int feed_w = M5.Display.width();

    // Копируем только поле текста, не доходя до полосы прокрутки в правом
    // поле. Иначе полоса уехала бы вместе с лентой и на время сдвига
    // оказалась бы в двух местах.
    const int shift_w = feed_w - kMarginX;

    if (shift > 0) {
        // Прокрутка вверх (к старым): изображение уезжает вниз, сверху
        // открывается полоса.
        M5.Display.copyRect(0, kFeedY + shift, shift_w, kFeedH - shift, 0, kFeedY);
        paint_feed(win, kFeedY, kFeedY + shift);
    } else {
        // Прокрутка вниз (к новым): изображение уезжает вверх, полоса
        // открывается снизу.
        const int d = -shift;
        M5.Display.copyRect(0, kFeedY, shift_w, kFeedH - d, 0, kFeedY + d);
        paint_feed(win, kFeedY + kFeedH - d, kFeedY + kFeedH);
    }

    draw_scrollbar(total, max_lines);

    remember_feed(total);
    return true;
}

void ChatScreen::paint_feed(const chatview::Window& win, int band_top, int band_bottom)
{
    const size_t count = _store->count();
    const size_t max_lines = visible_lines();
    const size_t width = chars_per_line();
    const int feed_w = M5.Display.width();
    const int feed_bottom = kFeedY + kFeedH;

    // Строки прижаты к низу области: так свежий ответ виден целиком.
    int y = kFeedY + static_cast<int>(max_lines - win.lines_used) * kLineH;

    // Пустое место над лентой попадает в полосу — его тоже надо почистить.
    if (band_top < y) {
        const int from = band_top > kFeedY ? band_top : kFeedY;
        const int to = y < band_bottom ? y : band_bottom;
        if (to > from) M5.Display.fillRect(0, from, feed_w, to - from, TFT_BLACK);
    }

    int drawn = 0;

    for (size_t i = static_cast<size_t>(win.first_msg); i < count && static_cast<size_t>(drawn) < win.visible_count;
        ++i, ++drawn) {
        const hw::ChatMessage& m = _store->at(i);

        const uint16_t color = role_color(m.role, m.partial);
        const size_t lines = split_lines(m.content, width, g_offsets, kMaxLinesPerMessage);

        // Подпись роли — строка 0 сообщения, текст начинается со строки 1.
        const size_t first_text_line = (i == static_cast<size_t>(win.first_msg)) ? win.first_line : 0;

        if (first_text_line == 0) {
            if (y + kLineH > band_top && y < band_bottom) {
                paint_row(
                    y, feed_w, role_label(m.role, m.partial), kTextX, m.partial ? kColorPartial : kColorFaint, color);
            }
            y += kLineH;
        }

        const size_t from = first_text_line > 0 ? first_text_line - 1 : 0;

        for (size_t ln = from; ln < lines; ++ln) {
            // Ушли за нижний край области — дальше рисовать нечего: без этой
            // проверки цикл проходил бы все 400 строк длинного ответа, рисуя
            // их за пределами ленты и тратя время кадра.
            if (y >= feed_bottom) break;

            if (y + kLineH > band_top && y < band_bottom) {
                const size_t begin = g_offsets[ln];
                const size_t end = (ln + 1 < lines) ? g_offsets[ln + 1] : strlen(m.content);

                size_t len = end > begin ? end - begin : 0;
                const char* p = m.content + begin;

                // Переводы строк вырезаем: их уже учёл перенос.
                while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
                    --len;

                // Копия нужна, чтобы оборвать строку нулём. Обрезаем по границе
                // UTF-8: разрезанная пополам кириллица дала бы мусорный глиф.
                size_t copy = len < sizeof(g_line_buf) - 1 ? len : sizeof(g_line_buf) - 1;
                while (copy > 0 && copy < len && (static_cast<unsigned char>(p[copy]) & 0xC0) == 0x80)
                    --copy;

                memcpy(g_line_buf, p, copy);
                g_line_buf[copy] = '\0';

                paint_row(y, feed_w, g_line_buf, kTextX, color, color);
            }

            y += kLineH;
        }

        if (y >= feed_bottom) break;
    }

    // Остаток снизу: 510 не делится на 22 без остатка, и без этой заливки
    // последние 4 px оставались бы от прошлого кадра.
    if (band_bottom > y) {
        const int from = y > band_top ? y : band_top;
        if (band_bottom > from) M5.Display.fillRect(0, from, feed_w, band_bottom - from, TFT_BLACK);
    }
}

void ChatScreen::paint_row(int y, int feed_w, const char* text, int text_x, uint16_t fg, uint16_t bar_color)
{
    // Строку заливаем ЦЕЛИКОМ перед текстом, а не одной большой заливкой на
    // всю ленту. Заливка всей ленты — это 33 мс, в течение которых на экране
    // пусто: панель TAB5 сканирует единственный фреймбуфер, поэтому такая
    // заливка видна как чёрная волна. Построчно пустой остаётся одна строка
    // на пару миллисекунд — глаз этого не замечает.
    M5.Display.fillRect(0, y, feed_w, kLineH, TFT_BLACK);

    // Фон под каждым глифом не нужен: строка уже залита. Это почти вдвое
    // дешевле (на кадр ленты 125 мс против 222).
    ui::text_plain(text, text_x, y, fg);

    // Маркер ставим и у продолжений: видно, где кончается чужое сообщение и
    // начинается следующее.
    M5.Display.fillRect(kMarginX, y, kBarW, kLineH, bar_color);
}

void ChatScreen::draw_input_field()
{
    M5.Display.fillRect(0, kInputY - 6, M5.Display.width(), kInputH + 12, TFT_BLACK);
    M5.Display.drawRect(kMarginX, kInputY, M5.Display.width() - 2 * kMarginX, kInputH, kColorFrame);

    const int text_y = kInputY + (kInputH - 16) / 2;
    const int text_x = kMarginX + 10;
    const int room = M5.Display.width() - 2 * kMarginX - 30;

    if (_input_len == 0) {
        if (_mode == Mode::Idle) {
            ui::text("Вопрос…", text_x, text_y, kColorFaint);
        } else {
            ui::text("Ждите ответа · Esc — прервать", text_x, text_y, kColorFaint);
        }
        return;
    }

    // Показываем хвост ввода: при длинном вопросе важен конец, который
    // пользователь только что набрал, а не начало.
    size_t start = 0;
    if (M5.Display.textWidth(_input) > room) {
        // Идём с конца назад, набирая ширину, пока не переполним поле. Каждый
        // символ меряем ровно один раз: цикл с strlen на каждом шаге был бы
        // квадратичным на двухкилобайтном вопросе.
        int acc = 0;
        size_t p = _input_len;
        while (p > 0) {
            size_t prev = p - 1;
            while (prev > 0 && (static_cast<unsigned char>(_input[prev]) & 0xC0) == 0x80)
                --prev;

            char ch[5];
            const size_t clen = p - prev;
            memcpy(ch, _input + prev, clen);
            ch[clen] = '\0';

            acc += M5.Display.textWidth(ch);
            p = prev;
            if (acc > room) break;
        }
        start = p;
    }

    ui::text(_input + start, text_x, text_y, TFT_WHITE);

    // Курсор в конце ввода.
    const int caret_x = text_x + M5.Display.textWidth(_input + start) + 1;
    M5.Display.fillRect(caret_x, text_y, 2, 17, TFT_WHITE);
}

void ChatScreen::draw_status()
{
    M5.Display.fillRect(0, kStatusY, M5.Display.width(), kLineH + 2, TFT_BLACK);

    char left[128];
    switch (_mode) {
    case Mode::Idle: left[0] = '\0'; break;
    case Mode::Sending: {
        if (_retry_delay_s > 0) {
            // Обратный отсчёт, а не просто «ждём»: видно, что пауза конечна и
            // клиент работает. Попытка тоже видна — по ней понятно, первая она
            // или уже третья.
            const unsigned long elapsed = (millis() - _retry_started_ms) / 1000;
            int left_s = _retry_delay_s - static_cast<int>(elapsed);
            if (left_s < 0) left_s = 0;
            snprintf(left, sizeof(left), "сервер занят · повтор %d через %d с", _retry_attempt, left_s);
            break;
        }

        // Точки «думаю…» — единственный признак того, что запрос живой: при
        // reasoning_effort первый текст приходит через полторы секунды.
        static const char* const kDots[] = {"", ".", "..", "..."};
        const int d = _dots >= 0 && _dots <= 3 ? _dots : 0;
        snprintf(left, sizeof(left), "думаю%s  %d с", kDots[d], _wait_sec > 0 ? _wait_sec : 0);
        break;
    }
    case Mode::Streaming: snprintf(left, sizeof(left), "печатает…  %d с", _wait_sec > 0 ? _wait_sec : 0); break;
    }

    if (left[0] != '\0') ui::text(left, kMarginX, kStatusY, kColorAssistant);

    // Токены: последний ответ и сумма за сессию. Считать их самому не нужно —
    // сервер присылает usage в конце потока.
    char right[160];
    char last[24];
    char session[24];
    const long in = _answer.input_tokens;
    const long out = _answer.output_tokens;
    format_tokens(static_cast<unsigned long>(in + out), last, sizeof(last));
    format_tokens(static_cast<unsigned long>(_session_input + _session_output), session, sizeof(session));
    snprintf(right, sizeof(right), "токены: %s (ответ) · %s (сессия)", last, session);
    ui::text_right(right, M5.Display.width() - kMarginX, kStatusY, kColorFaint);
}

void ChatScreen::draw_error()
{
    M5.Display.fillRect(0, kErrorY, M5.Display.width(), 30, TFT_BLACK);
    if (_error[0] == '\0') return;

    // Ошибку показываем целиком, а не обрывком: в ней обычно адрес или код,
    // и потерянная половина делает её бесполезной.
    ui::text(_error, kMarginX, kErrorY, kColorPartial);
}

void ChatScreen::draw_hints()
{
    M5.Display.fillRect(0, kHintY - 6, M5.Display.width(), 30, TFT_BLACK);

    // Две колонки: слева — что делает текущий экран, справа — куда можно уйти.
    // Одной строкой всё не помещалось бы без сокращений до нечитаемого, а
    // «Ctrl+A — настройки» на этом экране нигде больше не написано: пока
    // подсказки не было, про настройки и Wi-Fi можно было узнать только из
    // документации.
    //
    // «Esc — очистить ввод» сказано полностью: рядом появился Ctrl+R со
    // словом «сброс», и голое «Esc — очистить» читалось бы как то же самое.
    ui::text("Enter — отправить · Esc — очистить ввод · ↑↓ — прокрутка", kMarginX, kHintY, kColorFaint);
    ui::text_right("Ctrl+T — треды · Ctrl+N — новый · Ctrl+R — сброс · Ctrl+A — настройки · Ctrl+W — Wi-Fi",
        M5.Display.width() - kMarginX, kHintY, kColorFaint);
}

}  // namespace ui
