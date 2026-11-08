#!/bin/sh
# Builds fm1tool.apk with the Android SDK's command-line tools only (no Gradle):
#   ANDROID_SDK=<sdk> sh build.sh [keystore]   (keystore: made on first use if missing)
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_SDK:?set ANDROID_SDK}
BT=$SDK/build-tools/34.0.0
JAR=$SDK/platforms/android-34/android.jar
KS=${1:-fm1tool.keystore}
OUT=build
mkdir -p dist; rm -rf $OUT && mkdir -p $OUT/classes $OUT/dex
$BT/aapt2 compile --dir res -o $OUT/res.zip
$BT/aapt2 link -I $JAR --manifest AndroidManifest.xml -o $OUT/base.apk $OUT/res.zip
javac --release 8 -parameters -nowarn -cp $JAR -d $OUT/classes $(find src -name '*.java')
$BT/d8 --release --min-api 26 --lib $JAR --output $OUT/dex $(find $OUT/classes -name '*.class')
cp $OUT/base.apk $OUT/unsigned.apk
(cd $OUT/dex && zip -q ../unsigned.apk classes.dex)
$BT/zipalign -f -p 4 $OUT/unsigned.apk $OUT/aligned.apk
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass fm1tool -keypass fm1tool -alias fm1tool \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=FM-1 Tool"
$BT/apksigner sign --ks "$KS" --ks-pass pass:fm1tool --key-pass pass:fm1tool --out dist/fm1tool.apk $OUT/aligned.apk
$BT/apksigner verify dist/fm1tool.apk
echo "built $(pwd)/dist/fm1tool.apk"
