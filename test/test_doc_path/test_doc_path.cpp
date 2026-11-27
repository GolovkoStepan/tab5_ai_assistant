/*
 * Тесты проверки путей документов.
 *
 * Путь приходит от модели, то есть из текста, который мы не контролируем.
 * Ошибка здесь означает запись за пределы архива — порчу настроек или чатов.
 * На плате такой дефект ищут по испорченным файлам, поэтому проверяем на хосте.
 */

#include <string.h>
#include <unity.h>

#include "util/doc_path.h"

void setUp(void) {}
void tearDown(void) {}

using paths::Result;

// --- Нормальные пути --------------------------------------------------------

void test_accepts_simple_path()
{
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("home/air-purifier-filter.md"));
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("readme.md"));
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("a.md"));
}

void test_accepts_deep_but_allowed()
{
    // Три уровня папок — предел.
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("home/kitchen/notes/x.md"));
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("a/b/c/d.md"));
}

void test_rejects_too_deep()
{
    TEST_ASSERT_EQUAL(Result::TooDeep, paths::check("a/b/c/d/e.md"));
}

void test_accepts_underscore_and_digit()
{
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("home/dishwasher_error_2.md"));
}

void test_accepts_dots_inside_name()
{
    // «a..b.md» — законное имя, хотя содержит две точки подряд. Отказывать
    // надо по сегменту «..», а не по подстроке, иначе страдают обычные имена.
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("home/a..b.md"));
}

// --- Выход за пределы ------------------------------------------------------

void test_rejects_absolute()
{
    TEST_ASSERT_EQUAL(Result::Absolute, paths::check("/etc/passwd.md"));
    TEST_ASSERT_EQUAL(Result::Absolute, paths::check("/sdcard/chats/x.md"));
}

void test_rejects_traversal()
{
    // Главные случаи: попытка выйти из архива.
    TEST_ASSERT_EQUAL(Result::Traversal, paths::check("../config.md"));
    TEST_ASSERT_EQUAL(Result::Traversal, paths::check("home/../../config.md"));
    TEST_ASSERT_EQUAL(Result::Traversal, paths::check("home/../chats/x.md"));
}

void test_rejects_dot_segment()
{
    TEST_ASSERT_EQUAL(Result::DotSegment, paths::check("./home/x.md"));
    TEST_ASSERT_EQUAL(Result::DotSegment, paths::check("home/./x.md"));
}

void test_rejects_empty_segment()
{
    TEST_ASSERT_EQUAL(Result::EmptySegment, paths::check("home//x.md"));
    TEST_ASSERT_EQUAL(Result::EmptySegment, paths::check("home/x.md/"));
}

// --- Символы ----------------------------------------------------------------

void test_rejects_cyrillic()
{
    // Кириллица в именах на FAT32 — источник проблем со сравнением имён.
    // Модель иногда её предлагает (проверено живым прогоном, Р-025), поэтому
    // отказ должен быть с подсказкой, а не молчаливый.
    TEST_ASSERT_EQUAL(Result::BadChar, paths::check("Архив/заметка.md"));
}

void test_rejects_spaces()
{
    TEST_ASSERT_EQUAL(Result::BadChar, paths::check("home/air purifier.md"));
}

void test_rejects_other_bad_chars()
{
    TEST_ASSERT_EQUAL(Result::BadChar, paths::check("home/x%.md"));
    TEST_ASSERT_EQUAL(Result::BadChar, paths::check("home/x?.md"));
}

void test_rejects_bad_extension()
{
    TEST_ASSERT_EQUAL(Result::BadExtension, paths::check("home/x.txt"));
    TEST_ASSERT_EQUAL(Result::BadExtension, paths::check("home/x"));
    TEST_ASSERT_EQUAL(Result::BadExtension, paths::check("home/x.markdown"));
    // «home/.md» — файл без имени: это отдельная, более понятная ошибка.
    TEST_ASSERT_EQUAL(Result::EmptySegment, paths::check("home/.md"));
}

void test_accepts_uppercase_extension()
{
    TEST_ASSERT_EQUAL(Result::Ok, paths::check("home/x.MD"));
}

// --- Длина ------------------------------------------------------------------

void test_rejects_too_long_segment()
{
    char path[128];
    memset(path, 'a', 60);
    memcpy(path + 60, ".md", 4);
    TEST_ASSERT_EQUAL(Result::SegmentTooLong, paths::check(path));
}

