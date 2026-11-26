#pragma once

// Экран настроек API: адрес, ключ, модель, температура, уровень рассуждений.
//
// Все поля редактируются на устройстве, а не только в config.json через
// компьютер: без этого первый запуск требует вынуть SD-карту, что сводит на нет
// идею карманного помощника.
//
// Ключ вводится здесь и уходит **только в NVS**. В файл настроек он не
// попадает ни при каких условиях: SD-карту легко прочитать на компьютере.

#include <stdint.h>

#include "hw/keyboard.h"
#include "hw/settings_store.h"
#include "hw/wifi.h"
#include "util/settings.h"

namespace ui {

class ApiSettingsScreen {
public:
    /// Привязать к хранилищу настроек. Вызывается один раз в setup().
    void init(hw::SettingsStore& settings, hw::WifiManager& wifi);

    /// Открыть экран: подтянуть текущие значения в черновик.
    void onEnter();

    /// Обработать событие клавиатуры.
    /// @return true, если экран нужно закрыть
    bool onKey(const hw::KeyEvent& ev);

    /// Обработать касание: выбор поля.
    bool onTouch(int x, int y);

    /// Периодическая работа: экран ничего не ждёт, но интерфейс экрана един.
    void tick();

    /// Перерисовать то, что помечено изменившимся.
    void draw();

    /// Есть ли что перерисовывать.
    bool dirty() const;

    /// Забыть несохранённый ввод при выходе.
    void onExit();

private:
    /// Поля формы, в порядке отображения.
    enum class Field : uint8_t {
        BaseUrl,
        ApiKey,
        Model,
        Temperature,
        Reasoning,
        Search,
        SearchResults,
        Count,
    };

    static constexpr size_t kFieldCount = static_cast<size_t>(Field::Count);

    /// Что делает экран.
    enum class Mode : uint8_t {
        Browsing,  ///< стрелки выбирают поле
        Editing,   ///< ввод идёт в выбранное поле
    };

    const char* field_name(Field f) const;
    /// Значение поля как строка для отображения.
    void field_value(Field f, char* out, size_t out_len) const;

    /// Поле меняется стрелками, а не вводом текста (числа, перечисления).
    static bool stepper(Field f);

    /// Буфер поля, доступный для ввода, и его ёмкость. nullptr — поле не
    /// строковое (температура и уровень меняются стрелками).
    char* field_buffer(Field f, size_t* cap);
    /// Текущая длина содержимого строкового поля.
    static size_t text_len(const char* buf);

    void begin_edit();
    void confirm_edit();
    void cancel_edit();
    void adjust_value(int delta);

    /// Поле показывает ровно то, что в буфере (адрес, модель). У ключа на
    /// экране только хвост, поэтому каретка там смысла не имеет.
    static bool caret_editable(Field f);

    /// Вставить текст по месту каретки.
    ///
    /// Отдельно от обработки символа: пробел приходит как спецклавиша с пустым
    /// utf8, и вставка «символа события» дала бы пустую строку.
    void insert_text(const char* text);

    void edit_char(const hw::KeyEvent& ev);
    void edit_backspace();
    /// Удалить символ ПОД кареткой (клавиша Delete).
    void edit_delete();

    /// Сдвинуть каретку на символ по границе UTF-8.
    void caret_left();
    void caret_right();
    void caret_to_start();
    void caret_to_end();
    /// Привести каретку в допустимые границы: 0..длина, по границе символа.
    void clamp_caret();
    /// Записать каретку в конец содержимого поля.
    void caret_to_end_of_field();

    void save();
    void fail(const char* text);

    void draw_all();
    void draw_rows();
    void draw_field_row(Field f);

    /// Нарисовать значение строкового поля с кареткой и горизонтальной
    /// прокруткой.
    ///
    /// Адрес бывает длиной под 190 символов — вчетверо шире экрана. Без
    /// прокрутки каретка уезжала бы за правый край, и правка «на ощупь» была бы
    /// невозможна: видно конец строки, а вводишь в середину.
    /// @param bg цвет фона строки: заливка поля не должна вырезать чёрный
    ///           прямоугольник внутри подсвеченной строки
    void draw_caret_line(const char* value, int y, int room, uint16_t bg);
    void draw_hints();
    void draw_error();

    /// Очистить полосу на всю ширину.
    static void clear_band(int y, int h);

    hw::SettingsStore* _settings{nullptr};
    hw::WifiManager* _wifi{nullptr};

    Mode _mode{Mode::Browsing};
    Field _selected{Field::BaseUrl};

    /// Черновик настроек: правки применяются к нему, а в файл уходят по Ctrl+S.
    /// Иначе каждая буква писала бы SD-карту.
    settings::Config _draft;
    char _api_key[settings::kMaxApiKey]{};

    char _error[160]{};

    /// Позиция каретки в СИМВОЛАХ поля — смещение в байтах UTF-8 от начала
    /// строки. Живёт только в режиме правки; при входе в поле встаёт в конец,
    /// как и ожидается от «нажми и печатай».
    size_t _caret{0};

    /// В NVS лежит непустой ключ. Нужно, чтобы отличить «ключа нет» от «ключ
    /// есть, но мы его не показываем».
    bool _has_key{false};

    /// Текст в поле — сообщение об успехе, а не ошибка. Отдельный флаг вместо
    /// сравнения строк: строка ошибки приходит из разных мест.
    bool _notice_ok{false};

    bool _redraw_all{true};
    bool _redraw_rows{false};
    bool _redraw_error{false};
    bool _redraw_hints{false};
};

}  // namespace ui
