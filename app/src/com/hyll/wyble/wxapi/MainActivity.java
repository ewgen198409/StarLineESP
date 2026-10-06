package com.hyll.wyble.wxapi;

import com.hyll.wyble2.R;

import android.Manifest;
import android.bluetooth.BluetoothDevice;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.media.MediaPlayer;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Vibrator;
import android.os.VibrationEffect;
import android.animation.ValueAnimator;
import android.graphics.Color;
import android.util.Log;
import android.view.View;
import android.view.animation.LinearInterpolator;
import android.widget.Button;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import com.hyll.wyble.ble.BleManager;
import com.hyll.wyble.ble.DeviceState;

import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity implements BleManager.BleCallback {
    private static final int REQ_PERMS = 1;

    private BleManager ble;
    private boolean connected = false;

    private TextView tvDevName, tvConn, tvStatusInfo, tvTemp;
    private Handler uiHandler;
    private boolean lastOhrana = false;
    private boolean firstState = true;
    private static final long PANIC_RESET_MS = 8000;
    private static final long TEMP_VOLT_SWITCH_MS = 10000;
    private long lastTempVoltSwitch = 0;
    private boolean showTemp = true;
    private final Runnable panicResetRunnable = new Runnable() {
        @Override
        public void run() {
            showNormalState();
            stopPanicSound();
        }
    };
    private ImageView ivCar, ivLock, ivBle, ivDor, ivEng, ivCtrlTop, ivCarState;
    private ImageView ivBrake, ivTrunkHood, ivShock;
    private ImageView ivValet, ivHand;
    private ImageView bar1, bar2, bar3, bar4;
    private ImageButton btnUnlock, btnLock, btnEngine, btnTrunk, btnHorn;
    private boolean engineOn = false;
    private boolean valetOn = false;
    private boolean ohranaOn = false;
    private boolean locksOn = false;
    private boolean handFreeActive = false;
    private boolean handFreeSent = false;
    private boolean handFreeSentOn = false;
    private boolean wasAboveThreshold = false; // предыдущее состояние сигнала относительно порога
    private boolean rssiInitialized = false; // флаг инициализации RSSI
    private int handFreeRssiThreshold = -80;
    private long thresholdCrossStartTime = 0; // время начала пересечения порога
    private boolean pendingThresholdCross = false; // ожидаем подтверждения пересечения
    private static final long THRESHOLD_CONFIRM_MS = 3000; // время подтверждения пересечения порога
    private boolean doorsOn = false;
    private boolean brakeOn = false;
    private boolean ignitionOn = false;
    private boolean hoodOn = false;
    private boolean trunkOn = false;
    private boolean waitingForState = false;
    private boolean pendingHandFreeAction = false;
    private long waitingForStateStartTime = 0;
    private static final long STATE_REQUEST_TIMEOUT_MS = 10000; // таймаут ожидания состояния
    private MediaPlayer panicMp;
    private ValueAnimator valetBorderAnim, handBorderAnim;
    private ValetBorderDrawable valetBorderDrawable, handBorderDrawable;
    private Vibrator vibrator;

    // --- Long-press на иконке Bluetooth для смены PIN ---
    private boolean pinDialogPending = false; // ожидаем получение PIN c устройства (GET)
    private boolean pinSetPending = false;    // ожидаем подтверждения установки PIN (SET)
    private int pendingNewPinValue = 0;       // новый PIN, который отправили на ESP32
    private boolean longPressTriggered = false;
    private Runnable longPressRunnable = new Runnable() {
        @Override
        public void run() {
            if (connected) {
                longPressTriggered = true;
                vibrate();
                // Запрашиваем текущий PIN с устройства
                pinDialogPending = true;
                ble.sendCommand(BleManager.CMD_GET_PIN);
            }
        }
    };
    private static final long LONG_PRESS_DURATION_MS = 2000;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        SharedPreferences prefs = getSharedPreferences("settings", MODE_PRIVATE);
        handFreeActive = prefs.getBoolean("handFreeActive", false);
        handFreeRssiThreshold = prefs.getInt("handFreeRssiThreshold", -80);

        tvDevName = (TextView) findViewById(R.id.tvDevName);
        tvConn = (TextView) findViewById(R.id.tvConn);
        ivCar = (ImageView) findViewById(R.id.ivCar);
        ivLock = (ImageView) findViewById(R.id.ivLock);
        ivBle = (ImageView) findViewById(R.id.ivBle);
        ivDor = (ImageView) findViewById(R.id.ivDor);
        ivEng = (ImageView) findViewById(R.id.ivEng);
        ivCtrlTop = (ImageView) findViewById(R.id.ivCtrlTop);
        ivBrake = (ImageView) findViewById(R.id.ivBrake);
        ivTrunkHood = (ImageView) findViewById(R.id.ivTrunkHood);
        ivShock = (ImageView) findViewById(R.id.ivShock);
        ivCarState = (ImageView) findViewById(R.id.ivCarState);
        ivValet = (ImageView) findViewById(R.id.ivValet);
        ivHand = (ImageView) findViewById(R.id.ivHand);
        bar1 = (ImageView) findViewById(R.id.bar1);
        bar2 = (ImageView) findViewById(R.id.bar2);
        bar3 = (ImageView) findViewById(R.id.bar3);
        bar4 = (ImageView) findViewById(R.id.bar4);
        tvTemp = (TextView) findViewById(R.id.tvTemp);
        tvStatusInfo = (TextView) findViewById(R.id.tvStatusInfo);
        btnLock = (ImageButton) findViewById(R.id.btnLock);
        btnUnlock = (ImageButton) findViewById(R.id.btnUnlock);
        btnEngine = (ImageButton) findViewById(R.id.btnEngine);
        btnTrunk = (ImageButton) findViewById(R.id.btnTrunk);
        btnHorn = (ImageButton) findViewById(R.id.btnHorn);

        ble = new BleManager(this, this);
        uiHandler = new Handler(Looper.getMainLooper());
        vibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);

        if (ble.isEnabled()) {
            if (ensurePermissions()) {
                tvConn.setText(R.string.scanning);
                ble.startScan();
            }
        }

        btnLock.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                vibrate();
                playSound(R.raw.fob_sound2);
                ble.sendCommand(BleManager.CMD_OHRANA_ON);
            }
        });
        btnUnlock.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                vibrate();
                playSound(R.raw.fob_sound2);
                ble.sendCommand(BleManager.CMD_OHRANA_OFF);
            }
        });
        // Long-press (>1s) для Engine
        setLongPressButton(btnEngine, 1000, new Runnable() {
            @Override
            public void run() {
                if (engineOn) {
                    playSound(R.raw.eng_off);
                    ble.sendCommand(BleManager.CMD_STOP_ENG);
                } else {
                    playSound(R.raw.start2);
                    ble.sendCommand(BleManager.CMD_START_ENG);
                }
            }
        });
        // Long-press (>1s) для Trunk
        setLongPressButton(btnTrunk, 1000, new Runnable() {
            @Override
            public void run() {
                playSound(R.raw.trunk);
                ble.sendCommand(BleManager.CMD_TRUNK);
            }
        });
        // Long-press (>1s) для Horn
        setLongPressButton(btnHorn, 1000, new Runnable() {
            @Override
            public void run() {
                playSound(R.raw.horn);
                ble.sendCommand(BleManager.CMD_HORN);
            }
        });
        ivCar.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (connected) {
                    ble.sendCommand(BleManager.CMD_ZAPROS);
                } else {
                    Toast.makeText(MainActivity.this, R.string.disconnected, Toast.LENGTH_SHORT).show();
                }
            }
        });
        ivValet.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (connected) {
                    ble.sendCommand(valetOn ? BleManager.CMD_VALET_OFF : BleManager.CMD_VALET_ON);
                } else {
                    Toast.makeText(MainActivity.this, R.string.disconnected, Toast.LENGTH_SHORT).show();
                }
            }
        });
        ivHand.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (connected) {
                    if (handFreeActive) {
                        handFreeActive = false;
                        SharedPreferences prefs = getSharedPreferences("settings", MODE_PRIVATE);
                        SharedPreferences.Editor editor = prefs.edit();
                        editor.putBoolean("handFreeActive", handFreeActive);
                        editor.apply();
                        handFreeSent = false;
                        handFreeSentOn = false;
                        wasAboveThreshold = false;
                        vibrate();
                        playSound(R.raw.fob_sound2);
                        setHandBorder(false);
                    } else {
                        showHandFreeDialog();
                    }
                } else {
                    Toast.makeText(MainActivity.this, R.string.disconnected, Toast.LENGTH_SHORT).show();
                }
            }
        });

        // --- Long-press на иконке car_state_bt_on (ivCarState) для смены PIN ---
        ivCarState.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                // Long-press сработал - отменяем отложенный запуск
                uiHandler.removeCallbacks(longPressRunnable);
                longPressTriggered = true;
                if (connected) {
                    vibrate();
                    pinDialogPending = true;
                    ble.sendCommand(BleManager.CMD_GET_PIN);
                } else {
                    Toast.makeText(MainActivity.this, R.string.disconnected, Toast.LENGTH_SHORT).show();
                }
                return true;
            }
        });
        // Отложенный запуск long-press по нажатию и отпусканию
        ivCarState.setOnTouchListener(new View.OnTouchListener() {
            private float downX, downY;
            @Override
            public boolean onTouch(View v, android.view.MotionEvent event) {
                switch (event.getAction()) {
                    case android.view.MotionEvent.ACTION_DOWN:
                        longPressTriggered = false;
                        downX = event.getX();
                        downY = event.getY();
                        uiHandler.postDelayed(longPressRunnable, LONG_PRESS_DURATION_MS);
                        return true;
                    case android.view.MotionEvent.ACTION_UP:
                    case android.view.MotionEvent.ACTION_CANCEL:
                        uiHandler.removeCallbacks(longPressRunnable);
                        // Если long-press не сработал и палец не уплыл далеко - обрабатываем как обычный клик
                        if (!longPressTriggered && connected) {
                            float dx = event.getX() - downX;
                            float dy = event.getY() - downY;
                            if (Math.abs(dx) < 20 && Math.abs(dy) < 20) {
                                // Обычный клик - запрос состояния
                                if (connected) {
                                    ble.sendCommand(BleManager.CMD_ZAPROS);
                                } else {
                                    Toast.makeText(MainActivity.this, R.string.disconnected, Toast.LENGTH_SHORT).show();
                                }
                            }
                        }
                        return true;
                }
                return false;
            }
        });
    }

    private void playSound(final int resId) {
        try {
            MediaPlayer mp = MediaPlayer.create(this, resId);
            if (mp != null) {
                mp.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                    @Override
                    public void onCompletion(MediaPlayer m) {
                        m.release();
                    }
                });
                mp.start();
            }
        } catch (Exception ignored) {
        }
    }

    private void playSoundTimes(final int resId, final int times) {
        if (times <= 0) return;
        playSound(resId);
        if (times > 1) {
            uiHandler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    playSoundTimes(resId, times - 1);
                }
            }, 350);
        }
    }

    private void vibrate() {
        if (vibrator == null) return;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                vibrator.vibrate(VibrationEffect.createOneShot(30, VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                vibrator.vibrate(30);
            }
        } catch (Exception ignored) {
        }
    }

    private boolean ensurePermissions() {
        List<String> perms = new ArrayList<String>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.BLUETOOTH_SCAN);
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.BLUETOOTH);
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_ADMIN) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.BLUETOOTH_ADMIN);
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (!perms.isEmpty()) {
            requestPermissions(perms.toArray(new String[0]), REQ_PERMS);
            return false;
        }
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_PERMS) {
            boolean ok = true;
            for (int r : grantResults) if (r != PackageManager.PERMISSION_GRANTED) ok = false;
            if (ok) ble.startScan();
            else Toast.makeText(this, R.string.perm_required, Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    public void onPinResponse(final int pin) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (pinSetPending) {
                    // Это ответ на SET - подтверждение нового PIN
                    pinSetPending = false;
                    if (pin >= 0) {
                        // Показываем окно с новым PIN-кодом через кастомный layout
                        final android.app.AlertDialog confirmDialog = new android.app.AlertDialog.Builder(
                            MainActivity.this, R.style.DarkAmoledDialog)
                            .setView(R.layout.dialog_pin_confirmation)
                            .create();
                        confirmDialog.show();
                        // Устанавливаем ширину
                        android.view.WindowManager.LayoutParams lp = new android.view.WindowManager.LayoutParams();
                        lp.copyFrom(confirmDialog.getWindow().getAttributes());
                        lp.width = (int)(getResources().getDisplayMetrics().widthPixels * 0.90f);
                        confirmDialog.getWindow().setAttributes(lp);
                        // Устанавливаем сообщение и кнопку
                        TextView tvMsg = (TextView) confirmDialog.findViewById(R.id.tvNewPinMessage);
                        if (tvMsg != null) {
                            tvMsg.setText(String.format(getString(R.string.pin_changed_message), pin));
                        }
                        Button btnOk = (Button) confirmDialog.findViewById(R.id.btnOkPinConfirm);
                        if (btnOk != null) {
                            btnOk.setOnClickListener(new View.OnClickListener() {
                                @Override
                                public void onClick(View v) {
                                    confirmDialog.dismiss();
                                }
                            });
                        }
                    } else {
                        Toast.makeText(MainActivity.this, R.string.pin_save_error, Toast.LENGTH_SHORT).show();
                    }
                } else if (pinDialogPending) {
                    // Это ответ на GET - открываем диалог смены PIN
                    pinDialogPending = false;
                    if (pin >= 0) {
                        showChangePinDialog(pin);
                    } else {
                        Toast.makeText(MainActivity.this, R.string.pin_save_error, Toast.LENGTH_SHORT).show();
                    }
                }
            }
        });
    }

    @Override
    public void onDevicesFound(final List<BluetoothDevice> devices) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isFinishing() || connected) return;

                if (devices.isEmpty()) {
                    tvConn.setText(R.string.disconnected);
                    new AlertDialog.Builder(MainActivity.this, R.style.DarkAmoledDialog)
                            .setTitle("Совместимые устройства не найдены")
                            .setPositiveButton("Повторить поиск", new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface dialog, int which) {
                                    tvConn.setText(R.string.scanning);
                                    ble.startScan();
                                }
                            })
                            .setNegativeButton("Закрыть", null)
                            .show();
                    return;
                }

                final CharSequence[] labels = new CharSequence[devices.size()];
                for (int i = 0; i < devices.size(); i++) {
                    BluetoothDevice device = devices.get(i);
                    String address = device.getAddress();
                    String addressSuffix = address.substring(Math.max(0, address.length() - 5));
                    labels[i] = device.getName() + " (" + addressSuffix + ")";
                }

                new AlertDialog.Builder(MainActivity.this, R.style.DarkAmoledDialog)
                        .setTitle("Выберите устройство")
                        .setItems(labels, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                ble.connect(devices.get(which));
                            }
                        })
                        .setNeutralButton("Повторить поиск", new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                tvConn.setText(R.string.scanning);
                                ble.startScan();
                            }
                        })
                        .setNegativeButton("Отмена", null)
                        .show();
            }
        });
    }

    @Override
    public void onConnectionState(final boolean isConnected, final boolean bonded) {
        this.connected = isConnected;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isConnected) {
                    if (bonded) {
                        tvDevName.setText(ble.getDeviceName());
                        tvConn.setText(R.string.connected);
                        ivBle.setImageResource(R.drawable.st_ble_on);
                        ivCarState.setImageResource(R.drawable.car_state_bt_on);
                        // Запускаем foreground service только после завершения bonding
                        startForegroundService();
                    } else {
                        tvDevName.setText(ble.getDeviceName());
                        tvConn.setText(R.string.bonding);
                        ivBle.setImageResource(R.drawable.st_ble_scan);
                        ivCarState.setImageResource(R.drawable.car_state_bt_off);
                        // Foreground service запустится после завершения bonding
                    }
                } else {
                    tvConn.setText(R.string.disconnected);
                    ivBle.setImageResource(R.drawable.st_ble);
                    ivCarState.setImageResource(R.drawable.car_state_bt_off);
                    // Останавливаем foreground service при отключении
                    stopForegroundService();
                    autoReconnect();
                }
            }
        });
    }

    private void autoReconnect() {
        if (ble.isEnabled() && ensurePermissions()) {
            tvConn.setText(R.string.scanning);
            ble.startReconnectScan();
        }
    }

    private void startForegroundService() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            if (checkSelfPermission(Manifest.permission.FOREGROUND_SERVICE) != PackageManager.PERMISSION_GRANTED) {
                // Запрашиваем разрешение на foreground service
                String[] perms = new String[]{Manifest.permission.FOREGROUND_SERVICE};
                requestPermissions(perms, 100);
                return;
            }
        }
        
        try {
            Intent serviceIntent = new Intent(this, BleForegroundService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(serviceIntent);
            } else {
                startService(serviceIntent);
            }
        } catch (Exception e) {
            Log.e("MainActivity", "Не удалось запустить foreground service", e);
        }
    }

    private void stopForegroundService() {
        Intent serviceIntent = new Intent(this, BleForegroundService.class);
        stopService(serviceIntent);
    }

    @Override
    public void onRssi(final int rssi) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (!connected || !handFreeActive) return;

                int level;
                if (rssi >= -70) level = 4;
                else if (rssi >= -80) level = 3;
                else if (rssi >= -90) level = 2;
                else if (rssi >= -100) level = 1;
                else level = 0;
                bar1.setAlpha(level >= 1 ? 1.0f : 0.3f);
                bar2.setAlpha(level >= 2 ? 1.0f : 0.3f);
                bar3.setAlpha(level >= 3 ? 1.0f : 0.3f);
                bar4.setAlpha(level >= 4 ? 1.0f : 0.3f);

                boolean isAboveThreshold = rssi > handFreeRssiThreshold;

                // Проверяем пересечение порога с дебаунсингом
                if (!waitingForState) {
                    if (isAboveThreshold != wasAboveThreshold) {
                        // Произошло пересечение порога - начинаем отсчет
                        if (!pendingThresholdCross) {
                            thresholdCrossStartTime = System.currentTimeMillis();
                            pendingThresholdCross = true;
                        } else if (System.currentTimeMillis() - thresholdCrossStartTime >= THRESHOLD_CONFIRM_MS) {
                            // Подтверждено пересечение порога - всегда отправляем запрос состояния
                            pendingThresholdCross = false;
                            waitingForState = true;
                            pendingHandFreeAction = isAboveThreshold; // true=закрыть, false=открыть
                            waitingForStateStartTime = System.currentTimeMillis();
                            ble.sendCommand(BleManager.CMD_ZAPROS);
                            wasAboveThreshold = isAboveThreshold;
                        }
                    } else {
                        // Сигнал стабилен на текущей стороне порога - сбрасываем ожидание подтверждения
                        pendingThresholdCross = false;
                    }
                }

                // Таймаут для waitingForState
                if (waitingForState && System.currentTimeMillis() - waitingForStateStartTime > STATE_REQUEST_TIMEOUT_MS) {
                    waitingForState = false;
                    pendingThresholdCross = false;
                }
            }
        });
    }

    @Override
    public void onState(final DeviceState s) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                boolean prevOhrana = lastOhrana;
                lastOhrana = s.ohrana;
                if (!firstState && prevOhrana != s.ohrana) {
                    if (s.ohrana && (s.doors || s.hood || s.trunk || s.brake)) {
                        playSoundTimes(R.raw.lock, 3);
                    } else {
                        playSound(s.ohrana ? R.raw.lock : R.raw.unlock);
                    }
                }
                firstState = false;
                if (s.trevoga) {
                    StringBuilder info = new StringBuilder();
                    info.append("Ohrana: ");
                    if (s.ohrana) {
                        info.append(getString(R.string.arm));
                    } else {
                        info.append(getString(R.string.disarm));
                    }
                    info.append("\nTrevoga: ").append(getString(R.string.panic))
                        .append(" (").append(s.getPanicReason()).append(")");
                    tvStatusInfo.setTextColor(0xFFFF3B30);
                    tvStatusInfo.setText(info.toString());
                    uiHandler.removeCallbacks(panicResetRunnable);
                    uiHandler.postDelayed(panicResetRunnable, PANIC_RESET_MS);
                    startPanicSound();
                } else {
                    uiHandler.removeCallbacks(panicResetRunnable);
                    showNormalState();
                    stopPanicSound();
                }

                doorsOn = s.doors;
                brakeOn = s.brake;
                engineOn = s.engine;
                ignitionOn = s.ignition;
                hoodOn = s.hood;
                trunkOn = s.trunk;

                // Обработка отложенного действия "Свободные руки"
                if (waitingForState) {
                    waitingForState = false;
                    boolean safeConditions = !doorsOn && !brakeOn && !ignitionOn && !engineOn && !hoodOn && !trunkOn;
                    if (safeConditions) {
                        if (pendingHandFreeAction && locksOn) {
                            handFreeSent = true;
                            ble.sendCommand(BleManager.CMD_OHRANA_OFF);
                        } else if (!pendingHandFreeAction && !locksOn) {
                            handFreeSentOn = true;
                            ble.sendCommand(BleManager.CMD_OHRANA_ON);
                        }
                    } else {
                        // Условия не выполнены - ставим флаг, чтобы не запрашивать состояние повторно
                        // Функция будет ждать перехода порога сигнала
                        if (pendingHandFreeAction) {
                            handFreeSent = true;
                        } else {
                            handFreeSentOn = true;
                        }
                    }
                }

                if (locksOn != s.locks) {
                    handFreeSent = false;
                    handFreeSentOn = false;
                    rssiInitialized = false; // сбрасываем инициализацию RSSI при изменении замков
                }
                locksOn = s.locks;
                ivLock.setImageResource(s.locks ? R.drawable.st_lock_sel : R.drawable.st_unlock_sel);
                ivDor.setImageResource(s.doors ? R.drawable.st_dor_open_onl : R.drawable.st_dor);
                ivEng.setImageResource(s.engine ? R.drawable.st_eng_onl : R.drawable.st_eng);
                ivBrake.setImageResource(s.brake ? R.drawable.car_indication_parking_blue_light
                                                 : R.drawable.car_indication_parking_blue_dark);
                ivTrunkHood.setImageResource((s.hood || s.trunk) ? R.drawable.car_control_icon_disarm_trunk_on_dark
                                                                : R.drawable.car_control_icon_disarm_trunk_on_light);
                ivShock.setImageResource(s.shock ? R.drawable.car_control_icon_shock_bpass_on_light
                                                 : R.drawable.car_control_icon_shock_bpass_on_dark);
                valetOn = s.valet;
                setValetBorder(s.valet);
                ohranaOn = s.ohrana;
                ivHand.setImageResource(s.ohrana ? R.drawable.hand_acc : R.drawable.hand);
                setHandBorder(handFreeActive);
                long now = System.currentTimeMillis();
                if (now - lastTempVoltSwitch >= TEMP_VOLT_SWITCH_MS) {
                    showTemp = !showTemp;
                    lastTempVoltSwitch = now;
                }
                if (showTemp) {
                    if (!Float.isNaN(s.temperature)) {
                        tvTemp.setText(String.format("%.1f°C", s.temperature));
                    } else {
                        tvTemp.setText("--°C");
                    }
                } else {
                    if (!Float.isNaN(s.voltage)) {
                        tvTemp.setText(String.format("%.1fV", s.voltage));
                    } else {
                        tvTemp.setText("--V");
                    }
                }

                btnEngine.setImageResource(s.engine ? R.drawable.ctrl_stop_selector : R.drawable.ctrl_start_selector);
                if (s.engine) {
                    ivCar.setImageResource(R.drawable.car_acc);
                    ivCtrlTop.setImageResource(R.drawable.ctrl_top_eng);
                } else if (s.ignition) {
                    ivCar.setImageResource(R.drawable.car_acc);
                    ivCtrlTop.setImageResource(R.drawable.ctrl_top_acc);
                } else {
                    ivCar.setImageResource(R.drawable.car);
                    ivCtrlTop.setImageResource(R.drawable.ctrl_top);
                }
            }
        });
    }

    private void showNormalState() {
        tvStatusInfo.setTextColor(getResources().getColor(R.color.text_white));
        tvStatusInfo.setText("Ohrana: " + (lastOhrana ? getString(R.string.arm) : getString(R.string.disarm)));
    }

    private void startPanicSound() {
        if (panicMp == null) {
            try {
                panicMp = MediaPlayer.create(this, R.raw.sixtone);
                if (panicMp != null) {
                    panicMp.setLooping(true);
                    panicMp.start();
                }
            } catch (Exception ignored) {
            }
        }
    }

    private void stopPanicSound() {
        if (panicMp != null) {
            try {
                if (panicMp.isPlaying()) panicMp.stop();
            } catch (Exception ignored) {
            }
            panicMp.release();
            panicMp = null;
        }
    }

    private void setValetBorder(boolean on) {
        if (valetBorderAnim != null) {
            valetBorderAnim.cancel();
            valetBorderAnim = null;
        }
        if (on) {
            if (valetBorderDrawable == null) {
                valetBorderDrawable = new ValetBorderDrawable(0x994A90E2, 3f, 10f, 8f);
            }
            ivValet.setBackground(valetBorderDrawable);
            final ValetBorderDrawable d = valetBorderDrawable;
            valetBorderAnim = ValueAnimator.ofFloat(0f, 18f);
            valetBorderAnim.setDuration(700);
            valetBorderAnim.setRepeatCount(ValueAnimator.INFINITE);
            valetBorderAnim.setInterpolator(new LinearInterpolator());
            valetBorderAnim.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override
                public void onAnimationUpdate(ValueAnimator animation) {
                    d.setDashOffset((Float) animation.getAnimatedValue());
                }
            });
            valetBorderAnim.start();
        } else {
            ivValet.setBackgroundResource(0);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (valetBorderAnim != null) valetBorderAnim.cancel();
        if (handBorderAnim != null) handBorderAnim.cancel();
        if (uiHandler != null) uiHandler.removeCallbacks(panicResetRunnable);
        stopPanicSound();
        ble.disconnect();
    }

    // Вспомогательный метод для long-press (>durationMs) на ImageButton
    // Возвращаем false из onTouch, чтобы стандартный обработчик кнопки (selector) работал
    private void setLongPressButton(final ImageButton button, final long durationMs, final Runnable action) {
        final Runnable longPressRunnable = new Runnable() {
            @Override
            public void run() {
                vibrate();
                action.run();
            }
        };
        button.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, android.view.MotionEvent event) {
                switch (event.getAction()) {
                    case android.view.MotionEvent.ACTION_DOWN:
                        uiHandler.postDelayed(longPressRunnable, durationMs);
                        return false; // пропускаем событие дальше — кнопка покажет selector
                    case android.view.MotionEvent.ACTION_UP:
                    case android.view.MotionEvent.ACTION_CANCEL:
                        uiHandler.removeCallbacks(longPressRunnable);
                        return false; // пропускаем событие дальше
                }
                return false;
            }
        });
    }

    private void showChangePinDialog(final int currentPin) {
        final android.app.AlertDialog dialog = new android.app.AlertDialog.Builder(MainActivity.this, R.style.DarkAmoledDialog)
            .setView(R.layout.dialog_change_pin)
            .create();

        dialog.show();
        // Устанавливаем ширину диалога
        android.view.WindowManager.LayoutParams lp = new android.view.WindowManager.LayoutParams();
        lp.copyFrom(dialog.getWindow().getAttributes());
        lp.width = (int)(getResources().getDisplayMetrics().widthPixels * 0.90f); // 90% ширины экрана
        dialog.getWindow().setAttributes(lp);

        // Показываем текущий PIN
        final TextView tvCurrentPin = (TextView) dialog.findViewById(R.id.tvCurrentPin);
        if (tvCurrentPin != null) {
            tvCurrentPin.setText(String.format(getString(R.string.current_pin_label), currentPin));
        }

        final android.widget.EditText etNewPin = (android.widget.EditText) dialog.findViewById(R.id.etNewPin);
        final Button btnCancel = (Button) dialog.findViewById(R.id.btnCancelPin);
        final Button btnSave = (Button) dialog.findViewById(R.id.btnSavePin);

        if (btnCancel != null) {
            btnCancel.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    dialog.dismiss();
                }
            });
        }

        if (btnSave != null) {
            btnSave.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    String pinStr = etNewPin != null ? etNewPin.getText().toString().trim() : "";
                    if (pinStr.length() == 0 || pinStr.length() > 6) {
                        Toast.makeText(MainActivity.this, R.string.pin_invalid, Toast.LENGTH_SHORT).show();
                        return;
                    }

                    // Проверяем, что все символы - цифры
                    boolean allDigits = true;
                    for (int i = 0; i < pinStr.length(); i++) {
                        if (!Character.isDigit(pinStr.charAt(i))) {
                            allDigits = false;
                            break;
                        }
                    }
                    if (!allDigits) {
                        Toast.makeText(MainActivity.this, R.string.pin_invalid, Toast.LENGTH_SHORT).show();
                        return;
                    }

                    long newPinLong = Long.parseLong(pinStr);
                    if (newPinLong > 999999L) {
                        Toast.makeText(MainActivity.this, R.string.pin_invalid, Toast.LENGTH_SHORT).show();
                        return;
                    }

                    // Отправляем новый PIN на устройство (5 байт: 0x61 + PIN uint32 LE)
                    byte[] pinBytes = new byte[4];
                    int newPinInt = (int) newPinLong;
                    pinBytes[0] = (byte) (newPinInt & 0xFF);
                    pinBytes[1] = (byte) ((newPinInt >> 8) & 0xFF);
                    pinBytes[2] = (byte) ((newPinInt >> 16) & 0xFF);
                    pinBytes[3] = (byte) ((newPinInt >> 24) & 0xFF);

                    pendingNewPinValue = newPinInt;
                    pinSetPending = true;
                    ble.sendCommand(BleManager.CMD_SET_PIN, pinBytes);
                    dialog.dismiss();
                }
            });
        }
    }

    private void showHandFreeDialog() {
        AlertDialog dialog = new AlertDialog.Builder(MainActivity.this, R.style.DarkAmoledDialog)
            .setTitle(R.string.hand_free_title)
            .setView(R.layout.dialog_hand_free)
            .create();

        dialog.show();

        dialog.findViewById(R.id.btnRssi70).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                handFreeRssiThreshold = -70;
                handFreeActive = true;
                saveHandFreeSettings();
                vibrate();
                playSound(R.raw.fob_sound2);
                setHandBorder(true);
                dialog.dismiss();
            }
        });

        dialog.findViewById(R.id.btnRssi80).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                handFreeRssiThreshold = -80;
                handFreeActive = true;
                saveHandFreeSettings();
                vibrate();
                playSound(R.raw.fob_sound2);
                setHandBorder(true);
                dialog.dismiss();
            }
        });

        dialog.findViewById(R.id.btnRssi90).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                handFreeRssiThreshold = -90;
                handFreeActive = true;
                saveHandFreeSettings();
                vibrate();
                playSound(R.raw.fob_sound2);
                setHandBorder(true);
                dialog.dismiss();
            }
        });
    }

    private void saveHandFreeSettings() {
        SharedPreferences prefs = getSharedPreferences("settings", MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean("handFreeActive", handFreeActive);
        editor.putInt("handFreeRssiThreshold", handFreeRssiThreshold);
        editor.apply();
    }

    private void setHandBorder(boolean on) {
        if (handBorderAnim != null) {
            handBorderAnim.cancel();
            handBorderAnim = null;
        }
        if (on) {
            if (handBorderDrawable == null) {
                handBorderDrawable = new ValetBorderDrawable(0x9900FF00, 3f, 10f, 8f);
            }
            ivHand.setBackground(handBorderDrawable);
            final ValetBorderDrawable d = handBorderDrawable;
            handBorderAnim = ValueAnimator.ofFloat(0f, 18f);
            handBorderAnim.setDuration(700);
            handBorderAnim.setRepeatCount(ValueAnimator.INFINITE);
            handBorderAnim.setInterpolator(new LinearInterpolator());
            handBorderAnim.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override
                public void onAnimationUpdate(ValueAnimator animation) {
                    d.setDashOffset((Float) animation.getAnimatedValue());
                }
            });
            handBorderAnim.start();
        } else {
            ivHand.setBackgroundResource(0);
        }
    }
}