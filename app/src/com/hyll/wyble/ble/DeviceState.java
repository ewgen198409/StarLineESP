package com.hyll.wyble.ble;

/**
 * Декодирование 32-битного слова состояния + температура, которые ESP32 шлёт в
 * BLE-характеристику STATUS (6 байт, little-endian):
 *   байты 0-3: слово состояния (см. биты ниже)
 *   байты 4-5: температура int16 (град * 10), 0x8000 = нет данных
 *
 * Биты инвертированы относительно "логической единицы" (см. прошивку
 * sl_data_esp32_test.ino, GoodResponse_received_from_starline):
 *   bit 0  -> Doors   (1 = закрыто в слове => doors=false)
 *   bit 2  -> Trunk
 *   bit 3  -> Hood
 *   bit 5  -> Locks
 *   bit 6  -> Ohrana (охрана)
 *   bit 7  -> Engine
 *   bit 12 -> Trevoga (тревога)
 *   bit 8  -> Shock sensor (датчик удара/наклона)
 *   bit 15 -> Valet
 *   bit 18 -> Brake
 *   bit 19 -> Ignition
 */
public class DeviceState {
    public boolean doors;
    public boolean trunk;
    public boolean hood;
    public boolean locks;
    public boolean ohrana;
    public boolean engine;
    public boolean trevoga;
    public boolean shock;
    public boolean valet;
    public boolean brake;
    public boolean ignition;
    public float temperature = Float.NaN; // градусы Цельсия
    public float voltage = Float.NaN;   // вольты

    /**
     * Декодировать 6 байт: [4 байта слово] + [2 байта температура int16*10]
     */
    public static DeviceState fromWord(byte[] v) {
        if (v == null || v.length < 6) return fromWord(v, 0);
        return fromWord(v, 0);
    }

    public static DeviceState fromWord(byte[] v, int extraOffset) {
        DeviceState s = new DeviceState();
        if (v == null || v.length < 4 + extraOffset) return s;
        int data = (v[0 + extraOffset] & 0xFF)
                 | ((v[1 + extraOffset] & 0xFF) << 8)
                 | ((v[2 + extraOffset] & 0xFF) << 16)
                 | ((v[3 + extraOffset] & 0xFF) << 24);
        s.doors = !bit(data, 0);
        s.trunk = !bit(data, 2);
        s.hood = !bit(data, 3);
        s.locks = !bit(data, 5);
        s.ohrana = !bit(data, 6);
        s.engine = !bit(data, 7);
        s.trevoga = !bit(data, 12);
        s.shock = !bit(data, 8);
        s.valet = !bit(data, 15);
        s.brake = !bit(data, 18);
        s.ignition = !bit(data, 19);

    // Температура: байты 4-5 (int16 LE, град*10)
    if (v.length >= 6 + extraOffset) {
        int raw = (v[4 + extraOffset] & 0xFF) | ((v[5 + extraOffset] & 0xFF) << 8);
        if (raw != (int)0x8000) {
            s.temperature = raw / 10.0f;
        }
    }
    // Напряжение: байты 6-7 (int16 LE, в*10)
    if (v.length >= 8 + extraOffset) {
        int raw = (v[6 + extraOffset] & 0xFF) | ((v[7 + extraOffset] & 0xFF) << 8);
        if (raw != (int)0x8000) {
            s.voltage = raw / 10.0f;
        }
    }
    return s;
}

    private static boolean bit(int data, int b) {
        return (data & (1 << b)) != 0;
    }

    /**
     * Причина срабатывания тревоги (Trevoga), определяется по активным
     * битам слова состояния. Возвращает читаемую строку на русском.
     */
    public String getPanicReason() {
        StringBuilder sb = new StringBuilder();
        if (shock) sb.append("Удар/Наклон; ");
        if (doors) sb.append("Двери; ");
        if (hood) sb.append("Капот; ");
        if (trunk) sb.append("Багажник; ");
        if (ignition) sb.append("Зажигание; ");
        if (brake) sb.append("Тормоз; ");
        if (sb.length() == 0) return "Неизвестно";
        // убираем лишний "; " в конце
        String r = sb.toString();
        return r.substring(0, r.length() - 2);
    }
}
