#!/usr/bin/env bash

export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

REPO="$HOME/Pulpit/ESP32_OTA_MANAGER/ESP32-C3-OTA-TEST"

clear

echo
echo "=============================================="
echo "       ESP32 OTA MANAGER - TEST"
echo "=============================================="
echo
echo "Ten przycisk:"
echo "  - skompiluje program,"
echo "  - sprawdzi firmware,"
echo "  - NIE wysle niczego do GitHub,"
echo "  - NIE zaprogramuje ESP32."
echo
echo "=============================================="
echo

cd "$REPO" || {
    echo "BLAD: nie znaleziono repozytorium."
    read -r -p "Nacisnij Enter..."
    exit 1
}

./manager/ota_publish.sh --dry-run

RC=$?

echo
echo "=============================================="

if [ "$RC" -eq 0 ]; then
    echo " TEST OTA MANAGER ZAKONCZONY PRAWIDLOWO"
else
    echo " TEST OTA MANAGER ZAKONCZONY BŁĘDEM"
fi

echo "=============================================="
echo
read -r -p "Nacisnij Enter, aby zamknac okno..."
