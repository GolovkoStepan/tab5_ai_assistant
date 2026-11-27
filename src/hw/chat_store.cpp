// Треды чата на SD: чтение, дозапись.
//
// Две операции записи, и обе — дозапись, а не перезапись файла:
//   * append() дописывает строку в конец — вопрос пользователя сохраняется
//     сразу, до ответа модели. При обрыве питания теряется максимум последняя
//     строка, и тред остаётся читаемым.
//   * append_to_last() копит растущий ответ в буфере _tail, не трогая файл.
//   * flush_last() дописывает готовый ответ одной строкой, когда он дописан.
//
// Перезаписи файла нет намеренно. Раньше flush_last() собирал файл заново из
// _messages, и история становилась заложником памяти: ошибка в указателях
// уезжала на карту, и треда на диске больше не было. Дозапись трогает только
// новую реплику — что уже лежит в файле, не переписывается никогда.
//
// Три буфера, и это не расточительство, а разделение по сроку жизни:
//   _file — прочитанный файл, разбирается один раз при загрузке;
//   _arena — текст всех сообщений треда, живой, пока открыт тред;
//   _tail — растущий ответ, перезаписывается на каждом событии стриминга.
//
// Важное правило про _tail: он ОДИН на все сообщения, поэтому сообщение,
// указывающее на него, обязано переселиться в арену, как только начнётся
// следующий ответ. Иначе все реплики модели начнут показывать текст последней,
// а flush_last() запишет этот повтор в файл для каждой из них.

#include "hw/chat_store.h"

#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "hw/clock.h"
#include "util/chat_list.h"

