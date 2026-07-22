package com.hyll.wyble.wxapi;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

import com.hyll.wyble2.R;

public class BleForegroundService extends Service {
    private static final String CHANNEL_ID = "StarLineBleChannel";
    private static final int NOTIFICATION_ID = 1001;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Создаем Intent для открытия MainActivity при нажатии на уведомление
        Intent notificationIntent = new Intent(this, MainActivity.class);
        PendingIntent pendingIntent = PendingIntent.getActivity(
            this,
            0,
            notificationIntent,
            PendingIntent.FLAG_IMMUTABLE
        );

        // Создаем уведомление
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("StarLineBle")
            .setContentText("Подключение к автомобилю активно")
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(pendingIntent)
            .setPriority(Notification.PRIORITY_LOW)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build();

        // Запускаем сервис в foreground
        startForeground(NOTIFICATION_ID, notification);

        // Если сервис убит системой, перезапускаем его
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel serviceChannel = new NotificationChannel(
                CHANNEL_ID,
                "StarLineBle Service Channel",
                NotificationManager.IMPORTANCE_LOW
            );
            serviceChannel.setDescription("Канал для уведомлений фонового сервиса StarLineBle");
            serviceChannel.setShowBadge(false);

            NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (manager != null) {
                manager.createNotificationChannel(serviceChannel);
            }
        }
    }
}
