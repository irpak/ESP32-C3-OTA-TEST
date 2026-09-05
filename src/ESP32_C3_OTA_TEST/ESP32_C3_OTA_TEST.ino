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
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>

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

#define CURRENT_VERSION "1.0.10"

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

// Defer the core's automatic PENDING_VERIFY decision until loop() has
// completed a local, network-independent probation period.
extern "C" bool verifyRollbackLater()
{
  return true;
}

bool otaHealthPending = false;
unsigned long otaHealthStarted = 0;
unsigned long otaHealthHeartbeats = 0;
const unsigned long OTA_HEALTH_PROBATION_MS = 12000;

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
  Preferences marker;
  if (marker.begin("ota", false))
  {
    uint8_t attempt[8]; esp_fill_random(attempt, sizeof(attempt)); char id[17];
    for (int i=0;i<8;i++) sprintf(id+i*2, "%02x", attempt[i]); id[16]=0;
    marker.putString("expected_target", remoteVersion); marker.putBool("boot_expected", true); marker.putString("attempt_id", id); marker.end();
  }
  delay(1000);
  ESP.restart();
}


void initializeOtaHealth()
{
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK)
  {
    Serial.println("OTA HEALTH: cannot read OTA state");
    return;
  }

  if (state == ESP_OTA_IMG_PENDING_VERIFY)
  {
    otaHealthPending = true;
    otaHealthStarted = millis();
    otaHealthHeartbeats = 0;
    Serial.println("OTA HEALTH: state=PENDING_VERIFY");
    Serial.println("OTA HEALTH: probation start");
  }
}

void serviceOtaHealth()
{
  if (!otaHealthPending)
    return;

  ++otaHealthHeartbeats;
  if (millis() - otaHealthStarted < OTA_HEALTH_PROBATION_MS)
    return;

  Serial.println("OTA HEALTH: setup OK");
  Serial.println("OTA HEALTH: loop heartbeat OK");
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK)
  {
    Serial.println("OTA HEALTH: cannot read OTA state");
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY)
  {
    otaHealthPending = false;
    return;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
  {
    otaHealthPending = false;
    Serial.println("OTA HEALTH: PASS");
    Serial.println("OTA HEALTH: firmware marked VALID");
  }
  else
  {
    Serial.println("OTA HEALTH: mark VALID failed");
  }
}

// ======================================================
// SETUP
// ======================================================

// Signed outbound telemetry foundation.  This subsystem is deliberately
// independent from rollback health and never runs from verifyRollbackLater().
const char* TELEMETRY_BROKER = "broker.hivemq.com";
const uint16_t TELEMETRY_PORT = 8883;
const char* TELEMETRY_NAMESPACE = "7a4e5c2d-2d95-4a4f-9b31-0cb9c70a4e1b";
WiFiClientSecure telemetryClient;
Preferences telemetryPreferences;
mbedtls_pk_context telemetryKey;
bool telemetryKeyReady = false;
String telemetryDeviceId;
uint8_t telemetryPublicDer[160]; size_t telemetryPublicDerLen = 0;
uint64_t telemetrySeq = 0;
unsigned long telemetryLastPublish = 0;
unsigned long telemetryNextAttempt = 0; uint32_t telemetryBackoff = 5000;

int telemetryRng(void*, unsigned char* out, size_t len)
{
  esp_fill_random(out, len);
  return 0;
}

String telemetryBase64(const uint8_t* data, size_t len)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  for (size_t i = 0; i < len; i += 3)
  {
    uint32_t value = (uint32_t)data[i] << 16;
    if (i + 1 < len) value |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len) value |= data[i + 2];
    out += alphabet[(value >> 18) & 63]; out += alphabet[(value >> 12) & 63];
    out += (i + 1 < len) ? alphabet[(value >> 6) & 63] : '=';
    out += (i + 2 < len) ? alphabet[value & 63] : '=';
  }
  return out;
}

