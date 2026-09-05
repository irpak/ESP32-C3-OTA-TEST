#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "public_key.h"
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <mbedtls/sha256.h>
#include <limits.h>

// ======================================================
// ESP32-C3 OTA TEST
// V1.0.2
//
// Wi-Fi:
// - SSID i haslo przechowywane w NVS
// - konfiguracja przez 192.168.4.1
//
// OTA:
// - aktualizacja tylko do wersji NOWSZEJ
// ======================================================

#define CURRENT_VERSION "1.0.8"

const char* VERSION_URL =
  "https://raw.githubusercontent.com/irpak/ESP32-C3-OTA-TEST/main/ota/version.txt";

const char* FIRMWARE_URL =
  "https://raw.githubusercontent.com/irpak/ESP32-C3-OTA-TEST/main/ota/firmware.bin";

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

String savedSSID;
String savedPassword;
String setupAPName;

bool configMode = false;

unsigned long lastCheck = 0;
const unsigned long CHECK_INTERVAL = 60000;

// The Arduino ESP32 3.3.11 build embeds the full Espressif CA bundle.
// These symbols are exported by the local esp32c3 mbedTLS component.
extern const uint8_t x509_crt_bundle[];
extern const size_t x509_crt_bundle_length;


// ======================================================
// WERSJE
// ======================================================

bool parseVersion(const String& version, int& major, int& minor, int& patch)
{
  major = 0;
  minor = 0;
  patch = 0;

  String value = version;
  value.trim();
  int first = value.indexOf('.');
  int second = first < 0 ? -1 : value.indexOf('.', first + 1);

  if (first <= 0 || second <= first + 1 || value.indexOf('.', second + 1) >= 0)
    return false;

  String parts[3] = {
    value.substring(0, first),
    value.substring(first + 1, second),
    value.substring(second + 1)
  };

  int* numbers[3] = {&major, &minor, &patch};
  for (int i = 0; i < 3; ++i)
  {
    if (parts[i].length() == 0 ||
        (parts[i].length() > 1 && parts[i][0] == '0'))
      return false;

    int parsed = 0;
    for (size_t j = 0; j < parts[i].length(); ++j)
    {
      if (parts[i][j] < '0' || parts[i][j] > '9')
        return false;

      int digit = parts[i][j] - '0';
      if (parsed > (INT_MAX - digit) / 10)
        return false;

      parsed = parsed * 10 + digit;
    }

    *numbers[i] = parsed;
  }

  return true;
}


bool isRemoteVersionNewer(String remoteVersion, String localVersion)
{
  remoteVersion.trim();
  localVersion.trim();

  int rMajor, rMinor, rPatch;
  int lMajor, lMinor, lPatch;

  if (!parseVersion(remoteVersion, rMajor, rMinor, rPatch))
  {
    Serial.println("Nieprawidlowy numer wersji na serwerze.");
    return false;
  }

  if (!parseVersion(localVersion, lMajor, lMinor, lPatch))
  {
    Serial.println("Nieprawidlowy lokalny numer wersji.");
    return false;
  }

  if (rMajor != lMajor)
    return rMajor > lMajor;

  if (rMinor != lMinor)
    return rMinor > lMinor;

  return rPatch > lPatch;
}


// ======================================================
// NVS - DANE WI-FI
// ======================================================

void loadWiFiCredentials()
{
  preferences.begin("wifi", true);

  savedSSID = preferences.getString("ssid", "");
  savedPassword = preferences.getString("pass", "");

  preferences.end();

  if (savedSSID.length() > 0)
  {
    Serial.print("Zapisana siec Wi-Fi: ");
    Serial.println(savedSSID);
  }
  else
  {
    Serial.println("Brak zapisanej sieci Wi-Fi.");
  }
}


bool saveWiFiCredentials(const String& ssid, const String& password)
{
  if (ssid.length() == 0)
    return false;

  preferences.begin("wifi", false);

  size_t a = preferences.putString("ssid", ssid);
  preferences.putString("pass", password);

  preferences.end();

  return a > 0;
}


void clearWiFiCredentials()
{
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
}


// ======================================================
// POLACZENIE WI-FI
// ======================================================