namespace hw {
namespace {

/// Выделить буфер в PSRAM, при неудаче — в обычной куче.
char* alloc_psram(size_t size)
{
    char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (p == nullptr) p = static_cast<char*>(malloc(size));
    return p;
}

}  // namespace

void ChatStore::make_path(const char* slug, char* out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s.jsonl", cfg::kChatsDir, slug);
}

bool ChatStore::valid_slug(const char* slug)
{
    if (slug == nullptr || slug[0] == '\0') return false;

    size_t len = 0;
    for (const char* p = slug; *p != '\0'; ++p) {
        const char c = *p;
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
        if (++len >= 48) return false;
    }
    return true;
}

bool ChatStore::begin()
{
    // Каталог создаём один раз: mkdir на существующем каталоге не ломается.
    if (!SD_MMC.exists(cfg::kChatsDir)) {
        if (!SD_MMC.mkdir(cfg::kChatsDir)) return false;
    }

    if (_arena == nullptr) _arena = alloc_psram(kMaxArenaBytes);
    if (_tail == nullptr) _tail = alloc_psram(kMaxContentBytes + 1);
    if (_file == nullptr) _file = alloc_psram(kMaxFileBytes + 1);
    if (_calls_scratch == nullptr) _calls_scratch = alloc_psram(kCallsScratchBytes);
    if (_line == nullptr) _line = alloc_psram(kMaxLineBytes);

    _ready = _arena != nullptr && _tail != nullptr && _file != nullptr && _calls_scratch != nullptr && _line != nullptr;
    return _ready;
}

void ChatStore::create(const char* slug)
{
    _count = 0;
    _arena_used = 0;
    _unsaved_tail = false;
    _tail_used = 0;
    _last_append_bytes = 0;
    _last_append_arena = 0;
    _last_append_msg_len = 0;
    if (_tail != nullptr) _tail[0] = '\0';

    if (slug != nullptr && valid_slug(slug)) {
        snprintf(_slug, sizeof(_slug), "%s", slug);
    } else {
        _slug[0] = '\0';
    }
}

bool ChatStore::create_new(char* out_slug, size_t out_len)
{
    if (out_slug == nullptr || out_len == 0) return false;

    // Предыдущий тред ни разу не использовался (Ctrl+N на пустом экране):
    // удаляем его файл, иначе каждое нажатие оставляло бы на карте пустышку,
    // и они копились бы десятками.
    if (_slug[0] != '\0' && _count == 0) {
        char old_path[96];
        make_path(_slug, old_path, sizeof(old_path));
        File old = SD_MMC.open(old_path, FILE_READ);
        if (old) {
            const bool empty = old.size() == 0;
            old.close();
            if (empty) SD_MMC.remove(old_path);
        }
    }

    const time_t now = time(nullptr);
    if (now < clock_sync::kMinValidEpoch) {
        // Часы не выставлены (незаряженный RTC, ещё нет NTP): берём uptime —
        // треды не должны перестать создаваться из-за неверного времени.
        // Сравнение именно с 2020 годом: time() до синхронизации возвращает
        // секунды с загрузки, то есть формально «> 0», и проверка на ноль
        // пропускала бы их — отсюда имена `chat-19700101-005946`.
        //
        // Формат намеренно такой же по длине, как у настоящего имени
        // (`chat-20261124-120000`): load_latest выбирает тред сравнением
        // строк, и «chat-12345» оказался бы «новее» любого нормального имени.
        // Нули в дате гарантируют, что тред, созданный с настоящими часами,
        // всегда окажется позже.
        char stamp[16];
        snprintf(stamp, sizeof(stamp), "%06lu", static_cast<unsigned long>(millis() / 1000) % 1000000UL);
        snprintf(out_slug, out_len, "chat-00000000-%.6s", stamp);
    } else {
        struct tm t{};
        localtime_r(&now, &t);
        snprintf(out_slug, out_len, "chat-%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
            t.tm_hour, t.tm_min, t.tm_sec);
    }

    // Совпадение возможно, если два треда созданы в одну секунду.
    char path[96];
    char base[48];
    snprintf(base, sizeof(base), "%s", out_slug);
    make_path(out_slug, path, sizeof(path));
    int suffix = 1;
    while (SD_MMC.exists(path) && suffix < 100) {
        snprintf(out_slug, out_len, "%s-%d", base, suffix);
        make_path(out_slug, path, sizeof(path));
        ++suffix;
    }

    create(out_slug);

    // Пустой файл создаём сразу. Иначе новый тред живёт только в памяти, и
    // после перезагрузки его нет: load_latest ищет файлы, а файла не было.
    // Пользователь нажимал Ctrl+N, видел «новый тред», перезагружал плату —
    // и тред исчезал.
    make_path(out_slug, path, sizeof(path));
    File f = SD_MMC.open(path, FILE_WRITE);
    if (f) f.close();
    return true;
}

bool ChatStore::read_file(const char* slug)
{
    char path[96];
    make_path(slug, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    const size_t size = f.size();
    if (size == 0) {
        f.close();
        // Пустой файл — не ошибка: тред создан, но сообщений ещё нет.
        if (_file != nullptr) _file[0] = '\0';
        return true;
    }

    // Файл больше буфера: читаем столько, сколько помещается. Обрезка истории
    // при отправке всё равно отбросила бы старое.
    const size_t want = (size < kMaxFileBytes) ? size : kMaxFileBytes;
    const size_t n = f.read(reinterpret_cast<uint8_t*>(_file), want);
    f.close();
    if (n == 0) return false;

    _file[n] = '\0';
    return true;
}

bool ChatStore::store_message(const api::Role& role, const char* content, const char* ts, long input_tokens,
    long output_tokens, bool partial, ChatMessage& out, const char* tool_call_id, const char* name,
    const char* tool_calls)
{
    if (content == nullptr) content = "";
    const size_t n = strlen(content);
    if (n > kMaxContentBytes) return false;

    const size_t calls_len = tool_calls != nullptr ? strlen(tool_calls) : 0;

    if (_arena == nullptr || _arena_used + n + 1 + calls_len + 1 > kMaxArenaBytes) {
        // Арена кончилась. Это следствие длинного треда: вызывающий увидит
        // false и сможет начать новый.
        return false;
    }

    memcpy(_arena + _arena_used, content, n + 1);
    out.content = _arena + _arena_used;
    out.content_len = n;
    _arena_used += n + 1;

    // Вызовы инструмента тоже в арене: они переживают ответ и уходят в
    // следующие запросы, значит должны жить, пока открыт тред.
    out.tool_calls = nullptr;
    out.tool_calls_len = 0;
    if (calls_len > 0) {
        memcpy(_arena + _arena_used, tool_calls, calls_len + 1);
        out.tool_calls = _arena + _arena_used;
        out.tool_calls_len = calls_len;
        _arena_used += calls_len + 1;
    }

    out.role = role;
    snprintf(out.ts, sizeof(out.ts), "%s", ts != nullptr ? ts : "");
    out.input_tokens = input_tokens;
    out.output_tokens = output_tokens;
    out.partial = partial;
    snprintf(out.tool_call_id, sizeof(out.tool_call_id), "%s", tool_call_id != nullptr ? tool_call_id : "");
    snprintf(out.name, sizeof(out.name), "%s", name != nullptr ? name : "");
    return true;
}

void ChatStore::parse_buffer()
{
    _count = 0;
    _arena_used = 0;
    if (_file == nullptr || _arena == nullptr) return;

    const size_t total = strlen(_file);
    char* line = _file;

    for (size_t i = 0; i <= total; ++i) {
        const bool at_end = (i == total);
        if (!at_end && _file[i] != '\n') continue;

        // Последняя строка без перевода строки в конце — не пустая строка.
        if (at_end && line == _file + total) break;

        _file[i] = '\0';
        size_t len = static_cast<size_t>(_file + i - line);
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        if (len > 0 && _count < kMaxMessages) {
            // Текст сообщения копируется в арену: строки файла перезаписываются
            // по мере разбора, и указатель внутрь строки стал бы висячим.
            ChatMessage& msg = _messages[_count];
            const size_t room = kMaxArenaBytes - _arena_used;

            chatline::Entry entry;
            if (chatline::parse(line, len, entry, _arena + _arena_used, room, _calls_scratch, kCallsScratchBytes)) {
                // Текст вызовов тоже копируется в арену: _calls_scratch один на
                // весь разбор и на следующей итерации будет перезаписан.
                const size_t calls_len = entry.tool_calls_len;
                const size_t need = entry.content_len + 1 + (calls_len > 0 ? calls_len + 1 : 0);
                if (need > room) {
                    line = _file + i + 1;
                    continue;
                }

                msg.role = entry.role;
                msg.content = _arena + _arena_used;
                msg.content_len = entry.content_len;
                snprintf(msg.ts, sizeof(msg.ts), "%s", entry.ts);
                msg.input_tokens = entry.input_tokens;
                msg.output_tokens = entry.output_tokens;
                msg.partial = entry.partial;
                snprintf(msg.tool_call_id, sizeof(msg.tool_call_id), "%s", entry.tool_call_id);
                snprintf(msg.name, sizeof(msg.name), "%s", entry.name);

                msg.tool_calls = nullptr;
                msg.tool_calls_len = 0;
                if (calls_len > 0) {
                    char* dst = _arena + _arena_used + entry.content_len + 1;
                    memcpy(dst, _calls_scratch, calls_len + 1);
                    msg.tool_calls = dst;
                    msg.tool_calls_len = calls_len;
                }

                _arena_used += need;
                ++_count;
            }
            // Битая строка не повод выбрасывать тред: пропускаем её. Обрыв
            // питания посреди записи — самый частый случай, и теряться должна
            // одна строка, а не весь разговор.
        }

        line = _file + i + 1;
    }
}

bool ChatStore::load(const char* slug)
{
    if (!_ready || !valid_slug(slug)) return false;

    create(slug);
    if (!read_file(slug)) return false;

    parse_buffer();
    return true;
}

bool ChatStore::load_latest()
{
    if (!_ready) return false;

    File dir = SD_MMC.open(cfg::kChatsDir);
    if (!dir || !dir.isDirectory()) return false;

    // Ищем лексикографически наибольшее имя: треды называются по времени
    // (`chat-20261124-120000`), поэтому это и есть самый свежий. Сравнение
    // строк дешевле разбора имён и не требует времени с RTC.
    char best[48] = {};
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        // Имя копируем ДО close(): File::close() отпускает реализацию, и name()
        // после него возвращает nullptr. С этим «последний тред» не находился
        // никогда, и после перезагрузки чат открывался пустым, хотя треды на
        // карте были.
        char name[64];
        snprintf(name, sizeof(name), "%s", entry.name() != nullptr ? entry.name() : "");
        entry.close();
        if (name[0] == '\0') continue;

        // name() может вернуть полный путь, а может — только имя файла.
        const char* slash = strrchr(name, '/');
        const char* base = slash != nullptr ? slash + 1 : name;

        const size_t len = strlen(base);
        if (len < 7 || strcmp(base + len - 6, ".jsonl") != 0) continue;

        char slug[48];
        const size_t slug_len = len - 6;
        if (slug_len == 0 || slug_len >= sizeof(slug)) continue;
        memcpy(slug, base, slug_len);
        slug[slug_len] = '\0';

        if (!valid_slug(slug)) continue;
        if (best[0] == '\0' || strcmp(slug, best) > 0) snprintf(best, sizeof(best), "%s", slug);
    }
    dir.close();

    if (best[0] == '\0') return false;
    return load(best);
}

size_t ChatStore::list_threads(ThreadInfo* out, size_t cap)
{
    if (!_ready || out == nullptr || cap == 0) return 0;
    if (cap > kMaxListedThreads) cap = kMaxListedThreads;

    File dir = SD_MMC.open(cfg::kChatsDir);
    if (!dir || !dir.isDirectory()) return 0;

    // Первый проход — имена и размеры, без чтения файлов. Имена тредов
    // начинаются с даты, поэтому лексикографически наибольшее имя — самое
    // свежее. Заполняем out[] сразу: каталог отдаёт файлы в произвольном
    // порядке, и останавливаться на первых cap нельзя — в список могли бы не
    // попасть последние разговоры. Поэтому массив всегда отсортирован по
    // убыванию, а всё, что старше последнего элемента, отбрасывается.
    size_t found = 0;

    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        // Имя копируем ДО close(): File::close() отпускает реализацию, и name()
        // после него возвращает nullptr (на этом уже обжигались в load_latest).
        char name[64];
        snprintf(name, sizeof(name), "%s", entry.name() != nullptr ? entry.name() : "");
        const size_t size = entry.size();
        entry.close();
        if (name[0] == '\0') continue;

        // name() может вернуть полный путь, а может — только имя файла.
        const char* slash = strrchr(name, '/');
        const char* base = slash != nullptr ? slash + 1 : name;

        const size_t len = strlen(base);
        if (len < 7 || strcmp(base + len - 6, ".jsonl") != 0) continue;

        char slug[48];
        const size_t slug_len = len - 6;
        if (slug_len == 0 || slug_len >= sizeof(slug)) continue;
        memcpy(slug, base, slug_len);
        slug[slug_len] = '\0';

        if (!valid_slug(slug)) continue;

        // Место в отсортированном массиве. Дёшево: записей не больше сорока, и
        // происходит это только при входе на экран.
        size_t pos = 0;
        while (pos < found && strcmp(out[pos].slug, slug) > 0)
            ++pos;
        if (pos >= cap) continue;  // старше всех, что помещаются

        const size_t last = found < cap ? found : cap - 1;
        for (size_t i = last; i > pos; --i)
            out[i] = out[i - 1];
        out[pos] = ThreadInfo{};
        snprintf(out[pos].slug, sizeof(out[pos].slug), "%s", slug);
        out[pos].bytes = size;
        if (found < cap) ++found;
    }
    dir.close();

