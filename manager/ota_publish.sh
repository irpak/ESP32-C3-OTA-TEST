#!/usr/bin/env bash

export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
set -Eeuo pipefail

REPO="$HOME/Pulpit/ESP32_OTA_MANAGER/ESP32-C3-OTA-TEST"
SKETCH_DIR="$REPO/src/ESP32_C3_OTA_TEST"
SKETCH="$SKETCH_DIR/ESP32_C3_OTA_TEST.ino"
BUILD_DIR="$REPO/manager/build"
OTA_DIR="$REPO/ota"

FQBN="esp32:esp32:esp32c3"

VERSION_URL="https://raw.githubusercontent.com/irpak/ESP32-C3-OTA-TEST/main/ota/version.txt"
FIRMWARE_URL="https://raw.githubusercontent.com/irpak/ESP32-C3-OTA-TEST/main/ota/firmware.bin"

MODE="${1:-publish}"

cd "$REPO"

echo
echo "=============================================="
echo " ESP32 OTA MANAGER"
echo "=============================================="
echo

# -------------------------------------------------------
# Kontrola środowiska
# -------------------------------------------------------

command -v arduino-cli >/dev/null || {
    echo "BLAD: brak arduino-cli"
    exit 1
}

command -v git >/dev/null || {
    echo "BLAD: brak git"
    exit 1
}

command -v gh >/dev/null || {
    echo "BLAD: brak GitHub CLI (gh)"
    exit 1
}

test -f "$SKETCH" || {
    echo "BLAD: brak szkicu:"
    echo "$SKETCH"
    exit 1
}

gh auth status -h github.com >/dev/null 2>&1 || {
    echo "BLAD: GitHub CLI nie jest zalogowany."
    exit 1
}

# -------------------------------------------------------
# Stan GitHub
# -------------------------------------------------------

echo "Pobieram informacje o GitHub..."
git fetch origin main --quiet

BEHIND="$(git rev-list --count HEAD..origin/main)"

if [ "$BEHIND" -gt 0 ]; then
    echo
    echo "STOP."
    echo "Ten komputer ma starsza kopie projektu niz GitHub."
    echo "Najpierw trzeba zsynchronizowac kod."
    exit 1
fi

REMOTE_VERSION="$(
    git show origin/main:ota/version.txt 2>/dev/null |
    tr -d '[:space:]'
)"

LOCAL_VERSION="$(
    sed -n \
      's/^#define CURRENT_VERSION "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' \
      "$SKETCH" |
    head -n 1
)"

if [ -z "$LOCAL_VERSION" ]; then
    echo "BLAD: nie znaleziono CURRENT_VERSION."
    exit 1
fi

echo
echo "Wersja w kodzie:  $LOCAL_VERSION"
echo "Wersja na GitHub: $REMOTE_VERSION"

NEXT_VERSION="$(
python3 - "$LOCAL_VERSION" "$REMOTE_VERSION" <<'PY'
import sys

def ver(s):
    try:
        return tuple(map(int, s.strip().split(".")))
    except Exception:
        return (0, 0, 0)

local = ver(sys.argv[1])
remote = ver(sys.argv[2])

base = max(local, remote)

print(f"{base[0]}.{base[1]}.{base[2] + 1}")
PY
)"

echo "Nastepna wersja:  $NEXT_VERSION"

# -------------------------------------------------------
# TRYB TESTOWY
# -------------------------------------------------------

if [ "$MODE" = "--dry-run" ]; then

    echo
    echo "=============================================="
    echo " TEST KOMPILACJI - BEZ PUBLIKOWANIA"
    echo "=============================================="

    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    arduino-cli compile \
      --fqbn "$FQBN" \
      --build-path "$BUILD_DIR" \
      "$SKETCH_DIR"

    BIN="$BUILD_DIR/ESP32_C3_OTA_TEST.ino.bin"

    test -f "$BIN" || {
        echo "BLAD: kompilacja nie utworzyla firmware."
        exit 1
    }

    echo
    echo "KOMPILACJA OK"
    echo

    ls -lh "$BIN"

    echo
    echo "SHA-256:"
    sha256sum "$BIN"

    echo
    echo "----------------------------------------------"
    echo "TRYB TESTOWY ZAKONCZONY."
    echo "NIC NIE ZOSTALO WYSLANE DO GITHUB."
    echo "ESP32 NIE ZOSTALO ZAPROGRAMOWANE."
    echo "version.txt NIE ZOSTAL ZMIENIONY."
    echo "----------------------------------------------"

    exit 0
