#!/usr/bin/env bash

export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

REPO="$HOME/Pulpit/ESP32_OTA_MANAGER/ESP32-C3-OTA-TEST"
SKETCH="$REPO/src/ESP32_C3_OTA_TEST/ESP32_C3_OTA_TEST.ino"

clear

echo
echo "=================================================="
echo "          ESP32 OTA - KOMPILUJ I WYSLIJ"
echo "=================================================="
echo

cd "$REPO" || {
    echo "BLAD: nie znaleziono repozytorium."
    read -r -p "Enter..."
    exit 1
}

# ----------------------------------------------------------
# Kontrole bezpieczeństwa
# ----------------------------------------------------------

if [ "$(git branch --show-current)" != "main" ]; then
    echo "STOP: nie jestes na galezi main."
    read -r -p "Enter..."
    exit 1
fi

if ! git diff --cached --quiet; then
    echo
    echo "STOP: Git ma juz przygotowane (staged) zmiany."
    echo "OTA nie zostanie uruchomione."
    echo
    git status --short
    echo
    read -r -p "Enter..."
    exit 1
fi

echo "KROK 1/3 - bezpieczny test kompilacji"
echo

./manager/ota_publish.sh --dry-run

RC=$?

if [ "$RC" -ne 0 ]; then
    echo
    echo "STOP: test kompilacji zakonczyl sie bledem."
    echo "NIC NIE ZOSTALO WYSLANE."
    read -r -p "Enter..."
    exit 1
fi

# ----------------------------------------------------------
# Ustalenie wersji, która zostanie wysłana
# ----------------------------------------------------------

git fetch origin main --quiet

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

NEXT_VERSION="$(
python3 - "$LOCAL_VERSION" "$REMOTE_VERSION" <<'PY'
import sys

def version(value):
    try:
        return tuple(map(int, value.strip().split(".")))
    except Exception:
        return (0, 0, 0)

local = version(sys.argv[1])
remote = version(sys.argv[2])

base = max(local, remote)

print(f"{base[0]}.{base[1]}.{base[2] + 1}")
PY
)"

echo
echo "=================================================="
echo "              GOTOWE DO PUBLIKACJI"
echo "=================================================="
echo
echo "Wersja w kodzie:        $LOCAL_VERSION"
echo "Wersja na GitHub:       $REMOTE_VERSION"
echo "Nowa wersja OTA:        $NEXT_VERSION"
echo
echo "Po zatwierdzeniu program:"
echo "  1. skompiluje wersje $NEXT_VERSION"
echo "  2. wysle firmware.bin"
echo "  3. sprawdzi SHA-256 pliku z GitHub"
echo "  4. dopiero potem zmieni version.txt"
echo "  5. ESP32 samo pobierze aktualizacje"
echo
echo "=================================================="
echo

read -r -p "Aby wyslac OTA wpisz dokladnie TAK: " ANSWER

if [ "$ANSWER" != "TAK" ]; then
    echo
    echo "ANULOWANO."
    echo "Nic nie zostalo wyslane."
    echo
    read -r -p "Enter, aby zamknac..."
    exit 0
fi

echo
echo "KROK 2/3 - PUBLIKOWANIE OTA"
echo

./manager/ota_publish.sh

RC=$?

echo
echo "=================================================="

if [ "$RC" -eq 0 ]; then
    echo "             OTA WYSLANE PRAWIDLOWO"
    echo "=================================================="
    echo
    echo "ESP32 powinno samo pobrac nowa wersje."
else
    echo "             BLAD PUBLIKOWANIA OTA"
    echo "=================================================="
    echo
    echo "Sprawdz komunikaty powyzej."
fi

echo
echo "KROK 3/3 - KONIEC"
echo
read -r -p "Nacisnij Enter, aby zamknac okno..."
