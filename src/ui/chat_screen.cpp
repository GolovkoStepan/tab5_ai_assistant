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
#include "hw/clock.h"
#include "ui/draw.h"
#include "ui/tray.h"
#include "util/chat_history.h"
#include "util/chat_measure.h"
#include "util/chat_view.h"
#include "util/format.h"
#include "util/markdown.h"
#include "util/offline.h"

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
    "вместо них рисуются пустые квадраты. "
    // Разметку экран не понимает: markdown-звено «[mi.com](https://…)" видно
    // человеку как скобки и сырой адрес. Список источников устройство дописывает
    // к ответу само, поэтому модель, перечисляя ссылки, дублировала его — в треде
    // это выглядело двумя блоками «Источники:» подряд (проверено 26.11.2026).
    "Не используй markdown-разметку: ни выделения, ни ссылок вида [текст](адрес). "
    "Не перечисляй источники сам — устройство добавляет список ссылок к ответу автоматически. "
    "У тебя есть архив документов на устройстве (инструменты list_docs, read_doc, "
    "search_docs, write_doc, delete_doc). Перед ответом на вопрос про бытовую технику, "
    "настройки приборов или инструкции сначала поищи в архиве: возможно, ответ там уже "
    "есть. Когда находишь решение, которое стоит запомнить, сохраняй его через write_doc: "
    "заголовок по-русски, путь ASCII-слагом латиницей. Не удаляй документы без просьбы.";

constexpr uint16_t kColorUser = TFT_CYAN;
constexpr uint16_t kColorAssistant = 0x07E0;  // зелёный
constexpr uint16_t kColorPartial = TFT_ORANGE;

constexpr uint16_t kColorDim = TFT_LIGHTGRAY;
constexpr uint16_t kColorFaint = TFT_DARKGRAY;
constexpr uint16_t kColorFrame = 0x18E3;

// Ошибка и подсказка раньше рисовались одинаково — одна оранжевая строка.
// Из-за этого отказ сервера («Лимит трат по ключу исчерпан…») читался как
// обычное «введите вопрос», и владелец не понял, что случилось.
// Теперь у ошибки тёмно-красная подложка на всю ширину и белый текст: её видно
// боковым зрением, не вычитывая буквы. Подсказка осталась тихой.
constexpr uint16_t kColorError = TFT_WHITE;
constexpr uint16_t kColorErrorBg = 0x7800;  // TFT_MAROON

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

/// Текст сообщения в том виде, в котором его видно в ленте.
///
/// Результат инструмента бывает на 4 КБ (документ целиком), и показывать его
/// в ленте — значит утопить разговор: пользователь листал бы простыню
/// markdown вместо своих вопросов. Показываем одну строку: имя инструмента и
/// начало результата. Модели при этом уходит полный текст — в запрос он
/// берётся из треда, а не отсюда.
///
/// У ответов модели снимается markdown-разметка (`md::plain`): экран её не
/// понимает, и `[mi.com](https://…)` читался как скобки и сырой адрес.
/// Чистим только показ — в треде и в истории запроса текст остаётся исходным.
const char* shown_text(const hw::ChatMessage& m)
{
    // Буфер под разметку нужен размером с сообщение: длинный ответ не влез бы
    // в общий 1 КБ буфер строки, а обрезанный вернул бы пустоту — сообщение
    // исчезло бы из ленты. Статический: 12 КБ на стеке задачи интерфейса
    // недопустимы.
    static char cleaned[hw::ChatStore::kMaxContentBytes + 1];

    if (m.role == api::Role::Tool && m.content != nullptr) {
        // Первая строка результата: для ответов вида «Создан документ home/x.md»
        // это и есть весь смысл.
        static char one_line[256];
        size_t n = 0;
        while (m.content[n] != '\0' && m.content[n] != '\n' && m.content[n] != '\r' && n + 1 < sizeof(one_line))
            ++n;

        snprintf(one_line, sizeof(one_line), "%s · %.*s%s", m.name[0] != '\0' ? m.name : "инструмент",
            static_cast<int>(n), m.content, m.content[n] == '\0' ? "" : " …");
        return one_line;
    }

    if (m.content == nullptr) return "";
    if (!md::needed(m.content, m.content_len)) return m.content;

    // Не поместилось (не должно: разметка только сокращает текст) — показываем
    // как есть. Потерять сообщение хуже, чем показать его с разметкой.
    if (md::plain(m.content, m.content_len, cleaned, sizeof(cleaned)) == 0) return m.content;
    return cleaned;
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
    void on_tool_calls(const tools::Accumulator& calls) override { _screen->on_tool_calls(calls); }
    void on_web_sources(const web::List& sources) override { _screen->on_web_sources(sources); }

private:
    ChatScreen* _screen;
};

AnswerSink* g_sink = nullptr;

/// Слушатель поискового подзапроса (Р-040).
///
/// Отдельный от AnswerSink: события поиска идут в СВОЙ буфер, а не в ленту —
/// открытой реплики ассистента в этот момент нет, и текст поисковика не должен
/// попасть в тред. Источники копятся тоже отдельно, а в общий список экрана
/// они переезжают в finish_search(): только тогда видно, что принёс именно
/// этот поиск.
class SearchSink : public hw::ApiClient::Listener {
public:
    explicit SearchSink(ChatScreen* screen) : _screen(screen) {}

    void on_text(const char* text, size_t len) override { _screen->on_search_text(text, len); }
    void on_usage(long input_tokens, long output_tokens, long) override
    {
        _screen->on_search_usage(input_tokens, output_tokens);
    }
    void on_finished(const char* reason) override { _screen->on_search_finished(reason); }
    void on_retry(int attempt, int delay_s) override { _screen->on_search_retry(attempt, delay_s); }
    void on_error(const char* message) override { _screen->on_search_error(message); }
    // Источники идут в общий список хода: он один на ход, дедуплицирует по URL,
    // и в конце именно из него собирается блок «Источники:» и `source_url`
    // заметки. Отдельный список дал бы два источника правды.
    void on_web_sources(const web::List& sources) override { _screen->on_web_sources(sources); }

private:
    ChatScreen* _screen;
};

SearchSink* g_search_sink = nullptr;

}  // namespace

// --- Жизненный цикл ---------------------------------------------------------