bool connectSavedWiFi(unsigned long timeoutMs)
{
  if (savedSSID.length() == 0)
    return false;

  Serial.println();
  Serial.print("Laczenie z Wi-Fi: ");
  Serial.println(savedSSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.begin(
    savedSSID.c_str(),
    savedPassword.c_str()
  );

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");

    if (millis() - start >= timeoutMs)
    {
      Serial.println();
      Serial.println("Nie udalo sie polaczyc.");
      return false;
    }
  }

  Serial.println();
  Serial.println("Wi-Fi POLACZONE");

  Serial.print("Adres IP z DHCP: ");
  Serial.println(WiFi.localIP());

  return true;
}


// ======================================================
// PORTAL KONFIGURACYJNY
// ======================================================

String makeSetupAPName()
{
  uint64_t chipId = ESP.getEfuseMac();

  char suffix[7];

  snprintf(
    suffix,
    sizeof(suffix),
    "%06llX",
    (unsigned long long)(chipId & 0xFFFFFF)
  );

  return String("ESP32-OTA-SETUP-") + suffix;
}


String configurationPage()
{
  String html;

  html +=
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 OTA Wi-Fi</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:520px;margin:35px auto;padding:20px;}"
    "input{width:100%;padding:12px;margin:7px 0 16px;box-sizing:border-box;}"
    "button{width:100%;padding:14px;font-size:16px;}"
    ".info{background:#eee;padding:12px;margin-bottom:20px;}"
    "</style>"
    "</head>"
    "<body>";

  html += "<h2>ESP32-C3 - konfiguracja Wi-Fi</h2>";

  html += "<div class='info'>";
  html += "Wersja programu: ";
  html += CURRENT_VERSION;
  html += "<br>Punkt dostepowy: ";
  html += setupAPName;
  html += "<br>Adres konfiguracji: 192.168.4.1";
  html += "</div>";

  html +=
    "<form method='POST' action='/save'>"
    "<label>Nazwa sieci Wi-Fi (SSID)</label>"
    "<input name='ssid' required>"
    "<label>Haslo Wi-Fi</label>"
    "<input name='password' type='password'>"
    "<button type='submit'>ZAPISZ I POLACZ</button>"
    "</form>";

  if (savedSSID.length() > 0)
  {
    html +=
      "<hr>"
      "<form method='POST' action='/clear'>"
      "<button type='submit'>USUN ZAPISANA SIEC</button>"
      "</form>";
  }

  html +=
    "</body>"
    "</html>";

  return html;
}


void startConfigurationPortal()
{
  configMode = true;

  setupAPName = makeSetupAPName();

  Serial.println();
  Serial.println("====================================");
  Serial.println(" TRYB KONFIGURACJI WI-FI");
  Serial.println("====================================");

  WiFi.mode(WIFI_AP_STA);

  // Wersja laboratoryjna:
  // punkt AP jest otwarty.
  WiFi.softAP(setupAPName.c_str());

  IPAddress apIP = WiFi.softAPIP();

  Serial.print("Polacz telefon z siecia: ");
  Serial.println(setupAPName);

  Serial.print("Otworz: http://");
  Serial.println(apIP);

  dnsServer.start(53, "*", apIP);


  server.on("/", HTTP_GET, []()
  {
    server.send(
      200,
      "text/html; charset=utf-8",
      configurationPage()
    );
  });


  server.on("/save", HTTP_POST, []()
  {
    String ssid = server.arg("ssid");
    String password = server.arg("password");

    ssid.trim();

    if (ssid.length() == 0)
    {
      server.send(
        400,
        "text/plain; charset=utf-8",
        "Brak SSID."
      );

      return;
    }

    if (!saveWiFiCredentials(ssid, password))
    {
      server.send(
        500,
        "text/plain; charset=utf-8",
        "Nie udalo sie zapisac konfiguracji."
      );

      return;
    }

    server.send(
      200,
      "text/html; charset=utf-8",
      "<h2>Zapisano Wi-Fi.</h2>"
      "<p>ESP32 uruchomi sie ponownie.</p>"
    );

    delay(1500);

    ESP.restart();
  });


  server.on("/clear", HTTP_POST, []()
  {
    clearWiFiCredentials();

    server.send(
      200,
      "text/html; charset=utf-8",
      "<h2>Usunieto konfiguracje Wi-Fi.</h2>"
      "<p>ESP32 uruchomi sie ponownie.</p>"
    );

    delay(1500);

    ESP.restart();
  });


  server.onNotFound([]()
  {
    server.send(
      200,
      "text/html; charset=utf-8",
      configurationPage()
    );
  });


  server.begin();

  // Jezeli istnieja stare dane Wi-Fi,
  // ESP32 nadal probuje laczyc sie w tle.
  if (savedSSID.length() > 0)
  {
    WiFi.begin(
      savedSSID.c_str(),
      savedPassword.c_str()
    );
  }
}


