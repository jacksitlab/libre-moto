#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# build-osmand-fork.sh — Baut einen OsmAnd-Fork mit PR #25378
#   (AIDL: active route geometry + route lifecycle callbacks)
#
# Ergebnis: APK mit Paket net.osmand.plus (androidFull = OsmAnd~), Flavor legacy,
#   ABI arm64 — installierbar neben dem Original, übernimmt POIs/Favoriten/Karten.
#
# Voraussetzungen (werden geprüft):
#   - JDK 17
#   - Android SDK mit platform-36 + build-tools 36.0.0
#   - Android NDK 23
#   - git, wget/curl, python3, cmake, clang
#
# Usage:
#   ./build-osmand-fork.sh [output-dir]
#
# Environment (optional):
#   OSMAND_WORK   — Arbeitsverzeichnis  (default: ./osmand-build)
#   JAVA_HOME     — JDK 17 Pfad
#   ANDROID_HOME  — Android SDK Pfad
#   ANDROID_NDK   — Android NDK 23 Pfad
# ============================================================================

DIRBIN="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
ROOT="$DIRBIN/.."


WORK="$ROOT/osmand-build"
OUT="${1:-${WORK}/output}"
JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.16248370}"

PR_BRANCH="feature/osmand-route-polyline-export"
PR_USER="peresmishnyk"
PR_REPO="https://github.com/${PR_USER}/OsmAnd.git"

OSMAND_REPO="${OSMAND_REPO:-https://github.com/osmandapp/OsmAnd.git}"
CORE_LEGACY_REPO="https://github.com/osmandapp/OsmAnd-core-legacy.git"
RESOURCES_REPO="https://github.com/osmandapp/OsmAnd-resources.git"
BUILD_REPO="https://github.com/osmandapp/OsmAnd-build.git"

# Build-Konfiguration: androidFull (net.osmand.plus = OsmAnd~), legacy core, arm64
FLAVOR="androidFullLegacyArm64Debug"

# versionCode hoch genug für Upgrade über F-Droid 5.3.10 (versionCode 531003)
# F-Droid 5.4.4 hat 540403. Wir setzen 599999 — höher als alle F-Droid-Builds.
OSMAND_VERSION_CODE="${OSMAND_VERSION_CODE:-599999}"
OSMAND_VERSION_NAME="${OSMAND_VERSION_NAME:-5.5.0-libremoto}"

log()  { echo -e "\033[1;34m[build-osmand]\033[0m $*"; }
err()  { echo -e "\033[1;31m[ERROR]\033[0m $*" >&2; exit 1; }
ok()   { echo -e "\033[1;32m[OK]\033[0m $*"; }

# ---------------------------------------------------------------------------
# 1. Voraussetzungen prüfen
# ---------------------------------------------------------------------------
log "Prüfe Voraussetzungen …"

[[ -d "$JAVA_HOME" ]]      || err "JAVA_HOME nicht gefunden: $JAVA_HOME"
[[ -x "$JAVA_HOME/bin/java" ]] || err "java in JAVA_HOME nicht ausführbar"
[[ -d "$ANDROID_HOME" ]]   || err "ANDROID_HOME nicht gefunden: $ANDROID_HOME"
[[ -d "$ANDROID_NDK" ]]    || err "ANDROID_NDK nicht gefunden: $ANDROID_NDK"

command -v git    >/dev/null || err "git fehlt"
command -v python3 >/dev/null || err "python3 fehlt"
command -v cmake  >/dev/null || err "cmake fehlt (für core-legacy native build)"

# SDK Komponenten prüfen
[[ -d "$ANDROID_HOME/platforms/android-36" ]] || err "platform-36 fehlt. Installiere: sdkmanager \"platforms;android-36\""
[[ -d "$ANDROID_HOME/build-tools/36.0.0" ]]   || err "build-tools 36.0.0 fehlt. Installiere: sdkmanager \"build-tools;36.0.0\""

ok "Voraussetzungen erfüllt"

# ---------------------------------------------------------------------------
# 2. Arbeitsverzeichnis + Repos
# ---------------------------------------------------------------------------
mkdir -p "$WORK"
cd "$WORK"