void ChatScreen::init(
    hw::ApiClient& api, hw::ChatStore& store, hw::SettingsStore& settings, hw::WifiManager& wifi, hw::DocStore& docs)
{
    _api = &api;
    _store = &store;
    _settings = &settings;
    _wifi = &wifi;
    _docs = &docs;

    // Тело запроса в PSRAM: история легко даёт десятки килобайт, а рядом с TLS
    // во внутренней RAM столько не найти.
    _body = static_cast<char*>(heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM));
    if (_body == nullptr) _body = static_cast<char*>(malloc(kBodyCap));

    // Буфер счётчиков строк обнуляем сразу: если измерение по какой-то причине
    // не дошло до сообщения, ноль оставит в ленте одну строку вместо мусорного
    // числа из кучи — это заметно, но не превращает окно ленты в хаос.
    _line_counts = static_cast<uint16_t*>(calloc(hw::ChatStore::kMaxMessages, sizeof(uint16_t)));

    if (g_sink == nullptr) g_sink = new AnswerSink(this);

    // --- Агент: инструменты архива -----------------------------------------
    //
    // Всё тяжёлое — в PSRAM. На стеке задачи интерфейса 8 КБ, а здесь только
    // описания вызовов занимают 34 КБ (8 вызовов по 4 КБ аргументов).
    _agent_calls = static_cast<tools::Accumulator*>(
        static_cast<void*>(heap_caps_malloc(sizeof(tools::Accumulator), MALLOC_CAP_SPIRAM)));
    if (_agent_calls == nullptr) _agent_calls = static_cast<tools::Accumulator*>(malloc(sizeof(tools::Accumulator)));
    if (_agent_calls != nullptr) new (_agent_calls) tools::Accumulator();

    _tools_json = static_cast<char*>(heap_caps_malloc(kToolsJsonCap, MALLOC_CAP_SPIRAM));
    if (_tools_json == nullptr) _tools_json = static_cast<char*>(malloc(kToolsJsonCap));

    _calls_json = static_cast<char*>(heap_caps_malloc(kCallsJsonCap, MALLOC_CAP_SPIRAM));
    if (_calls_json == nullptr) _calls_json = static_cast<char*>(malloc(kCallsJsonCap));

    // 64 байта — можно и на стеке, но буфер живёт весь сеанс: пусть лежит
    // рядом с остальными в PSRAM, чтобы не путать источники выделения.
    // 4 КБ — можно и на стеке, но буфер живёт весь сеанс: пусть лежит
    // рядом с остальными в PSRAM, чтобы не путать источники выделения.
    _search_body = static_cast<char*>(heap_caps_malloc(kSearchBodyCap, MALLOC_CAP_SPIRAM));
    if (_search_body == nullptr) _search_body = static_cast<char*>(malloc(kSearchBodyCap));
    if (_search_body != nullptr) _search_body[0] = '\0';

    _tool_out = static_cast<char*>(heap_caps_malloc(tools::kMaxResultBytes + 1, MALLOC_CAP_SPIRAM));
    if (_tool_out == nullptr) _tool_out = static_cast<char*>(malloc(tools::kMaxResultBytes + 1));

    _tool_body = static_cast<char*>(heap_caps_malloc(hw::DocStore::kMaxDocBytes + 1, MALLOC_CAP_SPIRAM));
    if (_tool_body == nullptr) _tool_body = static_cast<char*>(malloc(hw::DocStore::kMaxDocBytes + 1));

    _doc_entries = static_cast<docapi::Entry*>(
        static_cast<void*>(heap_caps_malloc(docapi::kMaxListedDocs * sizeof(docapi::Entry), MALLOC_CAP_SPIRAM)));
    if (_doc_entries == nullptr) {
        _doc_entries = static_cast<docapi::Entry*>(malloc(docapi::kMaxListedDocs * sizeof(docapi::Entry)));
    }

    _doc_hits =
        static_cast<docapi::Hit*>(static_cast<void*>(heap_caps_malloc(16 * sizeof(docapi::Hit), MALLOC_CAP_SPIRAM)));
    if (_doc_hits == nullptr) _doc_hits = static_cast<docapi::Hit*>(malloc(16 * sizeof(docapi::Hit)));

    // Источники веб-поиска: пять записей по ~600 байт — тоже только в PSRAM.
    _web_sources = static_cast<web::List*>(static_cast<void*>(heap_caps_malloc(sizeof(web::List), MALLOC_CAP_SPIRAM)));
    if (_web_sources == nullptr) _web_sources = static_cast<web::List*>(malloc(sizeof(web::List)));
    if (_web_sources != nullptr) new (_web_sources) web::List();

    // Текст поискового подзапроса (Р-040). 4 КБ — столько хватает на пересказ
    // в тысячу символов с большим запасом; в PSRAM, рядом с остальными.
    _search_text = static_cast<char*>(heap_caps_malloc(kSearchTextCap, MALLOC_CAP_SPIRAM));
    if (_search_text == nullptr) _search_text = static_cast<char*>(malloc(kSearchTextCap));
    if (_search_text != nullptr) _search_text[0] = '\0';

    doctools::register_all(_tools);
    wsearch::register_tool(_tools);
    if (_tools_json != nullptr) {
        // Учитываем настройку сразу: если поиск выключен в config.json, при
        // загрузке схема не должна содержать `web_search` — иначе модель звала
        // бы его до первого переключения в Ctrl+A, и мы платили бы за поиск,
        // который пользователь выключил.
        const bool search_on = _settings->config().search_enabled;
        if (!_tools.build_tools_json(_tools_json, kToolsJsonCap, search_on ? nullptr : "web_search")) {
            // Не влезли — работаем без инструментов: чат важнее агента, и
            // молчаливое «модель не умеет искать» лучше, чем отказ экрана.
            Serial.println("Агент: схемы инструментов не поместились, работаем без них");
            _tools_json[0] = '\0';
        } else {
            Serial.printf(
                "Агент: %u инструментов, схемы %u байт\n", (unsigned)_tools.count(), (unsigned)strlen(_tools_json));
        }
    }
    _tools_search_on = _settings->config().search_enabled;
}

void ChatScreen::onEnter()
{
    _error[0] = '\0';
    _failed = false;

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
        notice("Тред и так пуст");
        return;
    }

    if (!confirmed) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Ctrl+R ещё раз — очистить тред и удалить %u сообщений с SD", (unsigned)n);
        notice(msg);
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
    notice("Тред очищен");
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
        notice("Дождитесь ответа или нажмите Esc");
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
    _failed = false;
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
    _failed = true;
    _redraw_error = true;
    // В монитор — с префиксом: по жалобе «вылезла непонятная ошибка» нужно
    // видеть, что именно пришло от сервера. Экран показывает текст, но не
    // сохраняет его в тред, а Serial остаётся при владельце.
    Serial.printf("Чат: ошибка — %s\n", _error);
}

void ChatScreen::notice(const char* text)
{
    snprintf(_error, sizeof(_error), "%s", text);
    _failed = false;
    _redraw_error = true;
}

void ChatScreen::shutdown_save()
{
    if (_mode == Mode::Idle) {
        // Ответа нет — дописывать нечего. Но если остался несохранённый хвост
        // (например, агентский ход уже завершил текст, а flush не прошёл),
        // сохраняем и его: при выключении он пропадёт вместе с памятью.
        if (_store != nullptr && _store->has_unsaved()) {
            char err[96];
            if (!_store->flush_last(err, sizeof(err))) Serial.printf("Чат: хвост треда не сохранён — %s\n", err);
        }
        return;
    }

    // Запрос идёт: обрываем его так же, как по Esc, и сохраняем столько, сколько
    // модель успела сказать. finish_answer(true) ставит пометку «неполный» —
    // иначе после такой записи нельзя отличить целый ответ от оборванного.
    const hw::ChatMessage* last = _store != nullptr ? _store->last() : nullptr;
    const bool has_text = last != nullptr && last->role == api::Role::Assistant && last->content_len > 0;

    if (_api != nullptr && _api->busy()) _api->cancel();

    if (has_text) {
        _answer.partial = true;
        finish_answer(true, "shutdown");
        Serial.println("Чат: ответ сохранён неполным перед выключением");
        return;
    }

    // Текста нет: пустую реплику модели в файл писать незачем, она
    // неотличима от «модель ничего не сказала».
    if (last != nullptr && last->role == api::Role::Assistant) _store->drop_last();
    Serial.println("Чат: ответа не было, ход отменён");
}

