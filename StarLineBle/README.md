# StarLineBle — единый редактируемый проект

Исходный XAPK распакован и объединён в единый APK. Все split-модули
(`config.xxhdpi`, `config.en`) влиты в основной проект, поэтому
собирается один APK-файл.

## Структура

```
work/
  base/          # ДЕКОМПИЛИРОВАННЫЙ проект (smali + res) — РЕДАКТИРУЙТЕ ЗДЕСЬ
  jadx_src/      # Java-исходники (1232 файла) для анализа кода
tools/
  apktool.jar    # apktool 2.11.1 (сборка)
  jadx/          # jadx 1.5.1 (GUI: tools/jadx/bin/jadx-gui)
blekey.keystore  # собственный ключ подписи
build.sh         # сборка + zipalign + подпись
merged_release.apk # ПОДПИСАННЫЙ релизный APK (готов к установке)
```

## Как редактировать

1. **Ресурсы** (layout, strings, drawable и т.д.) — правьте в `work/base/res/`.
2. **Смали-код** (Dalvik bytecode) — правьте в `work/base/smali*/`.
   Для удобства изучайте Java-аналоги в `work/jadx_src/`
   (открыть GUI: `tools/jadx/bin/jadx-gui work/jadx_src`).
3. **Манифест** — `work/base/AndroidManifest.xml`.

## Как собрать релиз

```bash
./build.sh
```

Пайплайн: apktool b → zipalign -p 4 → apksigner sign.
Результат: `merged_release.apk`.

Параметры ключа (при смене — пересоздайте keystore):
- alias: `blekey`
- store/key password: `android123`

## Проверка подписи и выравнивания

```bash
apksigner verify --print-certs merged_release.apk
zipalign -c -p 4 merged_release.apk   # должно вывести "Success"
```

## Установка на устройство

```bash
adb install -r --no-incremental merged_release.apk
```

Флаг `--no-incremental` обязателен, если устройство запрещает
incremental install (иначе ошибка "Incremental installation not allowed").