    if (found == 0) return 0;

    // Второй проход — даты и заголовки. Буфер строки один на весь проход:
    // 16 КБ не помещаются на стеке задачи цикла, а выделять его на каждый тред
    // незачем.
    char* line = static_cast<char*>(alloc_psram(kMaxLineBytes));
    if (line == nullptr) return 0;

    for (size_t i = 0; i < found; ++i) {
        ThreadInfo& info = out[i];

        chatlist::Stamp stamp;
        if (chatlist::parse_slug(info.slug, stamp)) {
            info.year = stamp.year;
            info.month = stamp.month;
            info.day = stamp.day;
            info.hour = stamp.hour;
            info.minute = stamp.minute;
            info.time_known = stamp.real;
        }

        // У пустого треда заголовка нет по определению.
        if (info.bytes == 0) continue;

        char path[96];
        make_path(info.slug, path, sizeof(path));
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) continue;

        // Читаем построчно, но не больше нескольких строк: первая строка может
        // оказаться ответом модели или мусором, а вопрос пользователя —
        // следующей.
        for (int attempt = 0; attempt < 4; ++attempt) {
            int n = f.readBytesUntil('\n', line, kMaxLineBytes - 1);
            if (n <= 0) break;
            line[n] = '\0';
            // Хвостовой '\r' от CRLF-файла.
            while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n'))
                line[--n] = '\0';
            if (n == 0) continue;
            if (chatlist::title_from_line(line, static_cast<size_t>(n), info.title, sizeof(info.title))) break;
        }
        f.close();
    }

    free(line);
    return found;
}