void ChatScreen::fill_model_status(led::Status& s) const
{
    // Подтверждение — не «модель работает»: светодиод не должен гореть синим,
    // пока ждём решения человека.
    s.busy = _mode == Mode::Sending || _mode == Mode::Streaming || _mode == Mode::Searching;
    s.streaming = _mode == Mode::Streaming;
    // Пауза перед повтором: счётчик уже сброшен при выходе из Sending, но
    // пока идёт обратный отсчёт, режим всё ещё Sending.
    s.retrying = (_mode == Mode::Sending || _mode == Mode::Searching) && _retry_delay_s > 0;
    s.error = _failed;
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
            notice("Дождитесь ответа или нажмите Esc");
        }
        return false;
    }

    // Ctrl+R — очистка открытого треда. «к» — та же клавиша в ЙЦУКЕН.
    //
    // Сброс необратим, поэтому первое нажатие только спрашивает, второе —
    // чистит.
    if (ev.ctrl && is_char(ev) && (strcmp(ev.utf8, "r") == 0 || strcmp(ev.utf8, "к") == 0)) {
        if (_mode != Mode::Idle) {
            notice("Дождитесь ответа или нажмите Esc");
            return false;
        }
        clear_thread(clear_was_armed);
        return false;
    }

    // Подтверждение удаления агентом: Enter — удалить, Esc — отклонить. Всё
    // остальное игнорируется: вопрос необратимый, и случайная клавиша не
    // должна ни удалить файл, ни оставить агента без ответа.
    if (_mode == Mode::Confirming) {
        if (ev.type == hw::KeyEvent::Type::Special && ev.special == keymap::Special::Enter) {
            resolve_delete(true);
        } else if (ev.type == hw::KeyEvent::Type::Special && ev.special == keymap::Special::Escape) {
            resolve_delete(false);
        }
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

            // Цикл агента обрываем вместе с запросом: иначе после Esc он
            // продолжил бы выполнять инструменты и писать в тред результаты,
            // которых пользователь уже не ждёт.
            //
            // Ходом агента считаем не только «выполняется вызов»
            // (_agent_pending), но и «ждём ответа на шаг» (_agent_step > 0):
            // иначе Esc на запросе после инструментов увёл бы ход в
            // restore_unsent_question(), а откатывать там нечего — вопрос
            // пользователя давно в середине треда, и последним лежит
            // результат инструмента. Получилось бы «вопрос остался без
            // ответа» на ровном месте.
            const bool agent_turn = _agent_pending || _search_pending || _agent_step > 0;
            _have_calls = false;
            _turn_ended = false;
            _agent_pending = false;
            _search_pending = false;
            _search_ended = false;
            _confirm_index = static_cast<size_t>(-1);

            if (agent_turn) {
                const hw::ChatMessage* last = _store->last();
                const bool has_text = last != nullptr && last->role == api::Role::Assistant && last->content_len > 0;

                if (_api != nullptr && _api->busy()) _api->cancel();

                if (has_text) {
                    // Модель успела что-то сказать — текст сохраняем, как и в
                    // обычном ответе, с пометкой неполного.
                    _answer.partial = true;
                    finish_answer(true, nullptr);
                    notice("Ответ прерван, сохранён неполным");
                    return false;
                }

                // Незавершённая пустая реплика модели осталась бы в ленте
                // подписью «модель» без текста. Убираем её.
                if (last != nullptr && last->role == api::Role::Assistant) _store->drop_last();
                // Ход брошен — итог поиска в статусе («поиск не удался») врал
                // бы: поиск не провалился, его просто прервали.
                _web_search_ran = false;
                _web_search_failed = false;
                enter_mode(Mode::Idle);
                notice("Агент прерван, выполненные шаги сохранены");
                _redraw_feed = true;
                _redraw_status = true;
                return false;
            }

            _answer.partial = true;
            if (_api != nullptr) _api->cancel();
            finish_answer(true, "cancel");

            if (had_text) {
                notice("Ответ прерван, сохранён неполным");
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
        notice("Вопрос слишком длинный");
        return;
    }

    memcpy(_input + _input_len, ev.utf8, n);
    _input_len += n;
    _input[_input_len] = '\0';

    _error[0] = '\0';
    _failed = false;
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

/// Отправить вопрос, пришедший извне (команда `ask` по Serial).
///
/// Путь тот же, что у набранного текста: кладём в буфер ввода и зовём
/// send_question(). Отдельной ветки отправки нет намеренно — иначе проверки
/// «нет Wi-Fi», «нет ключа», «идёт ответ» пришлось бы повторять, и они бы
/// разъехались.
bool ChatScreen::ask(const char* text, size_t len)
{
    if (text == nullptr || len == 0) {
        notice("Пустой вопрос");
        return false;
    }
    if (_mode != Mode::Idle) {
        notice("Дождитесь ответа или нажмите Esc");
        return false;
    }
    if (len >= sizeof(_input)) {
        fail("Вопрос слишком длинный");
        return false;
    }

    memcpy(_input, text, len);
    _input[len] = '\0';
    _input_len = len;
    _redraw_input = true;

    if (!send_question()) return false;

    Serial.printf("Чат: вопрос из Serial, %u байт\n", static_cast<unsigned>(len));
    return true;
}

bool ChatScreen::send_question()
{
    if (_mode != Mode::Idle) return false;
    if (_input_len == 0) {
        notice("Введите вопрос");
        return false;
    }
    if (_wifi == nullptr || !_wifi->connected()) {
        // Радио могло быть выключено командой Serial: тогда «Ctrl+W» не
        // поможет, и человек искал бы сеть в списке, которого не будет.
        if (_wifi != nullptr && !_wifi->radio_on()) {
            fail("Wi-Fi выключен: «wifi on» по Serial или Ctrl+W");
        } else {
            fail("Нет Wi-Fi: Ctrl+W — настройка сети");
        }
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
    _agent_step = 0;
    _agent_call_index = 0;
    _agent_pending = false;
    _have_calls = false;
    _turn_ended = false;
    _confirm_index = static_cast<size_t>(-1);
    _dsml.reset();

    // Поисковое состояние сбрасываем на новом ходу: недособранный текст
    // прошлого поиска не должен попасть в результат следующего.
    _search_pending = false;
    _search_ended = false;
    _search_text_len = 0;
    _search_status[0] = '\0';
    if (_search_text != nullptr) _search_text[0] = '\0';

    // Источники — свои у каждого хода: прошлый ответ не должен подмешивать
    // свои ссылки к новому вопросу.
    if (_web_sources != nullptr) _web_sources->clear();
    _web_search_ran = false;
    _web_search_failed = false;

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
    _failed = false;
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
        g_hist[i].tool_call_id = m.tool_call_id[0] != '\0' ? m.tool_call_id : nullptr;
        g_hist[i].name = m.name[0] != '\0' ? m.name : nullptr;
        g_hist[i].tool_calls = m.tool_calls;
        // Длина вызовов нужна обрезке: они уходят в тело запроса, и без учёта
        // в бюджете длинный тред с write_doc перерастал буфер тела.
        g_hist[i].tool_calls_len = m.tool_calls_len;
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

    // Настройка «Веб-поиск» меняется с экрана Ctrl+A во время сеанса, а схемы
    // собраны в init(). Пересобираем при расхождении: иначе переключатель
    // работал бы только после перезагрузки.
    if (cfg.search_enabled != _tools_search_on && _tools_json != nullptr) {
        _tools_search_on = cfg.search_enabled;
        if (!_tools.build_tools_json(_tools_json, kToolsJsonCap, cfg.search_enabled ? nullptr : "web_search")) {
            _tools_json[0] = '\0';
        }
    }

    // tools — в КАЖДОМ запросе, включая ходы с результатами инструментов: без
    // них модель иногда печатает внутреннюю разметку в текст (Р-025).
    opt.tools_json = (_tools_json != nullptr && _tools_json[0] != '\0') ? _tools_json : nullptr;

    // Плагина `web` здесь НЕТ — ни в одном шаге (Р-040). Раньше он уходил в
    // первом запросе хода, и сервер искал по тексту последней реплики всегда,
    // независимо от того, нужен ли интернет: на «привет» он тоже искал и брал
    // за это ~2,1 ₽ (замерено, Р-038 п. 5). Теперь поиск — обычный инструмент
    // `web_search`, и плагин уходит только в поисковом подзапросе (см.
    // begin_search()). «Привет» стоит ~0,02 ₽ вместо ~2,17 ₽.
    opt.plugins_json = nullptr;

    // Собираем тело, ужимая историю, пока не влезет.
    //
    // Бюджет kHistoryBudgetBytes — это оценка по содержимому сообщений, а не
    // точный размер JSON: экранирование кавычек и переводов строки раздувает
    // тело (в тексте заметок их много), плюс системная инструкция и схемы
    // инструментов. Обычно оценка с запасом, но при неудачном сочетании
    // build_request возвращал 0, и ход падал с «История не собралась» —
    // выглядело как поломка диалога на ровном месте. Поэтому ужимаем бюджет
    // вдвое и пробуем снова: лучше ответ по свежей части разговора, чем отказ.
    size_t budget = kHistoryBudgetBytes;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const history::TrimResult trim = history::trim(g_hist, usable, budget);

        size_t n = 0;
        g_out[n].role = api::Role::System;
        g_out[n].content = kSystemPrompt;
        g_out[n].tool_call_id = nullptr;
        g_out[n].name = nullptr;
        g_out[n].tool_calls_json = nullptr;
        ++n;

        for (size_t i = trim.begin; i < trim.end; ++i) {
            g_out[n].role = g_hist[i].role;
            g_out[n].content = g_hist[i].content;
            g_out[n].tool_call_id = g_hist[i].tool_call_id;
            g_out[n].name = g_hist[i].name;
            g_out[n].tool_calls_json = g_hist[i].tool_calls;
            ++n;
        }

        const size_t len = api::build_request(g_out, n, opt, _body, kBodyCap);
        if (len > 0) {
            if (trim.dropped > 0) {
                Serial.printf("Чат: в запрос ушло %u сообщений, отброшено %u, бюджет %u КБ\n", (unsigned)trim.kept,
                    (unsigned)trim.dropped, (unsigned)(budget / 1024));
            }
            return len;
        }

        Serial.printf("Чат: тело запроса не влезло в %u КБ, ужимаю историю (бюджет %u КБ)\n",
            (unsigned)(kBodyCap / 1024), (unsigned)(budget / 1024));
        budget /= 2;
    }

    // Не влезло даже с вдвое ужатой историей трижды: причина не в объёме
    // истории (схемы инструментов, огромный системный промпт или битый текст).
    return 0;
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

    notice("Запрос отменён до первого слова, вопрос вернулся в поле ввода");
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
            notice("Запрос отменён до первого слова");
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

    // Источники дописываем в текст ответа ДО сохранения: отдельная реплика
    // system попала бы в историю запроса, и модель приняла бы список ссылок
    // за собственные слова. Так они и в ленте, и в треде, и переживают
    // перезагрузку.
    append_web_sources();

    // Итог хода в Serial: по нему проверяется приёмка, когда экран не перед
    // глазами (прогон командой `ask`).
    log_turn_result();

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
        notice("Ответ обрезан лимитом токенов модели");
    }

    enter_mode(Mode::Idle);
    _redraw_feed = true;
    _redraw_status = true;
}