fi

# -------------------------------------------------------
# PUBLIKOWANIE OTA
# -------------------------------------------------------

echo
echo "=============================================="
echo " PRZYGOTOWANIE WERSJI $NEXT_VERSION"
echo "=============================================="

# Zmieniamy numer wersji w programie.
sed -i \
  "s/^#define CURRENT_VERSION \".*\"/#define CURRENT_VERSION \"$NEXT_VERSION\"/" \
  "$SKETCH"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo
echo "Kompiluje..."

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD_DIR" \
  "$SKETCH_DIR"

BIN="$BUILD_DIR/ESP32_C3_OTA_TEST.ino.bin"

test -f "$BIN" || {
    echo "BLAD: brak firmware po kompilacji."
    exit 1
}

SIZE="$(stat -c '%s' "$BIN")"

if [ "$SIZE" -lt 100000 ]; then
    echo "BLAD: firmware jest podejrzanie maly."
    exit 1
fi

SHA="$(
    sha256sum "$BIN" |
    awk '{print $1}'
)"

echo
echo "Firmware:"
ls -lh "$BIN"

echo "SHA-256: $SHA"

mkdir -p "$OTA_DIR"

cp -f "$BIN" "$OTA_DIR/firmware.bin"

printf '%s  firmware.bin\n' "$SHA" \
  > "$OTA_DIR/firmware.sha256"

# -------------------------------------------------------
# ETAP 1 - najpierw firmware
# -------------------------------------------------------

echo
echo "ETAP 1/2: wysylam firmware..."

git add \
  "$SKETCH" \
  "$OTA_DIR/firmware.bin" \
  "$OTA_DIR/firmware.sha256"

git commit \
  -m "OTA firmware $NEXT_VERSION"

git push origin main

echo
echo "Sprawdzam firmware pobrane z GitHub..."

TMP_REMOTE="$(mktemp)"

curl -fsSL \
  "${FIRMWARE_URL}?nocache=$(date +%s)" \
  -o "$TMP_REMOTE"

REMOTE_SHA="$(
    sha256sum "$TMP_REMOTE" |
    awk '{print $1}'
)"

rm -f "$TMP_REMOTE"

if [ "$REMOTE_SHA" != "$SHA" ]; then
    echo
    echo "BLAD: firmware na GitHub ma inny SHA-256."
    echo "version.txt NIE zostanie zmieniony."
    exit 1
fi

echo "Firmware na GitHub zweryfikowany."

# -------------------------------------------------------
# ETAP 2 - dopiero teraz uruchamiamy OTA
# -------------------------------------------------------

echo
echo "ETAP 2/2: aktywuje wersje $NEXT_VERSION..."

printf '%s\n' "$NEXT_VERSION" \
  > "$OTA_DIR/version.txt"

git add "$OTA_DIR/version.txt"

git commit \
  -m "Release OTA $NEXT_VERSION"

git push origin main

sleep 2

SERVER_VERSION="$(
    curl -fsSL \
      "${VERSION_URL}?nocache=$(date +%s)" |
    tr -d '[:space:]'
)"

echo
echo "Wersja widoczna z internetu: $SERVER_VERSION"

if [ "$SERVER_VERSION" != "$NEXT_VERSION" ]; then
    echo "OSTRZEZENIE: GitHub jeszcze nie pokazuje nowej wersji."
    exit 1
fi

echo
echo "=============================================="
echo " OTA WYSŁANE PRAWIDLOWO"
echo "=============================================="
echo
echo "Nowa wersja: $NEXT_VERSION"
echo "Firmware SHA-256: $SHA"
echo
echo "ESP32 samo pobierze aktualizacje przez Internet."
echo
