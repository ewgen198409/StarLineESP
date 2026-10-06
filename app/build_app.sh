#!/bin/bash
# Сборка нового приложения StarLineBle из Java-исходников + ресурсов оригинала.
# Пайплайн: aapt2 compile -> aapt2 link -> javac -> dx -> apksigner
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

rm -rf "$OUT"
mkdir -p "$GEN" "$OBJ" "$COMP"

echo "[1/5] aapt2 compile resources ..."
"$AAPT2" compile --dir "$APP/res" -o "$COMP/" 2>&1 | tail -20

echo "[2/5] aapt2 link -> base.apk ..."
"$AAPT2" link "$COMP"/*.flat \
  -I "$PLATFORM" \
  --manifest "$APP/AndroidManifest.xml" \
  --java "$GEN" \
  --min-sdk-version 26 \
  --target-sdk-version 34 \
  --version-code 1 \
  --version-name "2.0" \
  --no-version-vectors \
  -o "$OUT/base.apk" 2>&1 | tail -30

echo "[3/5] javac compile java ..."
find "$APP/src" -name "*.java" > "$OUT/srcs.txt"
find "$GEN" -name "*.java" >> "$OUT/srcs.txt"
javac -source 1.8 -target 1.8 -nowarn -cp "$PLATFORM" -d "$OBJ" @"$OUT/srcs.txt" 2>&1 | tail -30

echo "[4/5] dx -> classes.dex ..."
"$DX" --dex --min-sdk-version=26 --output "$OUT/classes.dex" "$OBJ"

echo "[5/5] package + sign ..."
cp "$OUT/base.apk" "$OUT/unsigned.apk"
cd "$OUT"
zip -q -r "unsigned.apk" "classes.dex"
cd ../..
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-key-alias "$ALIAS" \
  --ks-pass "pass:$STORE_PASS" --key-pass "pass:$KEY_PASS" \
  --out "$OUT/StarLineBle.apk" "$OUT/unsigned.apk"

echo "Готово: $OUT/StarLineBle.apk"
"$APKSIGNER" verify --print-certs "$OUT/StarLineBle.apk" 2>&1 | head -5

echo ""
echo "Установка на устройство..."
adb install -r --no-incremental "$OUT/StarLineBle.apk" 2>&1 || echo "Устройство не найдено или adb недоступен"
