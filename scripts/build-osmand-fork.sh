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

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_DIR="${SCRIPT_DIR}/patches"

WORK="${OSMAND_WORK:-${SCRIPT_DIR}/../osmand-build}"
OUT="${1:-${WORK}/output}"
JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.16248370}"

OSMAND_REPO="${OSMAND_REPO:-https://github.com/osmandapp/OsmAnd.git}"
CORE_LEGACY_REPO="https://github.com/osmandapp/OsmAnd-core-legacy.git"
RESOURCES_REPO="https://github.com/osmandapp/OsmAnd-resources.git"
BUILD_REPO="https://github.com/osmandapp/OsmAnd-build.git"

FLAVOR="androidFullLegacyArm64Debug"
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
[[ -d "$PATCH_DIR" ]]      || err "Patch-Verzeichnis nicht gefunden: $PATCH_DIR"

command -v git    >/dev/null || err "git fehlt"
command -v python3 >/dev/null || err "python3 fehlt"
command -v cmake  >/dev/null || err "cmake fehlt"

[[ -d "$ANDROID_HOME/platforms/android-36" ]] || err "platform-36 fehlt"
[[ -d "$ANDROID_HOME/build-tools/36.0.0" ]]   || err "build-tools 36.0.0 fehlt"

PATCH_COUNT=$(find "$PATCH_DIR" -name "*.patch" -type f | wc -l)
log "Patches gefunden: $PATCH_COUNT"
ok "Voraussetzungen erfüllt"

# ---------------------------------------------------------------------------
# 2. Arbeitsverzeichnis + Repos
# ---------------------------------------------------------------------------
mkdir -p "$WORK"
cd "$WORK"

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

[[ -x "$ANDROID_NDK/ndk-build" ]] || chmod +x "$ANDROID_NDK/ndk-build" 2>/dev/null || true

git -C "$WORK/OsmAnd" config user.email  "libre-moto@jacksitlab.de" 2>/dev/null || true
git -C "$WORK/OsmAnd" config user.name  "LibreMoto Build"          2>/dev/null || true

# ---------------------------------------------------------------------------
# 3. Patches anwenden
# ---------------------------------------------------------------------------
cd "$WORK/OsmAnd"

# Prüfe ob Patches bereits angewendet wurden (anhand eines Marker-Commits)
if git log --oneline | grep -q "libremoto-patches-applied"; then
    log "Patches bereits angewendet — überspringe"
else
    log "Setze OsmAnd auf clean master zurück …"
    git checkout master
    git reset --hard origin/master

    # Alle .patch-Dateien in alphabetischer Reihenfolge anwenden
    for patch_file in $(find "$PATCH_DIR" -name "*.patch" -type f | sort); do
        patch_name=$(basename "$patch_file")
        log "Wende $patch_name an …"
        if git am --abort 2>/dev/null; git apply --check "$patch_file" 2>/dev/null; then
            # Probiere git am (erhält Commit-Message aus dem Patch)
            if git am "$patch_file" 2>/dev/null; then
                log "  angewendet (git am)"
            else
                git am --abort 2>/dev/null || true
                # Fallback: git apply + manueller Commit
                git apply "$patch_file" || err "Patch fehlgeschlagen: $patch_name"
                git add -A
                git commit -m "$patch_name (applied from libre-moto patches)"
                log "  angewendet (git apply + commit)"
            fi
        else
            err "Patch $patch_name kann nicht angewendet werden — Konflikt.\n  Prüfe ob der Patch mit dem aktuellen OsmAnd master kompatibel ist."
        fi
    done

    # Marker-Commit damit wir beim nächsten Lauf erkennen dass Patches drauf sind
    git commit --allow-empty -m "libremoto-patches-applied" 2>/dev/null || true
    ok "Alle Patches angewendet ($PATCH_COUNT)"
fi

# ---------------------------------------------------------------------------
# 3b. versionCode + versionName setzen
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
# 4. Environment + Build
# ---------------------------------------------------------------------------
export JAVA_HOME ANDROID_HOME ANDROID_NDK
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_ROOT="$ANDROID_NDK"
export PATH="$JAVA_HOME/bin:$PATH"

# JVM-Dateinamen-Encoding auf UTF-8 erzwingen. Ohne UTF-8-Locale kann die JVM
# nicht-ASCII-Dateinamen nicht auflösen — z. B.
#   test-resources/search/ludwigstraße.obf.gz
# — und der Build bricht mit "Failed to create MD5 hash … does not exist" ab.
export LC_ALL="C.UTF-8"
export LANG="C.UTF-8"

log "Starte Build: $FLAVOR"
cd "$WORK/OsmAnd"
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
./gradlew "assemble${FLAVOR}" -x test -x :OsmAnd-java:test --console=plain --no-daemon

# ---------------------------------------------------------------------------
# 5. APK finden + kopieren
# ---------------------------------------------------------------------------
mkdir -p "$OUT"
APK=$(find "$WORK/OsmAnd/OsmAnd/build/outputs" -name "*.apk" -path "*androidFull*" -path "*arm64*" 2>/dev/null | head -1)
if [[ -z "$APK" || ! -f "$APK" ]]; then
    APK=$(find "$WORK/OsmAnd/OsmAnd/build/outputs" -name "*.apk" 2>/dev/null | head -1)
fi
[[ -z "$APK" || ! -f "$APK" ]] && err "Keine APK gefunden."

cp "$APK" "$OUT/osmand-fork-arm64-debug.apk"
ok "Fertig: $OUT/osmand-fork-arm64-debug.apk"
log "Paket: net.osmand.plus ($OSMAND_VERSION_NAME)"
log "Installieren: adb install -r $OUT/osmand-fork-arm64-debug.apk"