void ChatScreen::append_web_sources()
{
    if (_web_sources == nullptr) return;

    // Поиск был, но ничего не нашёл: честнее сказать об этом, чем молчать —
    // иначе выглядит, будто ассистент не умеет искать. Но только если поиск
    // ДЕЙСТВИТЕЛЬНО отработал: «не смог» (сеть, отказ сервера, Esc) уже
    // показан ошибкой, и подменять её «ничего не дал» нельзя.
    if (_web_sources->count == 0) {
        if (_web_search_ran && !_web_search_failed) {
            const char* text = "\n\nПоиск в интернете не дал результатов.";
            const size_t n = strlen(text);
            if (append_answer(text, n) && _scroll == 0) _redraw_feed = true;
        }
        return;
    }

    // Сборка списка — в чистом util/web_sources: там она проверяется тестами на
    // хосте, включая обрезку. Здесь только буфер и дописывание в тред.
    //
    // Буфер статический: 2.7 КБ при 8 КБ стека задачи интерфейса — слишком
    // много для функции, которая вызывается на каждый ответ.
    static char text[web::kMaxListText];
    const size_t len = web::format_list(*_web_sources, text, sizeof(text));

    // Диагностика дублирования (Р-039): печатаем факт вызова и длину блока.
    // Если блок в ответе один, а строк две — виновата модель, а не мы.
    Serial.printf("Источники в ответ: блок %u байт, до него ответ %u байт\n", static_cast<unsigned>(len),
        static_cast<unsigned>(_store->count() > 0 ? _store->last()->content_len : 0));

    if (len > 0 && append_answer(text, len) && _scroll == 0) _redraw_feed = true;
}

// --- Агентский цикл ---------------------------------------------------------
//
// Модель вместо текста просит инструменты. Ход тогда не заканчивается: её
// сообщение с вызовами попадает в тред, каждый вызов выполняется, результат
// уходит обратно как role=tool, и отправляется следующий запрос. До
// cfg::kMaxAgentSteps шагов.
//
// Почему ОДИН вызов за кадр, а не все сразу. Инструмент работает с SD: список
// читает заголовки шестидесяти документов, поиск — их тела. Восемь вызовов
// подряд дали бы секунды, в течение которых Esc не обрабатывался бы, — а
// агент как раз тот случай, когда пользователь хочет прервать. Поэтому шаг
// разложен по tick(): вызов, кадр, следующий вызов.

void ChatScreen::handle_turn_end()
{
    _turn_ended = false;

    // Фильтр держал хвост, который мог оказаться началом маркера. Поток
    // кончился — если это не маркер, хвост надо дописать, иначе последние
    // 1–12 байт ответа пропадут.
    const size_t tail = _dsml.flush(_dsml_buf, sizeof(_dsml_buf));
    if (tail > 0 && _turn_ok) {
        append_answer(_dsml_buf, tail);
        if (_scroll == 0) _redraw_feed = true;
    }

    // Пришёл ответ с вызовами: ход продолжается, а не заканчивается.
    if (_have_calls && _turn_ok) {
        _have_calls = false;

        if (_agent_calls == nullptr) {
            finish_answer(false, "не хватило памяти на разбор вызовов инструментов");
            return;
        }

        // Лимит шагов. Дописываем причину в саму реплику: пустую реплику
        // finish_answer() выбросил бы как «модель не ответила», а пользователю
        // нужен видимый след — он ждал результата.
        if (_agent_step >= cfg::kMaxAgentSteps) {
            const char* text = "Остановился: сделано 8 шагов. Уточните вопрос.";
            append_answer(text, strlen(text));
            finish_answer(true, nullptr);
            notice("Агент остановлен: лимит 8 шагов");
            return;
        }

        ++_agent_step;

        // Сообщение модели с вызовами: текст мог быть пустым, но массив
        // вызовов обязателен. Без него сервер не свяжет результаты role=tool.
        if (_calls_json != nullptr) tools::build_calls_json(*_agent_calls, _calls_json, kCallsJsonCap);
        if (_calls_json != nullptr && _calls_json[0] != '\0') {
            _store->set_last_tool_calls(_calls_json);

            // Это сообщение только что стало «служебным ходом» (текст пуст,
            // вызовы есть) и должно исчезнуть из ленты. Кэш строк об этом не
            // узнает сам: длина ТЕКСТА не изменилась, а решение «скрыть»
            // зависит ещё и от tool_calls_len.
            remeasure_last();
            _redraw_feed = true;
        }

        char ts[32];
        now_iso(ts, sizeof(ts));
        _store->finish_last(ts[0] != '\0' ? ts : nullptr, _answer.input_tokens, _answer.output_tokens, false);

        char err[96];
        if (!_store->flush_last(err, sizeof(err))) {
            finish_answer(false, err[0] != '\0' ? err : "не удалось записать вызовы в тред");
            return;
        }

        _agent_call_index = 0;
        _agent_pending = true;
        _redraw_feed = true;

        // Режим «агент работает»: в статусе будет видно шаг, а поле ввода
        // скажет ждать. Сам вызов выполнится в следующих кадрах.
        enter_mode(Mode::Sending);
        return;
    }

    _have_calls = false;
    finish_answer(_turn_ok, _turn_reason);
}

bool ChatScreen::step_agent()
{
    if (_agent_calls == nullptr) return false;

    if (_agent_call_index >= _agent_calls->count()) {
        _agent_pending = false;
        return send_agent_turn();
    }

    const tools::Call& call = _agent_calls->at(_agent_call_index);
    execute_call(call);
    // Обработчик попросил подтверждения: удаление необратимо, решает
    // пользователь. Индекс не двигаем — вызов повторится с флагом, когда
    // человек ответит.
    //
    // _agent_pending снимаем: иначе tick() вызывал бы step_agent() каждый кадр,
    // и delete_doc переспрашивал бы существование файла по SD 60 раз в секунду.
    // Взведёт флаг обратно resolve_delete().
    if (_confirm_index != static_cast<size_t>(-1)) {
        _agent_pending = false;
        enter_mode(Mode::Confirming);
        char msg[200];
        snprintf(msg, sizeof(msg), "Агент просит удалить %s · Enter — удалить, Esc — отмена", _confirm_path);
        notice(msg);
        return false;
    }

    // Обработчик попросил поиск: подзапрос уже ушёл, ждём его. Индекс не
    // двигаем — вызов повторится с готовым результатом в finish_search().
    if (_search_pending) {
        _agent_pending = false;
        enter_mode(Mode::Searching);
        return false;
    }

    // answer_call() мог не суметь записать результат (арена, файл) и уже
    // закрыть ход. Тогда цикл останавливаем: следующий запрос ушёл бы без
    // ответа на вызов, и модель запуталась бы.
    if (_mode == Mode::Idle) {
        _agent_pending = false;
        return false;
    }

    ++_agent_call_index;
    return true;
}