// ======================================================
// OTA
// ======================================================

bool synchronizeClock()
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  unsigned long started = millis();

  while (now < 8 * 3600 * 2 && millis() - started < 30000)
  {
    delay(250);
    now = time(nullptr);
  }

  if (now < 8 * 3600 * 2)
  {
    Serial.println("TLS: brak poprawnego czasu NTP.");
    return false;
  }

  return true;
}

void configureSecureClient(WiFiClientSecure& client)
{
  client.setCACertBundle(x509_crt_bundle, x509_crt_bundle_length);
  client.setTimeout(12000);
  client.setHandshakeTimeout(15);
}

bool isSha256Hex(const String& value)
{
  if (value.length() != 64)
    return false;

  for (size_t i = 0; i < value.length(); ++i)
  {
    char c = value[i];
    bool digit = c >= '0' && c <= '9';
    bool lower = c >= 'a' && c <= 'f';
    bool upper = c >= 'A' && c <= 'F';

    if (!digit && !lower && !upper)
      return false;
  }

  return true;
}

String readExpectedFirmwareSha256()
{
  const char* shaURL =
    "https://raw.githubusercontent.com/irpak/ESP32-C3-OTA-TEST/main/ota/firmware.sha256";

  WiFiClientSecure client;
  configureSecureClient(client);

  HTTPClient https;
  if (!https.begin(client, shaURL))
  {
    Serial.println("SHA: nie mozna otworzyc firmware.sha256.");
    return String();
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.print("SHA: blad HTTP: ");
    Serial.println(httpCode);
    https.end();
    return String();
  }

  String document = https.getString();
  https.end();

  if (document.endsWith("\r\n"))
    document.remove(document.length() - 2);
  else if (document.endsWith("\n"))
    document.remove(document.length() - 1);

  if (document.length() != 78 ||
      document.substring(64, 66) != "  " ||
      document.substring(66) != "firmware.bin")
  {
    Serial.println("SHA: nieprawidlowy format manifestu.");
    return String();
  }

  String expected = document.substring(0, 64);

  if (!isSha256Hex(expected))
  {
    Serial.println("SHA: nieprawidlowy format SHA-256.");
    return String();
  }

  expected.toLowerCase();
  return expected;
}

bool downloadAndVerifyFirmware(const String& expectedSha)
{
  WiFiClientSecure client;
  configureSecureClient(client);

  HTTPClient https;
  if (!https.begin(client, FIRMWARE_URL))
  {
    Serial.println("OTA: nie mozna otworzyc firmware.bin.");
    return false;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.print("OTA: blad HTTP firmware.bin: ");
    Serial.println(httpCode);
    https.end();
    return false;
  }

  int contentLength = https.getSize();
  if (contentLength <= 0)
  {
    Serial.println("OTA: nieprawidlowy rozmiar firmware.");
    https.end();
    return false;
  }

  UpdaterECDSAVerifier signatureVerifier(PUBLIC_KEY, PUBLIC_KEY_LEN, HASH_SHA256);
  if (!Update.installSignature(&signatureVerifier))
  {
    Serial.println("OTA: nie mozna zainstalowac weryfikacji podpisu.");
    https.end();
    return false;
  }

  if (!Update.begin((size_t)contentLength, U_FLASH))
  {
    Serial.print("OTA: Update.begin: ");
    Serial.println(Update.errorString());
    https.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);

  bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
  size_t written = 0;
  uint8_t buffer[1024];
  WiFiClient* stream = https.getStreamPtr();

  while (ok && written < (size_t)contentLength)
  {
    size_t available = stream->available();

    if (available > 0)
    {
      size_t want = min(available, sizeof(buffer));
      int received = stream->readBytes(buffer, want);

      if (received <= 0 || Update.write(buffer, received) != (size_t)received)
      {
        ok = false;
        break;
      }

      if (mbedtls_sha256_update(&sha, buffer, received) != 0)
      {
        ok = false;
        break;
      }

      written += (size_t)received;
    }
    else if (!https.connected())
    {
      ok = false;
      break;
    }
    else
    {
      delay(1);
    }
  }

  unsigned char calculated[32];
  if (ok && written == (size_t)contentLength)
    ok = mbedtls_sha256_finish(&sha, calculated) == 0;
  else
    mbedtls_sha256_finish(&sha, calculated);

  mbedtls_sha256_free(&sha);
  https.end();

  if (!ok || written != (size_t)contentLength)
  {
    Update.abort();
    Serial.println("OTA: niepelne pobranie albo blad zapisu.");
    return false;
  }

  char calculatedText[65];
  for (size_t i = 0; i < 32; ++i)
    snprintf(calculatedText + (i * 2), 3, "%02x", calculated[i]);
  calculatedText[64] = '\0';

  if (expectedSha != String(calculatedText))
  {
    Update.abort();
    Serial.println("OTA: SHA-256 firmware niezgodne — obraz odrzucony.");
    return false;
  }

  // Partition is activated only after complete download and SHA match.
  if (!Update.end(false) || !Update.isFinished())
  {
    Serial.print("OTA: Update.end: ");
    Serial.println(Update.errorString());
    return false;
  }

  return true;
}