# Verzeichnisstruktur (wie OsmAnd es erwartet):
#   $WORK/
#     OsmAnd/          — Hauptrepo
#     core-legacy/     — Native core
#     resources/       — Fonts, Voice, POI etc.
#     build/           — Build-Scripts (functions.sh für externals)

clone_or_update() {
    local url="$1" dir="$2" branch="${3:-master}"
    if [[ -d "$dir/.git" ]]; then
        log "Update $dir …"
        git -C "$dir" fetch origin "$branch"
        git -C "$dir" checkout "$branch"
        git -C "$dir" reset --hard "origin/$branch"
    else
        log "Klone $dir …"
        git clone --depth=1 -b "$branch" "$url" "$dir"
    fi
}

clone_or_update "$OSMAND_REPO"      "OsmAnd"      "master"
clone_or_update "$CORE_LEGACY_REPO" "core-legacy" "master"
clone_or_update "$RESOURCES_REPO"   "resources"   "master"
clone_or_update "$BUILD_REPO"       "build"        "master"

# NDK-Binär ausführbar machen (falls entpackt ohne +x)
[[ -x "$ANDROID_NDK/ndk-build" ]] || chmod +x "$ANDROID_NDK/ndk-build" 2>/dev/null || true

# Git-Identity für cherry-pick setzen (nur lokal im OsmAnd-Repo)
git -C "$WORK/OsmAnd" config user.email  "libre-moto@jacksitlab.de" 2>/dev/null || true
git -C "$WORK/OsmAnd" config user.name  "LibreMoto Build"          2>/dev/null || true

# ---------------------------------------------------------------------------
# 3. PR #25378 einbringen (cherry-pick)
# ---------------------------------------------------------------------------
cd "$WORK/OsmAnd"

# Prüfe ob der PR schon angewendet wurde
if git log --oneline | grep -q "route geometry and route lifecycle"; then
    log "PR #25378 bereits angewendet — überspringe"
else
    log "Bringe PR #25378 ein (route geometry AIDL) …"
    # Remote des PR-Autors hinzufügen + Branch fetchen
    if ! git remote get-url pr25378 >/dev/null 2>&1; then
        git remote add pr25378 "$PR_REPO"
    fi
    git fetch pr25378 "$PR_BRANCH"

    # Cherry-pick des einzelnen Commits (PR hat 1 Commit)
    PR_COMMIT=$(git log --format='%H' "pr25378/${PR_BRANCH}" -1)
    log "Cherry-pick commit $PR_COMMIT …"

    # Auf master cherry-picked der PR sauber (keine Konflikte).
    if ! git cherry-pick --no-commit "$PR_COMMIT"; then
        err "Cherry-pick fehlgeschlagen — Konflikte. Bitte manuell auflösen:\n  cd $WORK/OsmAnd && git cherry-pick --abort && git cherry-pick $PR_COMMIT"
    fi
    git commit -m "AIDL: active route geometry and route lifecycle callbacks (PR #25378)

Cherry-picked from peresmishnyk/OsmAnd feature/osmand-route-polyline-export
Adds getActiveRouteGeometry() + registerForRouteUpdates() to AIDL V2."
    ok "PR #25378 angewendet"
fi

# ---------------------------------------------------------------------------
# 3b. versionCode + versionName setzen (für Upgrade über F-Droid)
# ---------------------------------------------------------------------------
log "Setze versionCode=$OSMAND_VERSION_CODE, versionName=$OSMAND_VERSION_NAME …"
BUILD_GRADLE="OsmAnd/build.gradle"
python3 -c "
with open('$BUILD_GRADLE') as f: c = f.read()
import re
c = re.sub(r'versionCode\s+\d+', 'versionCode $OSMAND_VERSION_CODE', c, count=1)
c = re.sub(r'versionName\s+\"[^\"]*\"', 'versionName \"$OSMAND_VERSION_NAME\"', c, count=1)
with open('$BUILD_GRADLE', 'w') as f: f.write(c)
print('  versionCode=$OSMAND_VERSION_CODE versionName=$OSMAND_VERSION_NAME')
"