void ChatScreen::execute_call(const tools::Call& call)
{
    // Буферы собираются заново на каждый вызов: обработчик не должен видеть
    // остатки предыдущего результата.
    _tool_ctx = tools::Context{};

    // Неготовый архив — nullptr, а не пустое хранилище: иначе list_docs
    // ответил бы «архив пуст», и модель сказала бы пользователю, что документов
    // нет, хотя на самом деле нет SD-карты.
    _tool_ctx.store = (_docs != nullptr && _docs->ready()) ? static_cast<docapi::Store*>(_docs) : nullptr;
    _tool_ctx.out = _tool_out;
    _tool_ctx.out_cap = tools::kMaxResultBytes + 1;
    _tool_ctx.body = _tool_body;
    _tool_ctx.body_cap = hw::DocStore::kMaxDocBytes + 1;
    _tool_ctx.entries = _doc_entries;
    _tool_ctx.entries_cap = docapi::kMaxListedDocs;
    _tool_ctx.hits = _doc_hits;
    _tool_ctx.hits_cap = 16;
    _tool_ctx.confirmed = false;
    // Источник для source_url: первый найденный в интернете за этот ход, а не
    // выдуманный моделью адрес (Р-036). Модель всё равно вправе передать свой —
    // её аргумент важнее: она видела конкретную страницу.
    _tool_ctx.web_source_url = (_web_sources != nullptr) ? web::primary_url(*_web_sources) : nullptr;

    char ts[32];
    now_iso(ts, sizeof(ts));
    _tool_ctx.now_iso = ts[0] != '\0' ? ts : nullptr;

    if (_tool_out == nullptr) {
        answer_call(call, "Ошибка: не хватило памяти под результат инструмента");
        return;
    }

    // Пустые аргументы сервер присылает как "" — инструменту нужен хотя бы
    // пустой объект, иначе валидация решит, что аргументов нет вовсе.
    const char* args = call.arguments_len > 0 ? call.arguments : "{}";

    // Имя каждого вызова — в Serial: без этого «документ записался трижды»
    // не отличить от «модель попросила трижды». При приёмке командой `ask`
    // лента не перед глазами, и это единственный способ увидеть ход агента.
    Serial.printf("Инструмент %u: %s\n", (unsigned)(_agent_step), call.name);

    _tools.invoke(call.name, args, strlen(args), _tool_ctx);

    // Обработчик попросил подтверждения — ответ пока не отправляем, его даст
    // resolve_delete().
    if (_tool_ctx.deferred && _tool_ctx.needs_confirm) {
        _confirm_index = _agent_call_index;
        snprintf(_confirm_path, sizeof(_confirm_path), "%s", _tool_ctx.confirm_path);
        return;
    }

    // Обработчик попросил поиск (Р-040) — ответ пока не отправляем: сначала
    // отдельный запрос с плагином. Индекс не двигаем: вызов повторится с
    // готовым результатом, когда поиск закончится. Так же устроено удаление.
    if (_tool_ctx.needs_search) {
        // Настройка «Веб-поиск» убирает инструмент из списка схем, но модель
        // может вызвать его по памяти из истории треда. Платить за поиск,
        // который пользователь выключил, нельзя — отвечаем отказом.
        if (!_settings->config().search_enabled) {
            _search_pending = false;
            answer_call(call,
                "Ошибка: веб-поиск выключен в настройках устройства. Отвечай по памяти и скажи, что "
                "данные не проверены.");
            return;
        }

        snprintf(_search_query, sizeof(_search_query), "%s", _tool_ctx.search_query);
        _search_max_results = _tool_ctx.search_max_results;
        _search_pending = true;

        if (!begin_search(_search_query, _search_max_results)) {
            // Начать не удалось — модели надо ответить отказом, иначе она будет
            // ждать результата, которого не будет.
            _search_pending = false;
            char msg[200];
            snprintf(msg, sizeof(msg),
                "Ошибка: поиск не удалось выполнить (%s). Отвечай по памяти и скажи, что "
                "данные не проверены.",
                _search_reason[0] != '\0' ? _search_reason : "нет связи");
            answer_call(call, msg);
        }
        return;
    }

    answer_call(call, _tool_out);
}

/// Итог хода в Serial: чем именно закончился ответ и что легло в тред.
///
/// Нужно при прогоне командой `ask`, когда лента не перед глазами: по одним
/// счётчикам источников не видно, дошло ли дело до вызова инструмента и не
/// ушёл ли ответ в повтор. Раньше здесь печаталось только «Ответ: ок» —
/// и «вызова не было» от «вызов был» этим не отличалось никак.
void ChatScreen::log_turn_result()
{
    const unsigned src_count = _web_sources != nullptr ? static_cast<unsigned>(_web_sources->count) : 0u;

    // Счётчик «поиск: был/не было» — по нему проверяется экономия (Р-040):
    // основной запрос плагина не несёт ни в одном шаге, и «привет» стоит
    // ~0,02 ₽ вместо ~2,17 ₽. «Был» здесь означает, что модель позвала
    // `web_search`, и подзапрос с плагином действительно ушёл.
    //
    // Память печатаем в этой же строке: по ней проверяется долгая сессия
    // (сценарий С6) — если PSRAM убывает от хода к ходу, где-то утечка.
    // Стоит одна пара вызовов heap_caps_get_free_size() раз на ход, а без неё
    // утечку на плате не увидеть: экран «О программе» показывает только
    // мгновенный снимок, и «было 3000, стало 2500» по нему не восстановить.
    Serial.printf("Ответ: %s, источников %u, поиск %s, шагов агента %d, PSRAM %u КБ, heap %u КБ\n",
        _turn_ok ? "ок" : "оборван", src_count,
        _web_search_failed ? "не удался" : (_web_search_ran ? "был" : "не было"), static_cast<int>(_agent_step),
        static_cast<unsigned>(ESP.getFreePsram() / 1024), static_cast<unsigned>(ESP.getFreeHeap() / 1024));

    // Хвост треда как есть: роли и начало текста. По нему видно, что модель
    // сказала после вызова инструмента, а что — до.
    const size_t total = _store->count();
    const size_t from = total > 6 ? total - 6 : 0;
    for (size_t i = from; i < total; ++i) {
        const hw::ChatMessage& m = _store->at(i);
        const char* role = m.role == api::Role::User ? "user" : m.role == api::Role::Tool ? "tool" : "assistant";
        Serial.printf(
            "  [%s%s] %.*s\n", role, m.name[0] != '\0' ? m.name : "", 70, m.content != nullptr ? m.content : "");
    }
}

/// Распечатать открытый тред в Serial: роль, имя инструмента и начало текста.
///
/// Нужно, чтобы проверить «дошли ли источники до файла» без платного запроса:
/// тред уже лежит на карте, и его достаточно прочитать.
void ChatScreen::print_thread()
{
    if (_store == nullptr) {
        Serial.println("Тред: хранилище не готово");
        return;
    }
    Serial.printf("Тред: %s, сообщений %u\n", _store->slug(), static_cast<unsigned>(_store->count()));

    const size_t total = _store->count();
    for (size_t i = 0; i < total; ++i) {
        const hw::ChatMessage& m = _store->at(i);
        const char* role = m.role == api::Role::User ? "user" : m.role == api::Role::Tool ? "tool" : "assistant";
        Serial.printf(
            "[%s%s] %.*s\n", role, m.name[0] != '\0' ? m.name : "", 1600, m.content != nullptr ? m.content : "");

        // Хвост длинного сообщения печатаем отдельно: блок «Источники:»
        // дописывается в КОНЕЦ ответа, а он бывает в несколько килобайт —
        // без хвоста «источники не дошли» не отличить от «не влезли в печать».
        //
        // Считаем вхождения маркера: их число отвечает на вопрос «дублируется
        // ли список» без догадок. Хвост мог сам содержать «Источники:», и по
        // глазам это не отличить от второго блока (так и вышло 26.11.2026).
        if (m.content != nullptr) {
            const size_t len = strlen(m.content);
            if (len > 1600) {
                const size_t tail = 400 < len ? len - 400 : 0;
                Serial.printf("  …хвост: %s\n", m.content + tail);
            }
            unsigned marks = 0;
            for (const char* p = m.content; (p = strstr(p, "Источники:")) != nullptr; ++p)
                ++marks;
            if (marks > 1) {
                Serial.printf("  ВНИМАНИЕ: блоков «Источники:» в сообщении %u\n", marks);
            }
        }
    }
}

/// Инструменты, после которых содержимое архива меняется и индекс устаревает.
bool changes_archive(const char* name)
{
    return name != nullptr && (strcmp(name, "write_doc") == 0 || strcmp(name, "delete_doc") == 0);
}

/// Что легло во фронтматтер последней записи — в Serial.
///
/// Приёмка v0.4.0 проверяет, что `source_url` заметки взят из найденного
/// источника, а не выдуман моделью. При прогоне командой `ask` экран заметок
/// не перед глазами, поэтому печатаем то, что реально записано в файл:
/// результат инструмента («Создан документ home/x.md») и поле из фронтматтера,
/// перечитанное с карты. Путь берём из результата — отдельного канала от
/// инструмента нет, и добавлять его значило бы менять контракт ради лога.
void ChatScreen::log_written_doc()
{
    constexpr const char* kMarker = "документ ";
    const char* at = strstr(_tool_out, kMarker);
    if (at == nullptr) {
        Serial.printf("Заметка: %s\n", _tool_out);
        return;
    }
    at += strlen(kMarker);

    char rel[paths::kMaxPathLen];
    size_t n = 0;
    while (at[n] != '\0' && at[n] != '\n' && n + 1 < sizeof(rel))
        ++n;
    memcpy(rel, at, n);
    rel[n] = '\0';

    static doc::Meta meta;  // 4 КБ на стеке задачи интерфейса не место
    size_t bytes = 0;
    if (_docs->read_meta(rel, meta, &bytes)) {
        Serial.printf("Заметка: %s, source_url=%s\n", rel, meta.source_url[0] != '\0' ? meta.source_url : "(пусто)");
    } else {
        Serial.printf("Заметка: %s записана, фронтматтер не перечитался\n", rel);
    }
}