void test_rejects_too_long_path()
{
    // kMaxPathLen = 96, поэтому путь из 95 символов проходит по длине, а 100 —
    // нет. Собираем ровно за пределом: два сегмента по 48 и «.md».
    char path[128];
    memset(path, 'a', 48);
    path[48] = '/';
    memset(path + 49, 'b', 48);
    memcpy(path + 97, ".md", 4);
    TEST_ASSERT_TRUE(strlen(path) >= paths::kMaxPathLen);
    TEST_ASSERT_EQUAL(Result::TooLong, paths::check(path));

    // А заведомо короче предела — проходит (сегменты тоже в норме).
    char ok[128];
    memset(ok, 'a', 40);
    ok[40] = '/';
    memset(ok + 41, 'b', 40);
    memcpy(ok + 81, ".md", 4);
    TEST_ASSERT_TRUE(strlen(ok) < paths::kMaxPathLen);
    TEST_ASSERT_EQUAL(Result::Ok, paths::check(ok));
}

// --- Нормализация -----------------------------------------------------------

void test_normalize_lowercases()
{
    char out[paths::kMaxPathLen];
    TEST_ASSERT_EQUAL(Result::Ok, paths::normalize("Home/Air-Purifier.MD", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("home/air-purifier.md", out);
}

void test_normalize_adds_extension()
{
    char out[paths::kMaxPathLen];
    TEST_ASSERT_EQUAL(Result::Ok, paths::normalize("home/air-filter", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("home/air-filter.md", out);
}

void test_normalize_strips_dot_slash()
{
    char out[paths::kMaxPathLen];
    // Модель иногда пишет «./home/x.md». Это не попытка выйти наружу, а
    // просто привычка; отказывать из-за неё незачем.
    TEST_ASSERT_EQUAL(Result::Ok, paths::normalize("./home/x.md", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("home/x.md", out);
}

void test_normalize_strips_spaces()
{
    char out[paths::kMaxPathLen];
    TEST_ASSERT_EQUAL(Result::Ok, paths::normalize("  home/x.md  ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("home/x.md", out);
}

void test_normalize_rejects_traversal()
{
    char out[paths::kMaxPathLen];
    strcpy(out, "unchanged");
    TEST_ASSERT_EQUAL(Result::Traversal, paths::normalize("../config.json", out, sizeof(out)));
    // При отказе выходной буфер не тронут: иначе вызывающий мог бы случайно
    // использовать недопроверенный путь.
    TEST_ASSERT_EQUAL_STRING("unchanged", out);
}

void test_normalize_keeps_bad_extension_error()
{
    char out[paths::kMaxPathLen];
    // «file.txt» не превращается в «file.txt.md»: модель не поняла бы, куда
    // делся файл. Лучше честная ошибка.
    TEST_ASSERT_EQUAL(Result::BadExtension, paths::normalize("home/file.txt", out, sizeof(out)));
}

void test_normalize_empty()
{
    char out[paths::kMaxPathLen];
    TEST_ASSERT_EQUAL(Result::Empty, paths::normalize("", out, sizeof(out)));
    TEST_ASSERT_EQUAL(Result::Empty, paths::normalize(nullptr, out, sizeof(out)));
}

// --- Слаги ------------------------------------------------------------------

void test_slugify_latin()
{
    char out[64];
    TEST_ASSERT_TRUE(paths::slugify("Air Purifier Filter Reset", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("air-purifier-filter-reset", out);
}

void test_slugify_cyrillic()
{
    char out[64];
    TEST_ASSERT_TRUE(paths::slugify("Сброс ошибки очистителя", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("sbros-oshibki-ochistitelya", out);
}

void test_slugify_collapses_punctuation()
{
    char out[64];
    TEST_ASSERT_TRUE(paths::slugify("Фильтр — замена (каждые 6 месяцев)!", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("filtr-zamena-kazhdye-6-mesyatsev", out);
}

void test_slugify_no_leading_or_trailing_dash()
{
    char out[64];
    TEST_ASSERT_TRUE(paths::slugify("  ---Привет---  ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("privet", out);
}

void test_slugify_uppercase_cyrillic()
{
    char out[64];
    TEST_ASSERT_TRUE(paths::slugify("ЩЁЛОЧЬ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("shcheloch", out);
}

void test_slugify_empty_result()
{
    char out[64];
    // Только знаки — слага не выходит.
    TEST_ASSERT_FALSE(paths::slugify("!!! ???", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::slugify("", out, sizeof(out)));
}

void test_slugify_respects_length()
{
    char out[16];
    TEST_ASSERT_TRUE(paths::slugify("очень длинный заголовок документа", out, sizeof(out)));
    TEST_ASSERT_TRUE(strlen(out) < sizeof(out));
    // Обрезка не должна оставить хвостовой дефис.
    TEST_ASSERT_TRUE(out[strlen(out) - 1] != '-');
}

// --- Разбор пути ------------------------------------------------------------

void test_folder_of()
{
    char out[64];
    paths::folder_of("home/kitchen/x.md", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("home/kitchen", out);

    paths::folder_of("x.md", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_name_of()
{
    char out[64];
    paths::name_of("home/air-filter.md", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("air-filter", out);

    paths::name_of("dir/x.md", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("x", out);
}

// --- Тексты ошибок ----------------------------------------------------------

void test_error_text_mentions_example()
{
    // Модель исправляется по тексту ошибки (проверено живым прогоном), поэтому
    // в самых частых отказах должен быть пример правильного пути.
    TEST_ASSERT_NOT_NULL(strstr(paths::error_text(Result::BadChar), "home/"));
    TEST_ASSERT_NOT_NULL(strstr(paths::error_text(Result::Traversal), "home/"));
    TEST_ASSERT_NOT_NULL(strstr(paths::error_text(Result::BadExtension), ".md"));
    TEST_ASSERT_NOT_NULL(strstr(paths::error_text(Result::Empty), "home/"));
}

// --- Служебные имена в inbox/ (Р-050) ---------------------------------------

void test_inbox_path_from_iso()
{
    char out[paths::kMaxPathLen];
    TEST_ASSERT_TRUE(paths::inbox_path("2026-11-27T16:15:00+03:00", "note", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("inbox/note-20261127-161500.md", out);

    TEST_ASSERT_TRUE(paths::inbox_path("2026-11-27T16:15:09+03:00", "answer", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("inbox/answer-20261127-161509.md", out);

    // Путь обязан проходить ту же проверку, что и путь от модели.
    TEST_ASSERT_EQUAL(paths::Result::Ok, paths::check(out));
}

void test_inbox_path_rejects_bad_input()
{
    char out[paths::kMaxPathLen];

    // Не время: заметки с неразборчивой датой столкнулись бы друг с другом.
    TEST_ASSERT_FALSE(paths::inbox_path("", "note", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path(nullptr, "note", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path("2026-11-27", "note", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path("вчера", "note", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path("2026-11-27 16:15:00", "note", out, sizeof(out)));

    // Дата вне диапазона.
    TEST_ASSERT_FALSE(paths::inbox_path("1999-11-27T16:15:00+03:00", "note", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path("2026-13-27T16:15:00+03:00", "note", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path("2026-11-27T25:15:00+03:00", "note", out, sizeof(out)));

    // Префикс — только из списка: он попадает в путь и в проверку.
    TEST_ASSERT_FALSE(paths::inbox_path("2026-11-27T16:15:00+03:00", "смета", out, sizeof(out)));
    TEST_ASSERT_FALSE(paths::inbox_path("2026-11-27T16:15:00+03:00", "../x", out, sizeof(out)));

    // Маленький буфер — отказ, а не обрезанное имя.
    char tiny[8];
    TEST_ASSERT_FALSE(paths::inbox_path("2026-11-27T16:15:00+03:00", "note", tiny, sizeof(tiny)));
}

int main(int, char**)
{
    UNITY_BEGIN();

    RUN_TEST(test_accepts_simple_path);
    RUN_TEST(test_accepts_deep_but_allowed);
    RUN_TEST(test_rejects_too_deep);
    RUN_TEST(test_accepts_underscore_and_digit);
    RUN_TEST(test_accepts_dots_inside_name);

    RUN_TEST(test_rejects_absolute);
    RUN_TEST(test_rejects_traversal);
    RUN_TEST(test_rejects_dot_segment);
    RUN_TEST(test_rejects_empty_segment);

    RUN_TEST(test_rejects_cyrillic);
    RUN_TEST(test_rejects_spaces);
    RUN_TEST(test_rejects_other_bad_chars);
    RUN_TEST(test_rejects_bad_extension);
    RUN_TEST(test_accepts_uppercase_extension);

    RUN_TEST(test_rejects_too_long_segment);
    RUN_TEST(test_rejects_too_long_path);

    RUN_TEST(test_normalize_lowercases);
    RUN_TEST(test_normalize_adds_extension);
    RUN_TEST(test_normalize_strips_dot_slash);
    RUN_TEST(test_normalize_strips_spaces);
    RUN_TEST(test_normalize_rejects_traversal);
    RUN_TEST(test_normalize_keeps_bad_extension_error);
    RUN_TEST(test_normalize_empty);

    RUN_TEST(test_slugify_latin);
    RUN_TEST(test_slugify_cyrillic);
    RUN_TEST(test_slugify_collapses_punctuation);
    RUN_TEST(test_slugify_no_leading_or_trailing_dash);
    RUN_TEST(test_slugify_uppercase_cyrillic);
    RUN_TEST(test_slugify_empty_result);
    RUN_TEST(test_slugify_respects_length);

    RUN_TEST(test_folder_of);
    RUN_TEST(test_name_of);
    RUN_TEST(test_error_text_mentions_example);

    RUN_TEST(test_inbox_path_from_iso);
    RUN_TEST(test_inbox_path_rejects_bad_input);

    return UNITY_END();
}
