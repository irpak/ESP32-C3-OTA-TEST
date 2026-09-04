#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

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

#define CURRENT_VERSION "1.0.2"

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


// ======================================================
// WERSJE
// ======================================================

bool parseVersion(const String& version, int& major, int& minor, int& patch)
{
  major = 0;
  minor = 0;
  patch = 0;

  return sscanf(
    version.c_str(),
    "%d.%d.%d",
    &major,
    &minor,
    &patch
  ) == 3;
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

  WiFiClientSecure client;

  // LABORATORIUM.
  // Później dodamy prawidlowa walidacje TLS.
  client.setInsecure();

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

  Serial.println("Pobieram firmware.bin...");

  WiFiClientSecure updateClient;
  updateClient.setInsecure();

  t_httpUpdate_return result =
    httpUpdate.update(
      updateClient,
      FIRMWARE_URL
    );

  switch (result)
  {
    case HTTP_UPDATE_FAILED:

      Serial.printf(
        "OTA BLAD (%d): %s\n",
        httpUpdate.getLastError(),
        httpUpdate.getLastErrorString().c_str()
      );

      break;


    case HTTP_UPDATE_NO_UPDATES:

      Serial.println("OTA: brak aktualizacji.");
      break;


    case HTTP_UPDATE_OK:

      Serial.println("OTA OK.");
      break;
  }
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