void ChatScreen::answer_call(const tools::Call& call, const char* text)
{
    char id[tools::Call::kFallbackIdLen];
    tools::effective_id(call, id, sizeof(id));

    char ts[32];
    now_iso(ts, sizeof(ts));

    char err[96];
    if (!_store->append(api::Role::Tool, text != nullptr ? text : "", ts[0] != '\0' ? ts : nullptr, 0, 0, err,
            sizeof(err), id, call.name)) {
        // Не записалось — сообщать модели нечего: следующий запрос ушёл бы без
        // результата, и она в лучшем случае переспросила. Честнее сказать
        // пользователю, что тред переполнен.
        finish_answer(false, err[0] != '\0' ? err : "не удалось записать результат инструмента");
        return;
    }

    // Индекс пересобираем сразу после изменения архива, а не при выходе из
    // чата: пользователь может открыть карту на компьютере или посмотреть
    // notes/INDEX.md на самом устройстве, и устаревший список читался бы как
    // «документ не сохранился».
    if (changes_archive(call.name) && text != nullptr && strncmp(text, "Ошибка", 6) != 0 && _docs != nullptr) {
        char idx_err[96];
        if (!_docs->rebuild_index(idx_err, sizeof(idx_err))) {
            // Не ошибка диалога: документ записан, пострадал только индекс.
            Serial.printf("Архив: индекс не пересобрался — %s\n", idx_err);
        }

        // Что легло во фронтматтер — в Serial. Проверяемое при приёмке поле —
        // source_url, подставленный из первого источника поиска (Р-036), но
        // при прогоне командой `ask` экран заметок не перед глазами, и
        // заметка читается уже после прогона.
        if (strcmp(call.name, "write_doc") == 0) log_written_doc();
    }

    _redraw_feed = true;
    _redraw_title = true;
}

bool ChatScreen::send_agent_turn()
{
    _answer.input_tokens = 0;
    _answer.output_tokens = 0;
    _answer.partial = false;
    _retry_attempt = 0;
    _retry_delay_s = 0;
    _dsml.reset();

    char err[128];
    if (!_store->begin_answer(err, sizeof(err))) {
        finish_answer(false, err[0] != '\0' ? err : "тред переполнен, начните новый");
        return false;
    }

    const size_t body_len = build_body();
    if (body_len == 0) {
        _store->drop_last();
        finish_answer(false, "история не собралась в запрос");
        return false;
    }

    if (!_api->start(_settings->config().api_base_url, _settings->secrets().api_key, _body, g_sink, err, sizeof(err))) {
        _store->drop_last();
        finish_answer(false, err[0] != '\0' ? err : "не удалось отправить запрос");
        return false;
    }

    enter_mode(Mode::Sending);
    _scroll = 0;
    return true;
}

void ChatScreen::resolve_delete(bool confirmed)
{
    if (_confirm_index == static_cast<size_t>(-1) || _agent_calls == nullptr) {
        // Подтверждать нечего: скорее всего Esc уже прервал ход. Возвращаем
        // экран в покой, а не в «отправляю» — отправлять нечего.
        enter_mode(Mode::Idle);
        return;
    }

    const size_t idx = _confirm_index;
    _confirm_index = static_cast<size_t>(-1);

    const tools::Call& call = _agent_calls->at(idx);

    if (!confirmed) {
        // Отказ: модели надо ответить, что удаление отклонено. Иначе она
        // повторит вызов, и пользователя будут спрашивать снова.
        answer_call(call, "Пользователь отклонил удаление документа");
    } else {
        // Подтверждение: повторяем вызов с флагом. Решение пользователя — не
        // аргумент модели, поэтому передаём его в контексте.
        _tool_ctx = tools::Context{};
        _tool_ctx.store = (_docs != nullptr && _docs->ready()) ? static_cast<docapi::Store*>(_docs) : nullptr;
        _tool_ctx.out = _tool_out;
        _tool_ctx.out_cap = tools::kMaxResultBytes + 1;
        _tool_ctx.body = _tool_body;
        _tool_ctx.body_cap = hw::DocStore::kMaxDocBytes + 1;
        _tool_ctx.entries = _doc_entries;
        _tool_ctx.entries_cap = docapi::kMaxListedDocs;
        _tool_ctx.hits = _doc_hits;
        _tool_ctx.hits_cap = 16;
        _tool_ctx.confirmed = true;

        char ts[32];
        now_iso(ts, sizeof(ts));
        _tool_ctx.now_iso = ts[0] != '\0' ? ts : nullptr;

        const char* args = call.arguments_len > 0 ? call.arguments : "{}";
        _tools.invoke(call.name, args, strlen(args), _tool_ctx);
        answer_call(call, _tool_out);
    }

    if (_mode == Mode::Idle) {
        _agent_pending = false;
        return;
    }

    _agent_call_index = idx + 1;
    enter_mode(Mode::Sending);
    _agent_pending = true;
}

// --- Веб-поиск (Р-040) ------------------------------------------------------
//
// Поиск — инструмент, но обработчик инструмента не может ходить в сеть: он
// синхронный, а запрос занимает 20–30 с, и вызывается из tick(). Поэтому
// обработчик только складывает запрос в контекст и взводит флаг, а здесь
// делается ОТДЕЛЬНЫЙ запрос с плагином `web` — без `tools`, чтобы не было
// рекурсии. Результат возвращается модели как обычный ответ инструмента, и
// агентский цикл продолжается.
//
// Почему отдельный запрос, а не плагин в основном: плагин заставляет сервер
// искать по тексту ЛЮБОЙ реплики, включая «привет», и стоит ~2,1 ₽ за запрос.
// Модель же решает, нужен ли интернет, бесплатно — это её собственный ход.

bool ChatScreen::begin_search(const char* query, uint8_t max_results)
{
    _search_reason[0] = '\0';
    _search_ended = false;
    _search_ok = true;
    _search_text_len = 0;
    _search_input_tokens = 0;
    _search_output_tokens = 0;
    if (_search_text != nullptr) _search_text[0] = '\0';

    if (_search_body == nullptr || _search_text == nullptr) {
        snprintf(_search_reason, sizeof(_search_reason), "не хватило памяти");
        _web_search_failed = true;
        return false;
    }

    if (g_search_sink == nullptr) g_search_sink = new SearchSink(this);

    // Отметка в общем списке источников: в результат этого поиска пойдут
    // только ссылки, пришедшие после неё (см. _search_src_from).
    _search_src_from = (_web_sources != nullptr) ? _web_sources->count : 0;

    const settings::Config& cfg = _settings->config();
    // Модель число не назвала (0) — берём настройку «Источников в ответе»:
    // иначе этот пункт в Ctrl+A молча ни на что не влиял бы.
    if (max_results == 0) max_results = cfg.search_max_results;

    const size_t body_len = wsearch::build_body(query, cfg.api_model, max_results, _search_body, kSearchBodyCap);
    if (body_len == 0) {
        snprintf(_search_reason, sizeof(_search_reason), "запрос не собрался");
        _web_search_failed = true;
        return false;
    }

    // Строка статуса: видно, что именно ищется. Запрос модели не показываем
    // целиком — он бывает длинным; в статусе хватает начала. Если запрос не
    // влез, snprintf его обрежет, и обрезка может прийтись на середину
    // кириллического символа — тогда отступаем к началу символа.
    snprintf(_search_status, sizeof(_search_status), "ищу в сети: %s", query);
    {
        size_t end = strlen(_search_status);
        const size_t cap = sizeof(_search_status) - 1;
        if (end >= cap) {
            // Последний оставшийся байт — продолжение символа (10xxxxxx):
            // значит символ разрезан, отступаем к его началу.
            while (end > 0 && (static_cast<unsigned char>(_search_status[end - 1]) & 0xC0) == 0x80)
                --end;
            _search_status[end] = '\0';
        }
    }

    char err[128];
    if (!_api->start(cfg.api_base_url, _settings->secrets().api_key, _search_body, g_search_sink, err, sizeof(err))) {
        snprintf(_search_reason, sizeof(_search_reason), "%s", err[0] != '\0' ? err : "запрос не отправился");
        _web_search_failed = true;
        return false;
    }

    Serial.printf("Поиск: запрос «%.120s», источников просим %u\n", query, static_cast<unsigned>(max_results));

    // Поиск действительно пошёл. Ставим здесь, а не по приходу источников:
    // пустая выдача — тоже поиск, и в ленте должно появиться «поиск не дал
    // результатов», а не молчание. Ошибку поиска это тоже помечает, но она
    // видна отдельно — текстом в статусе ошибки и отказом модели.
    _web_search_ran = true;
    _web_search_failed = false;
    _wait_started = millis();
    _wait_sec = -1;
    _dots = -1;
    return true;
}