bool ChatStore::build_line(const ChatMessage& msg, char* out, size_t out_len)
{
    chatline::Usage usage;
    usage.input_tokens = msg.input_tokens;
    usage.output_tokens = msg.output_tokens;
    usage.partial = msg.partial;

    return chatline::build(msg.role, msg.content, msg.ts[0] != '\0' ? msg.ts : nullptr, usage, out, out_len,
               msg.tool_call_id[0] != '\0' ? msg.tool_call_id : nullptr, msg.name[0] != '\0' ? msg.name : nullptr,
               msg.tool_calls) > 0;
}

bool ChatStore::append(const api::Role& role, const char* content, const char* ts, long input_tokens,
    long output_tokens, char* err, size_t err_len, const char* tool_call_id, const char* name, const char* tool_calls)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!_ready || _slug[0] == '\0') {
        if (err && err_len > 0) snprintf(err, err_len, "тред не открыт");
        return false;
    }
    if (_count >= kMaxMessages) {
        // «В треде слишком много сообщений» — правда, но с ней непонятно, что
        // делать: человек продолжает писать, а вопрос не уходит. Подсказка
        // ведёт туда, где выход: Ctrl+N создаёт новый тред (старый остаётся
        // на карте и открывается из списка Ctrl+T).
        if (err && err_len > 0) snprintf(err, err_len, "Тред заполнен (200) — Ctrl+N начнёт новый");
        return false;
    }

    // Сообщение собираем во временную запись, чтобы не портить арену, если
    // текст не поместится.
    ChatMessage msg;
    const size_t arena_before = _arena_used;
    if (!store_message(role, content, ts, input_tokens, output_tokens, false, msg, tool_call_id, name, tool_calls)) {
        if (err && err_len > 0) snprintf(err, err_len, "тред переполнен, начните новый");
        return false;
    }

    if (!build_line(msg, _line, kMaxLineBytes)) {
        _arena_used = arena_before;
        if (err && err_len > 0) snprintf(err, err_len, "сообщение слишком длинное");
        return false;
    }

    char path[96];
    make_path(_slug, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) {
        _arena_used = arena_before;
        if (err && err_len > 0) snprintf(err, err_len, "не удалось открыть тред на запись");
        return false;
    }

    const size_t n = strlen(_line);
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(_line), n);
    const size_t newline = f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    f.close();

    if (written != n || newline != 1) {
        // Текст уже скопирован в арену, а сообщения в треде не будет: без
        // отката арена теряла бы место на каждую неудачную запись, и длинный
        // тред «переполнялся» бы на ровном месте.
        _arena_used = arena_before;
        if (err && err_len > 0) snprintf(err, err_len, "запись в тред прервалась");
        return false;
    }

    _messages[_count] = msg;
    ++_count;
    _unsaved_tail = false;

    // Запоминаем, сколько байт заняла эта запись: по этой величине
    // undo_last_user() откатит файл, если вопрос останется без ответа.
    _last_append_bytes = n + 1;
    _last_append_arena = arena_before;
    _last_append_msg_len = msg.content_len;
    return true;
}