void checkForUpdate()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("OTA: brak polaczenia Wi-Fi.");
    return;
  }

  Serial.println();
  Serial.println("------------------------------------");
  Serial.println("Sprawdzam aktualizacje OTA...");

  Serial.print("Aktualna wersja: ");
  Serial.println(CURRENT_VERSION);

  if (!synchronizeClock())
    return;

  WiFiClientSecure client;
  configureSecureClient(client);

  HTTPClient https;

  if (!https.begin(client, VERSION_URL))
  {
    Serial.println("Nie mozna otworzyc VERSION_URL.");
    return;
  }

  int httpCode = https.GET();

  if (httpCode != HTTP_CODE_OK)
  {
    Serial.print("Blad HTTP version.txt: ");
    Serial.println(httpCode);

    https.end();
    return;
  }

  String remoteVersion = https.getString();

  https.end();

  remoteVersion.trim();

  Serial.print("Wersja na serwerze: ");
  Serial.println(remoteVersion);

  if (!isRemoteVersionNewer(
        remoteVersion,
        String(CURRENT_VERSION)))
  {
    Serial.println("Brak nowszej wersji.");
    return;
  }

  Serial.println();
  Serial.println("NOWA WERSJA ZNALEZIONA");

  Serial.print("Aktualizacja ");
  Serial.print(CURRENT_VERSION);
  Serial.print(" -> ");
  Serial.println(remoteVersion);

  Serial.println("Pobieram firmware.sha256...");
  String expectedSha = readExpectedFirmwareSha256();
  if (expectedSha.length() != 64)
    return;

  Serial.println("Pobieram firmware.bin...");
  if (!downloadAndVerifyFirmware(expectedSha))
    return;

  Serial.println("OTA OK — SHA-256 zweryfikowane.");
  delay(1000);
  ESP.restart();
}


// ======================================================
// SETUP
// ======================================================

void setup()
{
  Serial.begin(115200);

  delay(1500);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" ESP32-C3 OTA MANAGER");
  Serial.print(" VERSION ");
  Serial.println(CURRENT_VERSION);
  Serial.println("====================================");

  loadWiFiCredentials();

  if (connectSavedWiFi(20000))
  {
    checkForUpdate();
    lastCheck = millis();
  }
  else
  {
    startConfigurationPortal();
  }
}


// ======================================================
// LOOP
// ======================================================

void loop()
{
  if (configMode)
  {
    dnsServer.processNextRequest();
    server.handleClient();

    // Jeżeli wcześniej zapisane Wi-Fi znów stanie się
    // dostępne, ESP32 może wrócić do normalnej pracy.
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println();
      Serial.println("Wi-Fi odzyskane.");

      dnsServer.stop();
      server.stop();

      WiFi.softAPdisconnect(true);

      configMode = false;

      Serial.print("Adres IP z DHCP: ");
      Serial.println(WiFi.localIP());

      checkForUpdate();

      lastCheck = millis();
    }

    delay(5);
    return;
  }


  if (millis() - lastCheck >= CHECK_INTERVAL)
  {
    lastCheck = millis();

    checkForUpdate();
  }

  delay(50);
}