void ChatScreen::on_search_text(const char* text, size_t len)
{
    if (text == nullptr || len == 0 || _search_text == nullptr) return;

    const size_t room = kSearchTextCap - 1 - _search_text_len;
    const size_t take = len < room ? len : room;
    if (take > 0) {
        memcpy(_search_text + _search_text_len, text, take);
        _search_text_len += take;
        _search_text[_search_text_len] = '\0';
    }
}

void ChatScreen::on_search_usage(long input_tokens, long output_tokens)
{
    // Поисковый подзапрос тоже платный, и его токены входят в счёт за сессию.
    // Прибавляем дельту: сервер вправе прислать usage несколько раз, и
    // накопительный итог посчитался бы дважды.
    _session_input += input_tokens - _search_input_tokens;
    _session_output += output_tokens - _search_output_tokens;
    _search_input_tokens = input_tokens;
    _search_output_tokens = output_tokens;
    _redraw_status = true;
}

void ChatScreen::on_search_retry(int attempt, int delay_s)
{
    _retry_attempt = attempt;
    _retry_delay_s = delay_s;
    _retry_started_ms = millis();
    _redraw_status = true;
}

void ChatScreen::on_search_finished(const char* reason)
{
    snprintf(_search_reason, sizeof(_search_reason), "%s", reason != nullptr ? reason : "stop");
    _search_ok = true;
    _search_ended = true;
}

void ChatScreen::on_search_error(const char* message)
{
    snprintf(_search_reason, sizeof(_search_reason), "%s", message != nullptr ? message : "ошибка сети");
    _search_ok = false;
    _search_ended = true;
    _web_search_failed = true;
}

void ChatScreen::finish_search()
{
    _search_ended = false;
    _search_pending = false;

    if (_agent_calls == nullptr) {
        enter_mode(Mode::Idle);
        return;
    }

    const size_t idx = _agent_call_index;
    if (idx >= _agent_calls->count()) {
        // Вызов уже отработал, а поиск завершился позже — этого быть не должно,
        // но зависший агент хуже лишней проверки.
        enter_mode(Mode::Sending);
        _agent_pending = true;
        return;
    }

    const tools::Call& call = _agent_calls->at(idx);

    // Результат — 2 КБ. На стеке задачи интерфейса его держать нельзя: рядом
    // живут TLS и парсер, и 8 КБ стека не хватает (проверено: «Stack
    // protection fault» сразу после прихода источников). Буфер один на экран,
    // он же место, где формируется результат инструмента.
    char* result = _tool_out;

    // Текст поискового ответа — только материал результата: в ленту он не
    // попадает, там будет ответ основной модели. Источники — лишь пришедшие
    // этим поиском (list_from): список на экране один на ход, и без отсечения
    // второй поиск отчитался бы ссылками первого.
    //
    // Пустой список — не отдельный объект: web::List это ~6 КБ, и локальная
    // копия на стеке уже роняла плату. Берём указатель, а отсутствие
    // источников видно по count.
    const web::List* sources = _web_sources;

    if (!_search_ok) {
        // 400 байт на стеке — это нормально, а вот объект списка источников
        // (6 КБ) нет; поэтому причина собирается в маленький буфер.
        char msg[400];
        snprintf(msg, sizeof(msg), "Ошибка: поиск не удался (%s). Отвечай по памяти и скажи, что данные не проверены.",
            _search_reason);
        answer_call(call, msg);
    } else if (result == nullptr) {
        answer_call(call,
            "Ошибка: не хватило памяти под результат поиска. Отвечай по памяти и скажи, что данные "
            "не проверены.");
    } else {
        const size_t n = wsearch::format_result(
            _search_text, _search_text_len, sources, result, tools::kMaxResultBytes + 1, _search_src_from);

        // Размер списка мог уменьшиться только через `clear()` — тогда отметка
        // больше его, и вычитание в size_t дало бы астрономическое число в
        // логе. Считаем «новых» с зажимом, как это делает format_result.
        const size_t total = sources != nullptr ? sources->count : 0u;
        const size_t fresh = total > _search_src_from ? total - _search_src_from : 0u;
        Serial.printf("Поиск: ответ %u байт, пересказ %u байт, источников новых %u из %u\n",
            static_cast<unsigned>(_search_text_len), static_cast<unsigned>(n), static_cast<unsigned>(fresh),
            static_cast<unsigned>(total));
        answer_call(call, n > 0 ? result : "Поиск ничего не нашёл.");
    }

    if (_mode == Mode::Idle) {
        _agent_pending = false;
        return;
    }

    // Вызов отработал — переходим к следующему.
    ++_agent_call_index;
    enter_mode(Mode::Sending);
    _agent_pending = true;
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
            notice("Ответ длиннее 12 КБ — сохранён неполным");
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
        // Баннер офлайн-режима живёт в полосе сообщений, а её видимость зависит
        // от того же состояния сети. Без этой строки баннер остался бы на
        // экране после подключения до следующей ошибки или подсказки.
        _redraw_error = true;
    }

    // Ход закончился: либо закрываем ответ, либо идём на следующий шаг агента.
    // Делаем это здесь, а не в обработчике потока: start() нового запроса
    // внутри on_finished() переинициализировал бы парсер под работающим feed().
    if (_turn_ended) {
        handle_turn_end();
        if (_mode == Mode::Idle) return;
    }

    // Агент выполняет ровно один инструмент за кадр: вызовы работают с SD, и
    // все сразу заморозили бы экран на секунды. Остальные — в следующих кадрах.
    if (_agent_pending) {
        step_agent();
        if (_mode == Mode::Idle) return;
    }

    // Поиск завершился: отдаём модели результат и продолжаем цикл агента.
    // Делаем это в tick(), а не в on_search_finished(): start() нового запроса
    // внутри обработчика потока переинициализировал бы парсер под feed().
    //
    // Режим проверяем обязательно. cancel() (Esc, переполнение буфера) тоже
    // зовёт on_search_error, и тот взводит _search_ended — но ход уже брошен,
    // и finish_search() дописал бы в тред результат инструмента, на чей вызов
    // ответа в треде больше нет. Модель получила бы role=tool на вызов,
    // которого не видит.
    if (_search_ended && _mode == Mode::Searching) {
        finish_search();
        if (_mode == Mode::Idle) return;
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
    if (text == nullptr || len == 0) return;
    // Внутренняя разметка DSML: модель иногда печатает её в текст вместо
    // вызова инструмента (Р-025). Фильтр режет её вместе со всем, что после
    // маркера, и режет с учётом границы чанков.
    if (len > sizeof(_dsml_buf)) len = sizeof(_dsml_buf);
    len = _dsml.feed(text, len, _dsml_buf, sizeof(_dsml_buf));
    if (len == 0) return;

    const bool appended = append_answer(_dsml_buf, len);

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
    // Не finish_answer() сразу: здесь могут быть вызовы инструментов, и тогда
    // ход продолжается. Решение принимает tick() — start() нового запроса
    // внутри обработчика потока переинициализировал бы парсер под feed().
    snprintf(_turn_reason, sizeof(_turn_reason), "%s", reason != nullptr ? reason : "stop");
    _turn_ok = true;
    _turn_ended = true;
}

void ChatScreen::on_answer_error(const char* message)
{
    snprintf(_turn_reason, sizeof(_turn_reason), "%s", message != nullptr ? message : "ошибка");
    _turn_ok = false;
    _turn_ended = true;
}

void ChatScreen::on_tool_calls(const tools::Accumulator& calls)
{
    if (_agent_calls == nullptr) return;
    *_agent_calls = calls;
    _have_calls = _agent_calls->any();
}