bool ChatStore::undo_last_user(char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (_count == 0) return false;

    // Откатываем только вопрос пользователя: ответ модели откатывать нельзя,
    // он уже в файле и в ленте, и потерять его — хуже, чем оставить.
    const ChatMessage& last = _messages[_count - 1];
    if (last.role != api::Role::User) return false;

    // Файл: обрезаем ровно на длину последней дописанной строки. Раньше
    // вопрос оставался в треде без ответа, и следующий ответ модели висел на
    // нём — в ленте получалось «Назови цвет» → «Пять шагов», то есть ответ не
    // на тот вопрос.
    if (_last_append_bytes > 0) {
        char path[96];
        make_path(_slug, path, sizeof(path));

        // truncate()/ftruncate() требуют POSIX-путь: SD_MMC.open() работает от
        // точки монтирования (/chats/...), а open() — от корня (/sdcard/chats/...).
        // Без префикса open() возвращает -1, и откат молча не срабатывает.
        char fs_path[128];
        snprintf(fs_path, sizeof(fs_path), "%s%s", cfg::kMountPoint, path);

        File f = SD_MMC.open(path, FILE_READ);
        const size_t size = f ? f.size() : 0;
        if (f) f.close();

        if (size >= _last_append_bytes) {
            const int fd = open(fs_path, O_RDWR);
            if (fd < 0) {
                if (err && err_len > 0) snprintf(err, err_len, "не удалось откатить вопрос в треде");
                return false;
            }
            const int rc = ftruncate(fd, static_cast<off_t>(size - _last_append_bytes));
            close(fd);
            if (rc != 0) {
                if (err && err_len > 0) snprintf(err, err_len, "не удалось откатить вопрос в треде");
                return false;
            }
        }
    }

    --_count;

    // Арену отматываем только если после вопроса в неё ничего не дописали.
    // Так почти всегда и бывает, но не всегда: begin_answer() при начале
    // ответа переносит ТУДА ЖЕ текст предыдущего ответа (он жил в _tail) —
    // значит выше вопроса в арене уже лежит живой текст, на который ссылается
    // другое сообщение. Отмотка в этом случае оставила бы в ленте мусор вместо
    // прошлого ответа: ровно это и было видно на плате, когда отменили ответ
    // до первого слова на втором вопросе.
    //
    // Если отмотать нельзя — просто оставляем байты в арене. Это несколько
    // килобайт на отмену, а не потеря текста.
    if (_arena_used == _last_append_arena + _last_append_msg_len + 1) {
        _arena_used = _last_append_arena;
    }

    _last_append_bytes = 0;
    _last_append_arena = 0;
    _last_append_msg_len = 0;
    _unsaved_tail = false;
    if (_tail != nullptr) _tail[0] = '\0';
    _tail_used = 0;
    return true;
}

