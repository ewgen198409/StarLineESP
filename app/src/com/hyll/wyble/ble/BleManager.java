package com.hyll.wyble.ble;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.List;
import java.util.UUID;

/**
 * BLE-клиент для управления ESP32 (прошивка sl_data_esp32_test.ino).
 *
 * Устройство выступает GATT-сервером:
 *   Service : 4fafc201-1fb5-459e-8fcc-c5c9c331914b
 *   CMD     : beb5483e-36e1-4688-b7f5-ea07361b26a8  (WRITE, 1 байт команды)
 *   STATUS  : beb5483e-36e1-4688-b7f5-ea07361b26a9  (NOTIFY, 4 байта LE состояния)
 *   Имя     : StarLineBLE
 */
@SuppressWarnings("deprecation")
public class BleManager {
    public static final UUID SERVICE_UUID = UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b");
    public static final UUID CMD_UUID = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a8");
    public static final UUID STATUS_UUID = UUID.fromString("beb5483e-36e1-4688-b7f5-ea07361b26a9");
    private static final UUID CCC_DESCRIPTOR = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    public static final String DEVICE_NAME = "StarLineBLE";

    // Команды (1 байт), см. прошивку ESP32
    public static final byte CMD_ZAPROS = (byte) 0x42;     // запрос состояния
    public static final byte CMD_START_ENG = (byte) 0x21;  // запуск двигателя
    public static final byte CMD_STOP_ENG = (byte) 0x20;   // остановка двигателя
    public static final byte CMD_OHRANA_ON = (byte) 0x11;  // охрана вкл (закрыть)
    public static final byte CMD_OHRANA_OFF = (byte) 0x10; // охрана выкл (открыть)
    public static final byte CMD_VALET_OFF = (byte) 0x50;  // valet выкл
    public static final byte CMD_VALET_ON = (byte) 0x51;   // valet вкл
    public static final byte CMD_TRUNK = (byte) 0x23;      // багажник (GPIO32, 1 сек)
    public static final byte CMD_HORN = (byte) 0x24;       // сигнал (GPIO33, 1 сек)

    // Команды для работы с PIN-кодом BLE
    public static final byte CMD_GET_PIN = (byte) 0x60;    // запрос текущего PIN
    public static final byte CMD_SET_PIN = (byte) 0x61;    // установка нового PIN (5 байт с PIN uint32 LE)

    // Маркер PIN-ответа в статусной характеристике
    public static final byte PIN_RESPONSE_MARKER = (byte) 0x70;
    public static final byte PIN_RESPONSE_SUCCESS = 0x01;
    public static final byte PIN_RESPONSE_ERROR = 0x02;

    public interface BleCallback {
        void onConnectionState(boolean connected, boolean bonded);

        void onState(DeviceState state);

        void onRssi(int rssi);

        void onPinResponse(int pin);
    }

    private final Context context;
    private final BleCallback callback;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private BluetoothAdapter bluetoothAdapter;
    private BluetoothLeScanner scanner;
    private boolean isScanning = false;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic cmdChar;
    private BluetoothGattCharacteristic statusChar;
    private boolean connecting = false;
    private String deviceName = DEVICE_NAME; // имя подключённого устройства
    private BluetoothDevice bondedDevice;     // устройство, ожидающее bonding
    private boolean bondReceiverRegistered = false;
    private boolean isBonded = false;          // флаг завершённого сопряжения
    private boolean intentionalDisconnect = false; // флаг намеренного отключения для переподключения

