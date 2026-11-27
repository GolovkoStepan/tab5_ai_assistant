#pragma once

// Экран «Параметры» (Ctrl+G, Р-049): яркость, подсветка, раскладка, пороги
// разряда, таймзона.
//
// Зачем отдельный экран, а не поля в Ctrl+A. В Ctrl+A уже семь полей про модель
// и ключ, и добавление туда ещё пяти сделало бы список из двенадцати строк, где
// половина — про API, половина — про само устройство. Здесь же то, что человек
// меняет вечером на диване: «ярко», «гаснет быстро», «переключи на русский».
//
// Правки идут в черновик и уходят в config.json по Ctrl+S — как на экране API.
// Яркость применяется сразу, ещё до сохранения: иначе «убавить яркость» было бы
// невидимым действием, а вечером именно она и нужна.
//
// Экран ничего не решает сам: шаги и ограничения — в util/device_settings
// (тесты на хосте), применение к железу — в main (подсветка живёт там).
// Экран только выставляет флаг «настройки сохранены», по которому main
// перечитывает их и применяет.

#include <stddef.h>
#include <stdint.h>

#include "hw/keyboard.h"
#include "hw/settings_store.h"
#include "util/device_settings.h"
#include "util/settings.h"

namespace ui {

class DeviceSettingsScreen {
public:
    /// Привязать к хранилищу настроек. Вызывается один раз в setup().
    void init(hw::SettingsStore& settings);

    /// Открыть экран: подтянуть текущие значения в черновик.
    void onEnter();

    /// Обработать событие клавиатуры.
    /// @return true, если экран нужно закрыть
    bool onKey(const hw::KeyEvent& ev);

    /// Обработать касание: выбор строки, повторный тап — шаг вперёд.
    /// @return true, если касание обработано
    bool onTouch(int x, int y);

    /// Периодическая работа не нужна: значения меняются по нажатиям.
    void tick() {}

    /// Перерисовать помеченное изменившимся.
    void draw();

    /// Есть ли что перерисовывать.
    bool dirty() const { return _redraw_all || _redraw_rows || _redraw_error || _redraw_hints; }

    /// Забыть несохранённые правки при выходе.
    void onExit();

    /// Настройки сохранились — main должен применить их к железу (яркость,
    /// раскладку, таймзону). Сбрасывает флаг.
    bool take_applied();

    /// Черновик для предварительного применения.
    ///
    /// Яркость, раскладка и пояс применяются сразу, ещё до Ctrl+S: «убавить
    /// яркость» иначе было бы невидимым действием. main берёт значения отсюда,
    /// а не из файла, — в файле они появятся только после сохранения.
    const settings::Config& draft() const { return _draft; }

    /// Есть ли несохранённые правки.
    bool unsaved() const;

    /// Подтянуть раскладку из сохранённых настроек.
    ///
    /// `Ctrl+Space` переключает раскладку с любого экрана и пишет её в файл
    /// сразу. Без этой синхронизации черновик экрана остался бы со старым
    /// значением, и `Ctrl+S` вернул бы раскладку назад — тихая потеря
    /// настройки, которую человек только что сделал.
    void sync_layout(bool russian);

    /// Подтянуть таймзону, сохранённую командой `tz` по Serial.
    ///
    /// Та же причина, что у раскладки: команда пишет файл сразу, а черновик
    /// экрана остался бы со старым поясом, и `Ctrl+S` вернул бы его назад.
    void sync_timezone(const char* posix);

    /// Шаг параметра снаружи экрана (команда `dev` по Serial, Р-046).
    ///
    /// Нужен, чтобы яркость и пороги разряда проверялись скриптом: стрелки
    /// скриптом не нажать, а приёмка иначе требует человека у платы.
    ///
    /// @return true, если значение изменилось
    bool apply_step(devsettings::Param param, int delta);

private:
public:
    /// Строк формы. Нужно отрисовке модуля, поэтому открыто.
    static constexpr size_t kRowCount = 7;

private:
    /// Строки формы, в порядке отображения.
    enum class Row : uint8_t {
        Brightness,
        Timeout,
        Layout,
        Warn,
        Critical,
        Shutdown,
        Timezone,
        Count,
    };
    static_assert(kRowCount == static_cast<size_t>(Row::Count), "kRowCount разошёлся с числом строк");

    /// Значение строки как текст.
    void row_value(Row r, char* out, size_t out_len) const;

    /// Изменить значение строки.
    /// @return true, если что-то изменилось
    bool adjust(Row r, int delta);

    /// Сохранить черновик в config.json.
    void save();

    void fail(const char* text);
    void notice(const char* text);

    void draw_all();
    void draw_rows();
    void draw_row(Row r);
    void draw_error();
    void draw_hints();

    hw::SettingsStore* _settings{nullptr};

    Row _selected{Row::Brightness};

    /// Черновик: правки уходят в файл только по Ctrl+S.
    settings::Config _draft{};

    char _error[192]{};
    /// Текст — сообщение об успехе, а не ошибка. Цвет другой.
    bool _notice_ok{false};

    /// Настройки сохранены, и их надо применить к железу.
    bool _applied{false};

    bool _redraw_all{true};
    bool _redraw_rows{false};
    bool _redraw_error{false};
    bool _redraw_hints{false};
};

}  // namespace ui