bool ChatStore::begin_answer(char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!_ready || _slug[0] == '\0' || _tail == nullptr) {
        if (err && err_len > 0) snprintf(err, err_len, "тред не открыт");
        return false;
    }
    if (_count >= kMaxMessages) {
        // «В треде слишком много сообщений» — правда, но с ней непонятно, что
        // делать: человек продолжает писать, а вопрос не уходит. Подсказка
        // ведёт туда, где выход: Ctrl+N создаёт новый тред (старый остаётся
        // на карте и открывается из списка Ctrl+T).
        if (err && err_len > 0) snprintf(err, err_len, "Тред заполнен (200) — Ctrl+N начнёт новый");
        return false;
    }

    // Предыдущий ответ дописан и уже в файле, но его текст лежит в _tail:
    // арена — для того, что больше не меняется, а _tail — буфер растущего
    // ответа, который очищается парой строк ниже. Скопировать обязательно:
    // иначе старый ответ продолжит указывать на _tail и покажет текст нового,
    // то есть все реплики модели в ленте станут одинаковыми, а при
    // flush_last() тот же повтор уедет в файл и затрёт историю.
    //
    // Ищем не только последнее сообщение: между двумя ответами в тред попадает
    // вопрос пользователя, поэтому _messages[_count - 1] — это он, а не прошлый
    // ответ, и проверка «последнее ли это сообщение» не срабатывала никогда.
    // Указатель на _tail уникален, так что цикл найдёт не больше одного.
    for (size_t i = _count; i > 0; --i) {
        ChatMessage& prev = _messages[i - 1];
        if (prev.content != _tail) continue;

        // Пустая реплика (модель попросила инструменты, текста нет) тоже
        // указывает на _tail. Переселять нечего, но и оставлять указатель
        // нельзя: _tail будет очищен, и лента показала бы под этой репликой
        // текст следующего ответа.
        if (prev.content_len == 0) {
            prev.content = "";
            continue;
        }

        const size_t n = prev.content_len;
        if (_arena == nullptr || _arena_used + n + 1 > kMaxArenaBytes) {
            if (err && err_len > 0) snprintf(err, err_len, "тред переполнен, начните новый");
            return false;
        }
        memcpy(_arena + _arena_used, _tail, n + 1);
        prev.content = _arena + _arena_used;
        _arena_used += n + 1;
        // Не break: если из-за прежней ошибки на _tail указывало несколько
        // сообщений, чиним все. После переноса указатель меняется, так что
        // цикл завершится сам.
    }

    // В файл ответ попадёт только при flush_last(): пустая реплика модели на
    // диске после обрыва питания выглядела бы как «модель промолчала».
    _tail[0] = '\0';
    _tail_used = 0;

    ChatMessage& msg = _messages[_count];
    msg = ChatMessage{};
    msg.role = api::Role::Assistant;
    msg.content = _tail;
    msg.content_len = 0;
    ++_count;
    _unsaved_tail = true;
    return true;
}

