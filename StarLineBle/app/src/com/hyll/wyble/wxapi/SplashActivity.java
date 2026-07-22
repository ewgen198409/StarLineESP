package com.hyll.wyble.wxapi;

import com.hyll.wyble2.R;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.graphics.LinearGradient;
import android.graphics.Shader;
import android.graphics.Color;
import android.view.animation.Animation;
import android.view.animation.AnimationUtils;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

public class SplashActivity extends Activity {
    private static final int SPLASH_DURATION = 2000;
    private static final int REQUEST_ENABLE_BT = 1;
    private static final int REQUEST_PERMS = 2;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.splash);

        ImageView ivLoading = (ImageView) findViewById(R.id.ivLoading);
        Animation nissanAnim = AnimationUtils.loadAnimation(this, R.anim.nissan_fall);
        ivLoading.startAnimation(nissanAnim);

        TextView tvStarLine = (TextView) findViewById(R.id.tvStarLine);
        
        // Создаем серебристый градиент для текста
        LinearGradient textGradient = new LinearGradient(
            0, 0, 0, tvStarLine.getTextSize(),
            new int[]{0xFFF8F8F8, 0xFFC0C0C0, 0xFF808080},
            null,
            Shader.TileMode.CLAMP
        );
        tvStarLine.getPaint().setShader(textGradient);
        
        // Добавляем тень для эффекта выпуклости
        tvStarLine.setShadowLayer(3, 0, -2, Color.BLACK);
        
        Animation textAnim = AnimationUtils.loadAnimation(this, R.anim.text_rise);
        tvStarLine.startAnimation(textAnim);

        // Проверяем включен ли Bluetooth
        BluetoothAdapter bluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
        if (bluetoothAdapter == null) {
            // Устройство не поддерживает Bluetooth
            Toast.makeText(this, "Устройство не поддерживает Bluetooth", Toast.LENGTH_LONG).show();
            finish();
            return;
        }

        if (!bluetoothAdapter.isEnabled()) {
            // Сначала запрашиваем разрешения, потом включаем Bluetooth
            if (checkAndRequestPermissions()) {
                requestEnableBluetooth();
            }
        } else {
            // Bluetooth уже включен, продолжаем загрузку
            proceedToMain();
        }
    }

    private boolean checkAndRequestPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.BLUETOOTH_CONNECT}, REQUEST_PERMS);
                return false;
            }
        }
        return true;
    }

    private void requestEnableBluetooth() {
        Intent enableBtIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
        startActivityForResult(enableBtIntent, REQUEST_ENABLE_BT);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQUEST_PERMS) {
            boolean allGranted = true;
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    allGranted = false;
                    break;
                }
            }
            if (allGranted) {
                requestEnableBluetooth();
            } else {
                Toast.makeText(this, "Для работы приложения необходимы разрешения Bluetooth", Toast.LENGTH_LONG).show();
                finish();
            }
        } else {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        
        if (requestCode == REQUEST_ENABLE_BT) {
            if (resultCode == RESULT_OK) {
                // Bluetooth включен пользователем
                proceedToMain();
            } else {
                // Пользователь отказался включать Bluetooth
                Toast.makeText(this, "Для работы приложения необходимо включить Bluetooth", Toast.LENGTH_LONG).show();
                finish();
            }
        }
    }

    private void proceedToMain() {
        new Handler().postDelayed(new Runnable() {
            @Override
            public void run() {
                startActivity(new Intent(SplashActivity.this, MainActivity.class));
                finish();
            }
        }, SPLASH_DURATION);
    }
}
