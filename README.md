# StarLineBle — сборка из Java-исходников

Проект собирается из Java-исходников и ресурсов оригинального приложения.

## Структура проекта

```
StarLineESP/
  app/
    res/          # Ресурсы приложения (layout, drawable, strings и т.д.)
    src/          # Java-исходники (редактируйте здесь)
    AndroidManifest.xml  # Манифест приложения
    build_app.sh  # Скрипт сборки
  blekey.keystore  # Ключ для подписи APK
  README.md       # Этот файл
```

## Как редактировать

1. **Ресурсы** (layout, strings, drawable и т.д.) — правьте в `app/res/`
2. **Java-код** — правьте в `app/src/com/hyll/wyble/`
3. **Манифест** — `app/AndroidManifest.xml`

## Как собрать приложение

```bash
./app/build_app.sh
```

Пайплайн сборки:
1. `aapt2 compile` — компиляция ресурсов
2. `aapt2 link` — создание base.apk
3. `javac` — компиляция Java-исходников
4. `dx` — конвертация в classes.dex
5. `apksigner` — подпись финального APK

Результат: `app/build/StarLineBle.apk`

## Параметры ключа подписи

- Файл: `blekey.keystore`
- Alias: `blekey`
- Пароль хранилища: `android123`
- Пароль ключа: `android123`

## Проверка подписи

```bash
apksigner verify --print-certs app/build/StarLineBle.apk
```

## Установка на устройство

```bash
adb install -r --no-incremental app/build/StarLineBle.apk
```

Флаг `--no-incremental` обязателен, если устройство запрещает incremental install.

## Примечания

- Для сборки требуется Android SDK (aapt2, dx, apksigner)
- Java 8+ требуется для компиляции исходников