bool ChatStore::append_to_last(const char* text, size_t len)
{
    if (_count == 0 || text == nullptr || _tail == nullptr) return false;

    ChatMessage& msg = _messages[_count - 1];
    if (msg.role != api::Role::Assistant) return false;

    // Ответ растёт в отдельном фиксированном буфере, а не в арене: арена нужна
    // всем сообщениям треда, и копить в ней промежуточные версии ответа значило
    // бы исчерпать её на одном длинном ответе.
    if (_tail_used + len + 1 > kMaxContentBytes + 1) {
        // Переполнение — не ошибка ввода, а слишком длинный ответ. Помечаем
        // неполным: молча укороченный ответ выглядит как законченный.
        msg.partial = true;
        _unsaved_tail = true;
        return false;
    }

    memcpy(_tail + _tail_used, text, len);
    _tail_used += len;
    _tail[_tail_used] = '\0';

    msg.content = _tail;
    msg.content_len = _tail_used;
    _unsaved_tail = true;
    return true;
}

bool ChatStore::set_last_tool_calls(const char* json)
{
    if (_count == 0 || json == nullptr || json[0] == '\0') return false;

    ChatMessage& msg = _messages[_count - 1];
    if (msg.role != api::Role::Assistant) return false;

    const size_t n = strlen(json);
    if (_arena == nullptr || _arena_used + n + 1 > kMaxArenaBytes) return false;

    memcpy(_arena + _arena_used, json, n + 1);
    msg.tool_calls = _arena + _arena_used;
    msg.tool_calls_len = n;
    _arena_used += n + 1;
    _unsaved_tail = true;
    return true;
}

bool ChatStore::finish_last(const char* ts, long input_tokens, long output_tokens, bool partial)
{
    if (_count == 0) return false;

    ChatMessage& msg = _messages[_count - 1];
    if (ts != nullptr) snprintf(msg.ts, sizeof(msg.ts), "%s", ts);
    msg.input_tokens = input_tokens;
    msg.output_tokens = output_tokens;
    if (partial) msg.partial = true;
    _unsaved_tail = true;
    return true;
}