    // Приёмник события сопряжения (bonding). ESP32 требует зашифрованный
    // канал, поэтому команды можно слать только ПОСЛЕ BOND_BONDED.
    private final BroadcastReceiver bondReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final String action = intent.getAction();
            if (BluetoothDevice.ACTION_BOND_STATE_CHANGED.equals(action)) {
                BluetoothDevice dev = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                int state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR);
                int prev = intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, BluetoothDevice.ERROR);
                if (dev != null && bondedDevice != null
                        && dev.getAddress().equals(bondedDevice.getAddress())) {
                    Log.d("BleManager", "bond state: " + prev + " -> " + state);
                    if (state == BluetoothDevice.BOND_BONDED) {
                        Log.d("BleManager", "bonding завершён, переподключаемся для шифрованного канала");
                        isBonded = true;
                        bondedDevice = null;
                        unregisterBondReceiver();
                        // После bonding переподключаемся — теперь канал будет
                        // зашифрован и ESP32 примет команды.
                        if (gatt != null) {
                            intentionalDisconnect = true; // помечаем, что отключение намеренное
                            gatt.disconnect();
                            gatt.close();
                            gatt = null;
                        }
                        connecting = false; // сбрасываем флаг, чтобы разрешить новое подключение
                        // Небольшая задержка перед переподключением
                        handler.postDelayed(new Runnable() {
                            @Override
                            public void run() {
                                connect(dev);
                            }
                        }, 500);
                    } else if (state == BluetoothDevice.BOND_NONE && prev == BluetoothDevice.BOND_BONDING) {
                        Log.e("BleManager", "bonding не удался (неверный PIN?)");
                        isBonded = false;
                        bondedDevice = null;
                        unregisterBondReceiver();
                    }
                }
            }
        }
    };

    private void registerBondReceiver() {
        if (bondReceiverRegistered) return;
        IntentFilter f = new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        context.registerReceiver(bondReceiver, f);
        bondReceiverRegistered = true;
    }

    private void unregisterBondReceiver() {
        if (!bondReceiverRegistered) return;
        try {
            context.unregisterReceiver(bondReceiver);
        } catch (IllegalArgumentException e) {
            Log.w("BleManager", "unregisterBondReceiver: уже не зарегистрирован");
        }
        bondReceiverRegistered = false;
    }

    public BleManager(Context context, BleCallback callback) {
        this.context = context;
        this.callback = callback;
        BluetoothManager bm = (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (bm != null) {
            bluetoothAdapter = bm.getAdapter();
            if (bluetoothAdapter != null) {
                scanner = bluetoothAdapter.getBluetoothLeScanner();
            }
        }
    }

    public boolean isEnabled() {
        return bluetoothAdapter != null && bluetoothAdapter.isEnabled();
    }

    public String getDeviceName() {
        return deviceName;
    }

    public void startScan() {
        if (scanner == null) {
            Log.e("BleManager", "startScan: scanner == null (Bluetooth выключен или недоступен)");
            return;
        }
        if (isScanning) {
            Log.d("BleManager", "startScan: сканирование уже запущено");
            return;
        }
        Log.d("BleManager", "startScan: ищем устройство '" + DEVICE_NAME + "'");
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        // Сканируем без фильтра: на части устройств/Android фильтр по имени
        // (ScanFilter.setDeviceName) мешает обнаружению. Имя проверяем в onScanResult.
        try {
            scanner.startScan(null, settings, scanCallback);
            isScanning = true;
        } catch (Exception e) {
            Log.e("BleManager", "Ошибка запуска сканирования: " + e.getMessage());
            isScanning = false;
        }
    }

    public void stopScan() {
        if (scanner != null && bluetoothAdapter != null && bluetoothAdapter.isEnabled() && isScanning) {
            try {
                scanner.stopScan(scanCallback);
            } catch (Exception e) {
                Log.e("BleManager", "Ошибка остановки сканирования: " + e.getMessage());
            }
        }
        isScanning = false;
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();
            String name = device != null ? device.getName() : null;
            Log.d("BleManager", "scan: name=" + name + " addr=" + (device != null ? device.getAddress() : "?"));
            if (device != null && DEVICE_NAME.equals(name)) {
                Log.d("BleManager", "найдено целевое устройство, подключаемся");
                stopScan();
                connect(device);
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            Log.e("BleManager", "onScanFailed: код ошибки " + errorCode);
            isScanning = false;
        }
    };

    public void connect(BluetoothDevice device) {
        if (connecting || gatt != null) return;
        connecting = true;

        // Если устройство ещё не сопряжено (bonding) — инициируем сопряжение.
        // ESP32 настроен на требование зашифрованного канала (PIN), поэтому
        // без bonding запись команд будет отклонена стеком.
        int bondState = device.getBondState();
        if (bondState == BluetoothDevice.BOND_NONE) {
            Log.d("BleManager", "устройство не сопряжено, запускаем bonding");
            isBonded = false;
            bondedDevice = device;
            registerBondReceiver();
            // На Android 6+ createBond() инициирует сопряжение; PIN нужно
            // ввести в системном диалоге (должен совпадать с PIN на ESP32).
            boolean started = device.createBond();
            Log.d("BleManager", "createBond() -> " + started);
            // GATT-подключение всё равно устанавливаем — процесс bonding
            // идёт поверх него, а после BOND_BONDED мы переподключимся.
        } else {
            Log.d("BleManager", "устройство уже сопряжено (bondState=" + bondState + ")");
            isBonded = true;
        }

        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
    }

    public void disconnect() {
        stopScan();
        if (gatt != null) {
            gatt.disconnect();
            gatt.close();
            gatt = null;
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            connecting = false;
            Log.d("BleManager", "onConnectionStateChange status=" + status + " newState=" + newState);
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                if (g.getDevice() != null) deviceName = g.getDevice().getName();
                g.discoverServices();
                g.readRemoteRssi(); // запрашиваем уровень сигнала
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                g.close();
                if (gatt == g) gatt = null;
                if (!intentionalDisconnect) {
                    isBonded = false;
                    handler.post(new Runnable() {
                        @Override
                        public void run() {
                            callback.onConnectionState(false, false);
                        }
                    });
                } else {
                    // Это намеренное отключение для переподключения после bonding
                    // Не уведомляем UI, чтобы не запускать сканирование заново
                    intentionalDisconnect = false;
                }
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            Log.d("BleManager", "onServicesDiscovered status=" + status);
            BluetoothGattService svc = g.getService(SERVICE_UUID);
            if (svc == null) {
                Log.e("BleManager", "сервис " + SERVICE_UUID + " не найден!");
                g.disconnect();
                return;
            }
            cmdChar = svc.getCharacteristic(CMD_UUID);
            statusChar = svc.getCharacteristic(STATUS_UUID);
            if (statusChar != null) {
                g.setCharacteristicNotification(statusChar, true);
                BluetoothGattDescriptor desc = statusChar.getDescriptor(CCC_DESCRIPTOR);
                if (desc != null) {
                    desc.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    g.writeDescriptor(desc);
                }
            }
            handler.post(new Runnable() {
                @Override
                public void run() {
                    callback.onConnectionState(true, isBonded);
                }
            });
            // Запрос состояния отправляем ПОСЛЕ завершения записи дескриптора
            // уведомлений (onDescriptorWrite), иначе writeCharacteristic может
            // быть отброшен BLE-стеком как вложенная операция.
        }

        @SuppressWarnings("deprecation")
        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor descriptor, int status) {
            // Когда дескриптор включения уведомлений записан — шлём запрос состояния,
            // НО только если канал уже зашифрован (bonding завершён). Иначе ESP32
            // отклонит запись команды по незашифрованному каналу.
            if (status != BluetoothGatt.GATT_SUCCESS) {
                Log.e("BleManager", "onDescriptorWrite: запись дескриптора завершилась ошибкой status=" + status);
                return;
            }
            if (CCC_DESCRIPTOR.equals(descriptor.getUuid())) {
                BluetoothDevice dev = g.getDevice();
                if (dev != null && dev.getBondState() == BluetoothDevice.BOND_BONDED) {
                    Log.d("BleManager", "onDescriptorWrite status=" + status + " -> запрос состояния (CMD_ZAPROS)");
                    sendCommand(CMD_ZAPROS);
                } else {
                    Log.d("BleManager", "onDescriptorWrite: канал ещё не сопряжён, запрос состояния отложен до bonding");
                }
            }
        }

        @Override
        public void onReadRemoteRssi(BluetoothGatt g, int rssi, int status) {
            // Передаём уровень сигнала в UI и планируем повторный опрос раз в 2 сек
            callback.onRssi(rssi);
            if (gatt != null) {
                handler.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (gatt != null) gatt.readRemoteRssi();
                    }
                }, 2000);
            }
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            handleChange(c);
        }

        // API 33+
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            handleChange(c);
        }

        private void handleChange(BluetoothGattCharacteristic c) {
            if (!STATUS_UUID.equals(c.getUuid())) return;
            byte[] v = c.getValue();
            if (v == null || v.length < 4) return;

            // Проверяем, не является ли это PIN-ответом (маркер 0x70, 6 байт)
            if (v.length >= 6 && v[0] == PIN_RESPONSE_MARKER) {
                final int pinType = v[1] & 0xFF;
                final int pin = (v[2] & 0xFF) | ((v[3] & 0xFF) << 8)
                              | ((v[4] & 0xFF) << 16) | ((v[5] & 0xFF) << 24);
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (pinType == PIN_RESPONSE_SUCCESS) {
                            callback.onPinResponse(pin);
                        } else {
                            callback.onPinResponse(-1); // error
                        }
                    }
                });
                return;
            }

            DeviceState st = DeviceState.fromWord(v);
            handler.post(new Runnable() {
                @Override
                public void run() {
                    callback.onState(st);
                }
            });
        }
    };

    public void sendCommand(byte cmd) {
        if (gatt == null || cmdChar == null) return;
        cmdChar.setValue(new byte[]{cmd});
        gatt.writeCharacteristic(cmdChar);
    }

    // Отправка мультибайтовой команды (для CMD_SET_PIN: 5 байт)
    public void sendCommand(byte cmd, byte[] extraData) {
        if (gatt == null || cmdChar == null) return;
        byte[] full = new byte[1 + extraData.length];
        full[0] = cmd;
        System.arraycopy(extraData, 0, full, 1, extraData.length);
        cmdChar.setValue(full);
        gatt.writeCharacteristic(cmdChar);
    }
}
