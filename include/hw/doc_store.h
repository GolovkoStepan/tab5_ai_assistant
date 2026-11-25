#pragma once

// Архив документов на SD: список, чтение, запись, поиск, индекс.
//
// Формат описан в docs/ARCHITECTURE.md §5.1: `<тема>/<слаг>.md` с YAML-
// фронтматтером. Валидация путей и разбор фронтматтера — в util/ (тесты на
// хосте), здесь только файловые операции.
//
// Почему атомарная запись, а не «открыть и писать». Документ пишется один раз,
// но если питание пропадёт посреди записи, на карте останется половина файла
// без фронтматтера — и он перестанет открываться как документ, хотя пользователь
// считает его сохранённым. Пишем в `<slug>.md.tmp`, потом rename: на FAT32
// rename в пределах каталога атомарен, так что на карте либо старый файл, либо
// новый, но не обрезанный.
//
// Rename поверх существующего файла на FAT32 не работает, поэтому старый
// сначала удаляется. Окно между remove и rename узкое, но оно есть; чтобы его
// пережить, содержимое уже лежит в .tmp — при сбое пользователь найдёт файл
// рядом и сможет переименовать руками, а не потеряет текст.

#include <stddef.h>
#include <stdint.h>

#include "util/doc_api.h"
#include "util/doc_path.h"
#include "util/front_matter.h"

namespace hw {

class DocStore : public docapi::Store {
public:
    /// Максимальный размер документа, который читаем целиком.
    ///
    /// 64 КБ — это ~30 тысяч русских символов. Документ-инструкция столько не
    /// занимает, а читать больше значило бы держать в PSRAM буфер, который
    /// никогда не заполнится.
    static constexpr size_t kMaxDocBytes = 64 * 1024;

    /// Сколько документов отдаём в списке за раз.
    static constexpr size_t kMaxListedDocs = 60;

    /// Открыть каталог архива, выделить буфер чтения.
    /// @return false, если SD недоступна или не хватило памяти
    bool begin();

    /// Архив готов к работе. Без этого «пустой список» не отличить от «нет
    /// карты», и модель сказала бы пользователю «документов нет» вместо
    /// «архив недоступен».
    bool ready() const { return _ready; }

    /// Собрать список документов.
    ///
    /// @param folder  подкаталог (`home`, `home/kitchen`) или пустая строка /
    ///                nullptr для всего архива
    /// @param out     массив результатов
    /// @param cap     его размер (не больше kMaxListedDocs)
    /// @return сколько заполнено; 0 — документов нет или SD недоступна
    size_t list(const char* folder, docapi::Entry* out, size_t cap) override;

    /// Прочитать документ.
    ///
    /// @param rel      путь относительно /notes
    /// @param meta     [выход] разобранный фронтматтер
    /// @param body     буфер тела (без фронтматтера)
    /// @param body_cap его размер
    /// @param body_len [выход] длина тела
    /// @return false, если файла нет или он слишком велик
    bool read(
        const char* rel, doc::Meta& meta, char* body, size_t body_cap, size_t* body_len, char* err, size_t err_len);

    /// Прочитать метаданные, не копируя тело. Нужно списку и проверке
    /// существования: читать 64 КБ ради заголовка незачем.
    bool read_meta(const char* rel, doc::Meta& meta, size_t* file_bytes) override;

    /// Записать документ.
    ///
    /// Если файл уже есть, `created` из него сохраняется, когда в новом
    /// фронтматтере дата создания не задана: иначе повторное сохранение
    /// «омолаживало» бы документ и он терял бы место в хронологии.
    ///
    /// @return false при ошибке; причина — в err
    bool write(
        const char* rel, const doc::Meta& meta, const char* body, bool* created, char* err, size_t err_len) override;

    /// Удалить документ. Пустые папки остаются: их незаметно, а обход
    /// каталога ради уборки — лишняя работа при каждом удалении.
    bool remove(const char* rel, char* err, size_t err_len) override;

    /// Найти документы по подстроке (без учёта регистра).
    ///
    /// Ищет и в заголовке, и в теле: пользователь помнит «фильтр», а слово
    /// может быть только в тексте.
    ///
    /// @param limit сколько результатов вернуть (0 — kMaxListedDocs)
    /// @return сколько заполнено
    size_t search(const char* query, size_t limit, docapi::Hit* out, size_t cap) override;

    /// Есть ли документ по такому пути.
    bool exists(const char* rel) override;

    /// Пересобрать `notes/INDEX.md`: все документы с заголовками и датами.
    ///
    /// Индекс лежит в архиве намеренно: его видно с компьютера, и он же
    /// показывает человеку, что устройство вообще сохранило.
    bool rebuild_index(char* err, size_t err_len);

    /// Сколько документов найдено последним вызовом list() — для «ещё N».
    size_t total_found() const { return _total_found; }

private:
    /// Полный путь на карте: `/notes/<rel>`.
    static void make_path(const char* rel, char* out, size_t out_len);

    /// Создать каталог и все родительские.
    ///
    /// `mkdir` в Arduino не рекурсивный: `mkdir("/notes/a/b")` при
    /// отсутствующем `/notes/a` вернёт ошибку. Поэтому идём по сегментам.
    static bool ensure_folder(const char* rel_folder);

    /// Обойти каталог и собрать документы.
    void walk(const char* rel_folder, docapi::Entry* out, size_t cap, size_t depth);

    /// Разобрать файл: метаданные и (если body не nullptr) тело.
    bool read_file(
        const char* path, doc::Meta& meta, char* body, size_t body_cap, size_t* body_len, size_t* file_bytes);

    /// Буфер файла: в него читается документ целиком.
    char* _buf{nullptr};
    /// Буфер тела: тело копируется сюда, чтобы вызывающий не держал файл.
    /// Отдельный от _buf, иначе копирование шло бы с перекрытием.
    char* _body{nullptr};
    /// Массив записей для list() и search(): 15 КБ, только в PSRAM.
    docapi::Entry* _entries{nullptr};
    /// Выделить массив записей (лениво, один раз).
    bool ensure_entries();
    bool _ready{false};
    size_t _total_found{0};
};

}  // namespace hw
