#!/bin/bash
# Сборка нового приложения StarLineBle из Java-источников + ресурсов оригинала.
# Пайплайн: aapt2 compile -> aapt2 link -> javac -> dx -> apksigner
# Версия хранится в app/build_version.txt и автоматически повышается при каждой сборке.
set -e
cd "$(dirname "$0")/.."

SDK="$HOME/Android/Sdk"
BUILD_TOOLS="$SDK/build-tools/34.0.0"
DX_HOME="$SDK/build-tools/30.0.3"
PLATFORM="$SDK/platforms/android-34/android.jar"
JAVA_HOME="/usr/lib/jvm/java-21-openjdk-amd64"
PATH="$JAVA_HOME/bin:$PATH"
AAPT2="$BUILD_TOOLS/aapt2"
DX="$DX_HOME/dx"
APKSIGNER="$BUILD_TOOLS/apksigner"
KEYSTORE="blekey.keystore"
ALIAS="blekey"
STORE_PASS="android123"
KEY_PASS="android123"

APP="app"
OUT="app/build"
GEN="$OUT/gen"
OBJ="$OUT/obj"
COMP="$OUT/compiled"
# Файл версии хранится ВНЕ $OUT, чтобы rm -rf $OUT не удалял его
VERSION_FILE="app/build_version.txt"

# --- Авто-версионирование ---
# Читаем текущую версию (или начинаем с 1), повышаем на 1.
# versionCode = major*10000 + minor*100 + patch
# versionName = "major.minor.patch"
if [ -f "$VERSION_FILE" ]; then
    read -r MAJOR MINOR PATCH < "$VERSION_FILE"
else
    MAJOR=1
    MINOR=0
    PATCH=0
fi

# Повышаем PATCH при каждой сборке
PATCH=$((PATCH + 1))

# Сохраняем новую версию
echo "$MAJOR $MINOR $PATCH" > "$VERSION_FILE"

rm -rf "$OUT"
mkdir -p "$GEN" "$OBJ" "$COMP"

VERSION_CODE=$((MAJOR * 10000 + MINOR * 100 + PATCH))
VERSION_NAME="${MAJOR}.${MINOR}.${PATCH}"

echo "Версия: $VERSION_NAME (code=$VERSION_CODE)"

echo "[1/5] aapt2 compile resources ..."
"$AAPT2" compile --dir "$APP/res" -o "$COMP/" 2>&1 | tail -20

echo "[2/5] aapt2 link -> base.apk ..."
"$AAPT2" link "$COMP"/*.flat \
  -I "$PLATFORM" \
  --manifest "$APP/AndroidManifest.xml" \
  --java "$GEN" \
  --min-sdk-version 26 \
  --target-sdk-version 34 \
  --version-code "$VERSION_CODE" \
  --version-name "$VERSION_NAME" \
  --no-version-vectors \
  -o "$OUT/base.apk" 2>&1 | tail -30

echo "[3/5] javac compile java ..."
find "$APP/src" -name "*.java" > "$OUT/srcs.txt"
find "$GEN" -name "*.java" >> "$OUT/srcs.txt"
javac -source 1.8 -target 1.8 -nowarn -cp "$PLATFORM" -d "$OBJ" @"$OUT/srcs.txt" 2>&1 | tail -30

echo "[4/5] dx -> classes.dex ..."
"$DX" --dex --min-sdk-version=26 --output "$OUT/classes.dex" "$OBJ"

echo "[5/5] package + sign ..."
APK_FILE="$OUT/StarLineBle_${VERSION_NAME}.apk"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
cd "$OUT"
zip -q -r "unsigned.apk" "classes.dex"
cd ../..
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-key-alias "$ALIAS" \
  --ks-pass "pass:$STORE_PASS" --key-pass "pass:$KEY_PASS" \
  --out "$APK_FILE" "$OUT/unsigned.apk"

echo "Готово: $APK_FILE"
"$APKSIGNER" verify --print-certs "$APK_FILE" 2>&1 | head -5

echo ""
echo "Установка на устройство..."
adb install -r --no-incremental "$APK_FILE" 2>&1 || echo "Устройство не найдено или adb недоступен"