bool initializeTelemetryIdentity()
{
  if (telemetryKeyReady) return true;
  telemetryPreferences.begin("telemetry", false);
  uint8_t privateDer[256], publicDer[160];
  size_t privateLen = telemetryPreferences.getBytes("priv", privateDer, sizeof(privateDer));
  size_t publicLen = telemetryPreferences.getBytes("pub", publicDer, sizeof(publicDer));
  mbedtls_pk_init(&telemetryKey);
  bool loaded = privateLen && publicLen &&
                mbedtls_pk_parse_key(&telemetryKey, privateDer, privateLen, nullptr, 0, telemetryRng, nullptr) == 0;
  if (!loaded)
  {
    mbedtls_pk_free(&telemetryKey); mbedtls_pk_init(&telemetryKey);
    if (mbedtls_pk_setup(&telemetryKey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(telemetryKey), telemetryRng, nullptr) != 0)
    { telemetryPreferences.end(); return false; }
    privateLen = mbedtls_pk_write_key_der(&telemetryKey, privateDer, sizeof(privateDer));
    publicLen = mbedtls_pk_write_pubkey_der(&telemetryKey, publicDer, sizeof(publicDer));
    if ((int)privateLen <= 0 || (int)publicLen <= 0) { telemetryPreferences.end(); return false; }
    telemetryPreferences.putBytes("priv", privateDer + sizeof(privateDer) - privateLen, privateLen);
    telemetryPreferences.putBytes("pub", publicDer + sizeof(publicDer) - publicLen, publicLen);
  }
  uint8_t digestBytes[32]; mbedtls_sha256(publicDer + sizeof(publicDer) - publicLen, publicLen, digestBytes, 0);
  char hex[65]; for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", digestBytes[i]); hex[64] = 0;
  memcpy(telemetryPublicDer, publicDer + sizeof(publicDer) - publicLen, publicLen); telemetryPublicDerLen = publicLen;
  telemetryDeviceId = String(hex); telemetryKeyReady = true; telemetryPreferences.end();
  return true;
}

bool telemetryPublishStatus()
{
  if (!telemetryKeyReady || WiFi.status() != WL_CONNECTED || (long)(millis()-telemetryNextAttempt)<0) return false;
  if (!telemetryClient.connected())
  {
    telemetryClient.setCACertBundle(x509_crt_bundle, x509_crt_bundle_length);
    if (!telemetryClient.connect(TELEMETRY_BROKER, TELEMETRY_PORT)) { telemetryNextAttempt=millis()+telemetryBackoff; telemetryBackoff=min<uint32_t>(telemetryBackoff*2,60000); return false; }
    uint8_t connectPacket[] = {0x10,19,0,4,'M','Q','T','T',4,2,0,60,0,6,'e','s','p','3','2'};
    telemetryClient.write(connectPacket, sizeof(connectPacket));
    unsigned long wait=millis(); while(telemetryClient.available()<2 && millis()-wait<2000) yield(); if(telemetryClient.available()<2 || telemetryClient.read()!=0x20 || telemetryClient.read()!=0x00) { telemetryClient.stop(); return false; } telemetryBackoff=5000;
  }
  ++telemetrySeq;
  String payload = String("{\"schema_version\":1,\"device_id\":\"") + telemetryDeviceId +
    "\",\"chip_family\":\"ESP32-C3\",\"fw_version\":\"" + CURRENT_VERSION +
    "\",\"seq\":" + String((unsigned long long)telemetrySeq) +
    ",\"uptime_s\":" + String(millis() / 1000) + ",\"wifi_connected\":true,\"wifi_rssi\":" + String(WiFi.RSSI()) +
    ",\"ota_state\":\"stable\",\"ota_result\":\"no_update\",\"health_state\":\"valid\",\"rollback_suspected\":false}";
  uint8_t hash[32], signature[160]; size_t signatureLen = 0;
  mbedtls_sha256((const uint8_t*)payload.c_str(), payload.length(), hash, 0);
  if (mbedtls_pk_sign(&telemetryKey, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, sizeof(signature), &signatureLen, telemetryRng, nullptr) != 0) return false;
  String envelope = String("{\"v\":1,\"alg\":\"ES256\",\"device_id\":\"") + telemetryDeviceId +
    "\",\"pubkey_b64\":\"" + telemetryBase64(telemetryPublicDer, telemetryPublicDerLen) + "\",\"payload_b64\":\"" + telemetryBase64((const uint8_t*)payload.c_str(), payload.length()) +
    "\",\"sig_b64\":\"" + telemetryBase64(signature, signatureLen) + "\"}";
  String topic = String("esp32-ota-lab/v1/") + TELEMETRY_NAMESPACE + "/" + telemetryDeviceId + "/status";
  size_t tlen=topic.length(); size_t rem=2+tlen+envelope.length(); uint8_t header=0x30; telemetryClient.write(&header,1); while(rem){uint8_t b=rem%128;rem/=128;if(rem)b|=128;telemetryClient.write(&b,1);} uint8_t tl[2]={(uint8_t)(tlen>>8),(uint8_t)tlen}; telemetryClient.write(tl,2); telemetryClient.write((const uint8_t*)topic.c_str(),tlen); telemetryClient.write((const uint8_t*)envelope.c_str(),envelope.length()); telemetryNextAttempt=millis()+60000; return true;
}

void serviceTelemetry()
{
  if (otaHealthPending) return;
  if (!telemetryKeyReady && !initializeTelemetryIdentity()) return;
  if (millis() - telemetryLastPublish >= 60000 || telemetryLastPublish == 0)
  { telemetryPublishStatus(); telemetryLastPublish = millis(); }
}

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

  initializeOtaHealth();
}


// ======================================================
// LOOP
// ======================================================

void loop()
{
  serviceOtaHealth();
  serviceTelemetry();

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