void ChatStore::drop_last()
{
    if (_count == 0) return;

    // Вызовы инструментов лежат в арене последними. Снимаем указатель до
    // отката: иначе на него ссылалось бы сообщение, которого уже нет.
    _messages[_count - 1].tool_calls = nullptr;
    _messages[_count - 1].tool_calls_len = 0;
    --_count;
    _tail_used = 0;
    if (_tail != nullptr) _tail[0] = '\0';
    _unsaved_tail = false;
}

bool ChatStore::flush_last(char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!_unsaved_tail) return true;
    if (_count == 0) return true;

    // Дописываем ОДНУ строку, а не перезаписываем файл целиком.
    //
    // Раньше здесь был rewrite_file(): файл собирался заново из _messages.
    // Это делало историю заложником памяти — любое повреждение указателей в
    // _messages (как было со ссылкой всех ответов на _tail) уезжало на карту,
    // и треда на диске больше не существовало. Дозапись трогает только новую
    // реплику: что уже лежит в файле, не переписывается никогда.
    //
    // Плюс к тому дозапись дешевле: rewrite_file писал весь тред (до 256 КБ)
    // после каждого ответа.
    const ChatMessage& last = _messages[_count - 1];
    if (!build_line(last, _line, kMaxLineBytes)) {
        if (err && err_len > 0) snprintf(err, err_len, "ответ не помещается в строку треда");
        return false;
    }

    char path[96];
    make_path(_slug, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось открыть тред на запись");
        return false;
    }

    const size_t n = strlen(_line);
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(_line), n);
    const size_t newline = f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    f.close();

    if (written != n || newline != 1) {
        // Флаг не снимаем: при следующей попытке ответ запишется ещё раз.
        // Дубль строки безобиден (при разборе выиграет последняя), а потеря
        // ответа — нет.
        if (err && err_len > 0) snprintf(err, err_len, "запись ответа в тред прервалась");
        return false;
    }

    _unsaved_tail = false;
    return true;
}

bool ChatStore::clear(char* err, size_t err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!_ready || _slug[0] == '\0') {
        if (err && err_len > 0) snprintf(err, err_len, "тред не открыт");
        return false;
    }

    char path[96];
    make_path(_slug, path, sizeof(path));

    // FILE_WRITE на существующем файле — усечение до нуля. Память трогаем
    // только после успеха: иначе экран показал бы пустой тред, а на карте
    // остался бы старый, и после перезагрузки разговор «вернулся».
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        if (err && err_len > 0) snprintf(err, err_len, "не удалось очистить тред на SD");
        return false;
    }
    f.close();

    // create() принимает const char* и пишет в тот же _slug, поэтому передать
    // _slug напрямую нельзя: snprintf с совпадающими буферами — неопределённое
    // поведение. Сбрасываем состояние через копию имени.
    char slug[48];
    snprintf(slug, sizeof(slug), "%s", _slug);
    create(slug);  // имя сохраняется: сброс — это очистка треда, а не новый
    return true;
}

bool ChatStore::remove(const char* slug)
{
    if (!valid_slug(slug)) return false;

    char path[96];
    make_path(slug, path, sizeof(path));
    if (!SD_MMC.remove(path)) return false;

    if (strcmp(slug, _slug) == 0) create("");
    return true;
}

bool ChatStore::title(char* out, size_t out_len) const
{
    if (out == nullptr || out_len == 0) return false;
    out[0] = '\0';

    for (size_t i = 0; i < _count; ++i) {
        if (_messages[i].role != api::Role::User) continue;
        if (_messages[i].content_len == 0) continue;

        // Заголовок — одна строка без переводов: в списке их всё равно не видно.
        size_t used = 0;
        for (size_t j = 0; j < _messages[i].content_len && used + 1 < out_len && used < 80; ++j) {
            const char c = _messages[i].content[j];
            if (c == '\n' || c == '\r') break;
            out[used++] = c;
        }
        out[used] = '\0';
        return used > 0;
    }
    return false;
}

size_t ChatStore::total_bytes() const
{
    size_t sum = 0;
    for (size_t i = 0; i < _count; ++i)
        sum += _messages[i].content_len;
    return sum;
}

}  // namespace hw