void ChatScreen::on_web_sources(const web::List& sources)
{
    if (_web_sources == nullptr) return;
    web::append(*_web_sources, sources);
    _web_search_ran = true;

    // Пишем в Serial: при прогоне по команде `ask` экран может быть не на
    // виду, а «источники пришли» — главное, что нужно подтвердить. Печатаем
    // счётчики, а не сам список: он бывает на 10 КБ.
    Serial.printf("Поиск: получено %u, всего %u\n", static_cast<unsigned>(sources.count),
        static_cast<unsigned>(_web_sources->count));
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

void ChatScreen::remeasure_last()
{
    // Счётчик отодвигаем на единицу назад: measure_messages() пересчитает всё
    // начиная с этого индекса, то есть ровно последнее сообщение.
    if (_measured_count > 0) --_measured_count;
    _feed_valid = false;
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

        // Служебный ход модели (пустой текст, только вызовы инструментов) в
        // ленте не виден: 0 строк. Пишется он ради протокола — без него сервер
        // не свяжет ответы role=tool с вызовами, — но показать под ним нечего.
        // Ноль здесь, а не «пропустить при отрисовке»: измерение и отрисовка
        // должны пользоваться одним правилом, иначе окно ленты разъедется с
        // содержимым.
        if (chatview::service_turn(m.content_len, m.tool_calls_len)) {
            _line_counts[i] = 0;
            continue;
        }

        size_t lines = kLabelLines + count_lines(shown_text(m), width);
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
    if (_settings == nullptr) return;

    // Вся логика времени — в hw/clock: треды, документы и заметки должны
    // получать метки одинаково. Здесь только источник настроек.
    hw::clock_sync::now_iso(_settings->config(), out, out_len);
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

    // Состояние сети переехало в трей (правый верхний угол, как в macOS):
    // оно нужно на всех экранах, а не только в чате. Здесь остаётся заголовок
    // треда, и место под него кончается там, где начинается трей.
    //
    // Уйти в трей и оставить адрес в заголовке значило бы показывать одно и то
    // же дважды — на 1280 px это ещё и отнимало место у названия треда.
    const int title_room = ui::tray_left_edge(M5.Display.width()) - kMarginX - 16;

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
    ui::draw_tray(M5.Display.width());
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

    // Границу проверяем в теле, а не в заголовке цикла: служебные ходы модели
    // занимают 0 строк и не считаются нарисованными, а счётчик в условии после
    // такого пропуска ушёл бы в минус.
    for (size_t i = static_cast<size_t>(win.first_msg); i < count; ++i) {
        if (static_cast<size_t>(drawn) >= win.visible_count) break;

        const hw::ChatMessage& m = _store->at(i);

        // Служебный ход модели с вызовами занимает 0 строк (см. measure_messages):
        // рисовать нечего, и место следующего сообщения он не съедает.
        if (chatview::service_turn(m.content_len, m.tool_calls_len)) continue;

        const uint16_t color = role_color(m.role, m.partial);

        // У результатов инструмента в ленте только одна строка — см. shown_text().
        const char* body = shown_text(m);
        const size_t lines = split_lines(body, width, g_offsets, kMaxLinesPerMessage);

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
                const size_t end = (ln + 1 < lines) ? g_offsets[ln + 1] : strlen(body);

                size_t len = end > begin ? end - begin : 0;
                const char* p = body + begin;

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
        ++drawn;
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
        } else if (_mode == Mode::Confirming) {
            ui::text("Подтвердите удаление: Enter или Esc", text_x, text_y, kColorPartial);
        } else if (_mode == Mode::Searching) {
            ui::text("Ищу в интернете · Esc — прервать", text_x, text_y, kColorPartial);
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
    case Mode::Idle:
        // Итог поиска в статусе, пока не задан новый вопрос: список ссылок
        // стоит в конце ответа, и в длинной реплике его надо ещё найти
        // прокруткой. Строка отвечает на вопрос «искали ли вообще и сколько
        // нашли» одним взглядом.
        //
        // Порядок проверок важен: в ходе могло быть два поиска, и удачный
        // первый не должен выглядеть неудачей из-за второго. Источники есть —
        // значит поиск нашёл; «не удался» — только когда их нет вовсе.
        if (_web_sources != nullptr && _web_sources->count > 0) {
            const unsigned n = static_cast<unsigned>(_web_sources->count);
            snprintf(left, sizeof(left), "поиск: %u %s", n, n == 1 ? "источник" : (n < 5 ? "источника" : "источников"));
        } else if (_web_search_failed) {
            snprintf(left, sizeof(left), "поиск не удался");
        } else if (_web_search_ran) {
            snprintf(left, sizeof(left), "поиск без результата");
        } else {
            left[0] = '\0';
        }
        break;
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
        if (_agent_step > 0) {
            snprintf(
                left, sizeof(left), "агент · шаг %d%s  %d с", _agent_step, kDots[d], _wait_sec > 0 ? _wait_sec : 0);
        } else {
            snprintf(left, sizeof(left), "думаю%s  %d с", kDots[d], _wait_sec > 0 ? _wait_sec : 0);
        }
        break;
    }
    case Mode::Streaming: snprintf(left, sizeof(left), "печатает…  %d с", _wait_sec > 0 ? _wait_sec : 0); break;

    // Поиск идёт отдельным запросом — и это видно. Иначе ход с поиском выглядел
    // в точности как обычный ответ: владелец справедливо спрашивал, работает
    // ли поиск вообще.
    case Mode::Searching:
        if (_retry_delay_s > 0) {
            const unsigned long elapsed = (millis() - _retry_started_ms) / 1000;
            int left_s = _retry_delay_s - static_cast<int>(elapsed);
            if (left_s < 0) left_s = 0;
            snprintf(left, sizeof(left), "поиск · повтор %d через %d с", _retry_attempt, left_s);
        } else {
            snprintf(left, sizeof(left), "%.80s%s  %d с", _search_status[0] != '\0' ? _search_status : "ищу в сети",
                (millis() / 400) % 2 == 0 ? "." : "..", _wait_sec > 0 ? _wait_sec : 0);
        }
        break;

    case Mode::Confirming: snprintf(left, sizeof(left), "ждёт подтверждения"); break;
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
    // Полоса на всю ширину: у ошибки своя подложка, у подсказки — чёрный фон.
    // Раньше отказ сервера и «введите вопрос» были одной оранжевой строкой.
    M5.Display.fillRect(0, kErrorY, M5.Display.width(), 30, _failed ? kColorErrorBg : TFT_BLACK);

    // Нет ни сообщения, ни ошибки, а сети нет — в этой же полосе стоит баннер
    // офлайн-режима. Место выбрано то же намеренно: вторая строка рядом
    // спорила бы с ошибкой, а правило показа (util/offline) как раз говорит,
    // что сообщение важнее баннера.
    if (_error[0] == '\0') {
        const bool connected = _wifi != nullptr && _wifi->connected();
        const bool connecting = _wifi != nullptr && _wifi->connecting();
        if (offline::banner_visible(connected, connecting, false)) {
            char buf[192];
            const char* shown =
                ui::fit_width(offline::banner_text(), M5.Display.width() - 2 * kMarginX, buf, sizeof(buf));
            ui::text(shown, kMarginX, kErrorY, kColorDim);
        }
        return;
    }

    if (_failed) {
        // Длинный отказ («Лимит трат по ключу исчерпан: 100,22 из 100,00 руб.
        // Пополните баланс.») в 1280 px не влезает одной строкой — обрезаем
        // по ширине с «…». Начало сохраняется: там суть, что случилось.
        char buf[192];
        const char* shown = ui::fit_width(_error, M5.Display.width() - 2 * kMarginX, buf, sizeof(buf));
        // text_plain, а не text: у text() под каждым глифом заливается фон
        // (по умолчанию чёрный). На тёмно-красной полосе это давало чёрные
        // прямоугольники вокруг букв — проверено на плате счётом пикселей.
        ui::text_plain(shown, kMarginX, kErrorY, kColorError);
        return;
    }

    // Сообщение-подсказка: не ошибка, краснеть незачем.
    ui::text(_error, kMarginX, kErrorY, kColorPartial);
}

void ChatScreen::draw_hints()
{
    M5.Display.fillRect(0, ui::kHintY - 6, M5.Display.width(), 30, TFT_BLACK);

    // Одна короткая строка. Раньше здесь были две колонки — «что делает
    // экран» слева и «куда уйти» справа, — и они перестали помещаться в
    // 1280 px: правая наезжала на левую и уходила за край. Сокращать до
    // «C+O — док.» не хотелось, поэтому переходы и редкие команды переехали в
    // меню по Ctrl+M, а внизу осталось только то, что нужно каждый раз.
    //
    // «Esc — очистить ввод» сказано полностью: рядом есть «прервать», и
    // голое «Esc — очистить» путалось бы с остановкой ответа.
    if (_mode == Mode::Confirming) {
        ui::text("Удаление необратимо · Enter — удалить · Esc — отмена", kMarginX, ui::kHintY, kColorPartial);
        return;
    }
    ui::text("Enter — отправить · Esc — прервать или очистить · ↑↓ — прокрутка · Ctrl+M — меню", kMarginX, ui::kHintY,
        kColorFaint);
}

}  // namespace ui