# ---------------------------------------------------------------------------
# 4. Environment setzen + Build
# ---------------------------------------------------------------------------
export JAVA_HOME
export ANDROID_HOME
export ANDROID_NDK
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_ROOT="$ANDROID_NDK"
export PATH="$JAVA_HOME/bin:/opt/data/.tools:$PATH"

# JVM-Dateinamen-Encoding auf UTF-8 erzwingen. Ohne UTF-8-Locale kann die JVM
# nicht-ASCII-Dateinamen nicht auflösen — z. B.
#   test-resources/search/ludwigstraße.obf.gz
# — und der Build bricht mit "Failed to create MD5 hash … does not exist" ab.
export LC_ALL="C.UTF-8"
export LANG="C.UTF-8"

# unzip-Wrapper (Python) verfügbar machen falls systemweit fehlt
if ! command -v unzip >/dev/null 2>&1; then
    export PATH="/opt/data/.tools:$PATH"
fi

log "Starte Build: $FLAVOR"
log "  JAVA_HOME=$JAVA_HOME"
log "  ANDROID_HOME=$ANDROID_HOME"
log "  ANDROID_NDK=$ANDROID_NDK"

cd "$WORK/OsmAnd"

# Gradle Wrapper nutzen (Gradle 8.11.1 ist im Repo)
chmod +x ./gradlew

# Gradle-JVM-Heap erhöhen. Das Repo lässt org.gradle.jvmargs kommentiert,
# daher läuft Gradle mit ~1.5 GB Default-Heap → bei OsmAnd GC-Thrashing
# ("build daemon has been stopped: JVM garbage collector is thrashing").
# Nach dem Clone setzen, da `git reset --hard` Änderungen sonst verwirft.
# Über GRADLE_XMX anpassbar (Default 6g, Maschine hat 62 GiB RAM).
GRADLE_XMX="${GRADLE_XMX:-6g}"
GP="$WORK/OsmAnd/gradle.properties"
grep -v '^org.gradle.jvmargs=' "$GP" > "$GP.tmp" && mv "$GP.tmp" "$GP"
printf 'org.gradle.jvmargs=-Xmx%s -XX:MaxMetaspaceSize=1g -Dfile.encoding=UTF-8\n' "$GRADLE_XMX" >> "$GP"
log "Gradle JVM-Heap: -Xmx$GRADLE_XMX"

# Erst nur arm64 (schneller als fat), legacy (kein OpenGL), androidFull (OsmAnd~)
log "Baue APK (das dauert beim ersten Mal 20-40 Min, hauptsächlich NDK native) …"
./gradlew "assemble${FLAVOR}" --console=plain --no-daemon

# ---------------------------------------------------------------------------
# 5. APK finden + kopieren
# ---------------------------------------------------------------------------
mkdir -p "$OUT"

# APK-Pfad (Gradle-Task-Name → APK-Name Konvention)
APK_DIR="OsmAnd/build/outputs/apk/androidFree/legacy/arm64/debug"
# Bei androidFull flavor liegt die APK in:
APK_DIR="OsmAnd/build/outputs/apk/androidFull/legacy/arm64/debug"

APK=$(find "$WORK/OsmAnd/OsmAnd/build/outputs" -name "*.apk" -path "*androidFull*" -path "*arm64*" 2>/dev/null | head -1)

if [[ -z "$APK" || ! -f "$APK" ]]; then
    # Fallback: suche alle APKs
    APK=$(find "$WORK/OsmAnd/OsmAnd/build/outputs" -name "*.apk" 2>/dev/null | head -1)
fi

if [[ -z "$APK" || ! -f "$APK" ]]; then
    err "Keine APK gefunden. Prüfe Build-Output unter $WORK/OsmAnd/OsmAnd/build/outputs/"
fi

log "Kopiere APK nach $OUT/ …"
cp "$APK" "$OUT/osmand-fork-arm64-debug.apk"
ok "Fertig: $OUT/osmand-fork-arm64-debug.apk"
log "Paket: net.osmand.plus (OsmAnd~ $OSMAND_VERSION_NAME mit Route-Geometry-AIDL)"
log "Installieren: adb install -r $OUT/osmand-fork-arm64-debug.apk"
