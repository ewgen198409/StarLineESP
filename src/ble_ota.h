/*
 * ble_ota.h
 *
 * Минимальный BLE OTA (Over-The-Air) поверх существующего BLE-стека ESP32.
 * Позволяет обновлять прошивку БЕЗ Android-приложения и БЕЗ Wi-Fi:
 *   - через nRF Connect (пишем чанки в OTA_DATA, "finish" в OTA_CTRL), либо
 *   - через Python-скрипт (bleak/pygatt), который читает .bin и заливает его.
 *
 * Безопасность: обе характеристики требуют ESP_GATT_PERM_WRITE_ENCRYPTED,
 * то есть OTA доступно ТОЛЬКО после BLE-bonding (как и команды сигнализации).
 * Посторонний без bonding не сможет даже начать запись.
 *
 * Партиции: требует таблицы с otadata + app0(ota_0) + app1(ota_1)
 * (в проекте partitions.csv уже содержит их).
 */
#ifndef BLE_OTA_H
#define BLE_OTA_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLECharacteristic.h>
#include <Update.h>

// UUID сервиса и характеристик OTA (независимы от сервиса сигнализации)
#define OTA_SERVICE_UUID "daf177f0-6f7a-4f5a-8f4a-4a3a2a1a0a01"
#define OTA_DATA_UUID    "daf177f1-6f7a-4f5a-8f4a-4a3a2a1a0a02"  // WRITE: чанки прошивки
#define OTA_CTRL_UUID    "daf177f2-6f7a-4f5a-8f4a-4a3a2a1a0a03"  // WRITE: "start:<size>" / "finish"

// Максимальный размер одного чанка (BLE MTU обычно 23..517 байт; берём с запасом)
#define OTA_MAX_CHUNK 512

// Helper macro for error handling with restart
#define OTA_ERROR_RESTART(msg) do { \
    Serial.println(msg); \
    Update.abort(); \
    delay(100); \
    ESP.restart(); \
} while(0)

class BleOtaCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic) override
  {
    String value = pCharacteristic->getValue();
    if (value.length() == 0) return;

    String uuid = pCharacteristic->getUUID().toString();

    if (uuid == OTA_CTRL_UUID)
    {
      // Управляющая команда: "start:<size>" или "finish"
      String cmd = value;
      cmd.trim();
      if (cmd.startsWith("start:"))
      {
        int total = cmd.substring(6).toInt();
        if (total > 0 && Update.begin(total))
        {
          Serial.printf("OTA: starting receive, size %d bytes\n", total);
        }
        else
        {
          Serial.printf("OTA: Update.begin() failed, error code: %d\n", Update.getError());
          // При ошибке начала OTA - перезагружаемся для восстановления
          delay(100);
          ESP.restart();
        }
      }
      else if (cmd == "finish")
      {
        if (Update.end(true))
        {
          Serial.println("OTA: write finished, restarting...");
          delay(200);
          ESP.restart();
        }
        else
        {
          Serial.printf("OTA: finish failed, error code: %d\n", Update.getError());
          Update.abort();
          // При ошибке завершения - перезагружаемся
          delay(100);
          ESP.restart();
        }
      }
    }
    else if (uuid == OTA_DATA_UUID)
    {
      // Чанк прошивки
      if (Update.write((uint8_t *)value.c_str(), value.length()) != value.length())
      {
        Serial.printf("OTA: chunk write failed, error code: %d\n", Update.getError());
        Update.abort();
        // При ошибке записи чанка - перезагружаемся для восстановления
        delay(100);
        ESP.restart();
      }
    }
  }
};

// Создаёт OTA-сервис и характеристики на уже запущенном BLE-сервере.
// Вызывать ПОСЛЕ BLEDevice::createServer() и до service->start() основного сервиса
// (или отдельным сервисом — порядок не важен, главное до BLEDevice::startAdvertising).
inline void initBleOta(BLEServer *server)
{
  BLEService *otaService = server->createService(OTA_SERVICE_UUID);

  BLECharacteristic *otaData = otaService->createCharacteristic(
      OTA_DATA_UUID, BLECharacteristic::PROPERTY_WRITE);
  otaData->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  otaData->setCallbacks(new BleOtaCallbacks());

  BLECharacteristic *otaCtrl = otaService->createCharacteristic(
      OTA_CTRL_UUID, BLECharacteristic::PROPERTY_WRITE);
  otaCtrl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  otaCtrl->setCallbacks(new BleOtaCallbacks());

  otaService->start();
}

#endif  // BLE_OTA_H