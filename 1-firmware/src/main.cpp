#include <Arduino.h>
#include <Wire.h>                // bus I2C (MPU6050)
#include <WiFi.h>                // Wi-Fi
#include <WiFiClientSecure.h>    // connexion chiffrée (TLS)
#include <MQTT.h>                // client MQTT (arduino-mqtt)
#include <ArduinoJson.h>         // messages JSON
#include <time.h>                // heure (NTP)
#include <OneWire.h>             // bus 1 fil (DS18B20)
#include <DallasTemperature.h>   // DS18B20
#include <Adafruit_MPU6050.h>    // MPU6050
#include <Adafruit_Sensor.h>
#include "secrets.h"             // Wi-Fi + compte MQTT (jamais sur GitHub)
#include "ca_cert.h"             // certificat de notre CA (public)

// --- Broches ---
const int PIN_DS18B20 = 4;
const int PIN_SDA     = 21;
const int PIN_SCL     = 22;
const int PIN_RELAIS  = 26;
const int PIN_BUZZER  = 27;
const int PIN_BOUTON  = 25;
const int CANAL_BUZZER = 0;      // canal PWM (LEDC) du buzzer

// --- Seuils ---
const float SEUIL_CRITIQUE_TEMP = 85.0;   // coupure
const float SEUIL_CRITIQUE_VIB  = 1.0;
const float SEUIL_REARM_TEMP    = 75.0;   // réarmement possible en dessous
const float SEUIL_REARM_VIB     = 0.5;

// --- Plages plausibles (sinon la valeur est ignorée) ---
const float TEMP_MIN = 0.0, TEMP_MAX = 120.0;
const float VIB_MAX  = 8.0;

// --- Topics MQTT ---
const char* TOPIC_TEMP        = "usine/ligne1/pompe1/capteurs/temperature";
const char* TOPIC_VIB         = "usine/ligne1/pompe1/capteurs/vibration";
const char* TOPIC_BUZ_CMD     = "usine/ligne1/pompe1/actionneurs/buzzer/commande";
const char* TOPIC_BUZ_ETAT    = "usine/ligne1/pompe1/actionneurs/buzzer/etat";
const char* TOPIC_RELAIS_ETAT = "usine/ligne1/pompe1/actionneurs/relais/etat";
const char* TOPIC_ALERTES     = "usine/ligne1/pompe1/alertes";                 // NOUVEAU (⑥)

// --- Capteurs ---
OneWire busOneWire(PIN_DS18B20);
DallasTemperature capteurTemp(&busOneWire);
Adafruit_MPU6050 mpu;

// --- Réseau ---
WiFiClientSecure reseauTLS;      // le tuyau chiffré
MQTTClient mqtt(512);            // client MQTT, 512 octets pour nos JSON
const unsigned long ATTENTE_MQTT_MS = 5000;   // un essai MQTT toutes les 5 s
unsigned long dernierEssaiMqtt = 0;

// --- Rythmes ---
const unsigned long PERIODE_MS     = 2000;   // mesures toutes les 2 s
const unsigned long ECHANTILLON_MS = 20;     // 1 lecture de vibration toutes les 20 ms
unsigned long derniereMesure     = 0;
unsigned long dernierEchantillon = 0;

// --- RMS ---
float sommeCarres    = 0;
int   nbEchantillons = 0;
const float G = 9.80665;

// --- Dernières valeurs ---
float derniereTemp = 0;
float derniereVib  = 0;

// --- États ---
bool moteurEnMarche = false;
bool buzzerActif    = false;
bool defautCritique = false;
bool wifiConnecte   = false;
bool mqttConnecte   = false;

// --- États à publier et commande reçue ---
String causeRelais      = "demarrage";   // pourquoi le relais est dans cet état
bool   relaisAPublier   = false;         // l'état du relais a changé → à envoyer
bool   buzzerAPublier   = false;         // l'état du buzzer a changé → à envoyer
String commandeRecue    = "";            // "ON" / "OFF" reçu, appliqué dans loop()
String sourceCommande   = "";            // "auto" / "manuel"

// --- NOUVEAU (⑥) : alertes « coupure locale » en attente d'envoi ---
// Une alerte par grandeur : si les deux sont critiques, deux messages partent.
struct AlerteCoupure {
  bool   aPublier = false;
  float  valeur   = 0;
  String ts       = "";                  // heure de la coupure (pas de l'envoi)
};
AlerteCoupure alerteTemp;
AlerteCoupure alerteVib;

// --- Bouton ---
int  etatBoutonPrecedent = HIGH;
unsigned long dernierChangementBouton = 0;

// ===== Actionneurs (chaque changement est noté « à publier ») =====
void moteurMarche(const char* cause) {
  digitalWrite(PIN_RELAIS, HIGH);
  moteurEnMarche = true;  causeRelais = cause;  relaisAPublier = true;
}
void moteurCoupe(const char* cause) {
  digitalWrite(PIN_RELAIS, LOW);
  moteurEnMarche = false; causeRelais = cause;  relaisAPublier = true;
}
void buzzerOn() {
  ledcWriteTone(CANAL_BUZZER, 2000);
  if (!buzzerActif) buzzerAPublier = true;
  buzzerActif = true;
}
void buzzerOff() {
  ledcWriteTone(CANAL_BUZZER, 0);
  if (buzzerActif) buzzerAPublier = true;
  buzzerActif = false;
}

// ===== Une seule règle « mesure bonne ou fausse » =====
bool temperatureValide(float t) { return !isnan(t) && t >= TEMP_MIN && t <= TEMP_MAX; }
bool vibrationValide(float v)   { return !isnan(v) && v >= 0 && v <= VIB_MAX; }

// ===== L'heure =====
// Donne l'heure au format "2026-10-07T10:00:00Z" (vide si pas encore reçue)
String horodatage() {
  time_t maintenant = time(nullptr);
  if (maintenant < 1700000000) return "";   // l'ESP32 croit être en 1970 → pas d'heure
  struct tm t;
  gmtime_r(&maintenant, &t);                 // heure UTC
  char texte[25];
  strftime(texte, sizeof(texte), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(texte);
}

// ===== Réception d'un message =====
// On NE publie PAS ici (règle d'arduino-mqtt) : on note la commande, appliquée dans loop().
void recevoirMessage(String &topic, String &texte) {
  if (topic != TOPIC_BUZ_CMD) return;

  JsonDocument doc;
  if (deserializeJson(doc, texte)) {                       // JSON illisible
    Serial.print("Commande ignoree : JSON invalide\r\n");
    return;
  }
  String cmd = doc["cmd"] | "";
  if (cmd != "ON" && cmd != "OFF") {                       // commande inconnue
    Serial.print("Commande ignoree : cmd doit etre ON ou OFF\r\n");
    return;
  }
  commandeRecue  = cmd;
  sourceCommande = doc["source"] | "inconnue";
}

// ===== Le Wi-Fi, sans jamais bloquer =====
void surveillerWifi() {
  bool connecte = (WiFi.status() == WL_CONNECTED);

  if (connecte && !wifiConnecte) {           // il vient d'arriver
    Serial.print("Wi-Fi connecte, IP = ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org");        // on demande l'heure à Internet
  }
  if (!connecte && wifiConnecte) {           // il vient de partir
    Serial.println("Wi-Fi perdu : la surveillance locale continue\r");
  }
  wifiConnecte = connecte;
}

// ===== MQTT chiffré, sans bloquer la boucle =====
void surveillerMqtt() {
  if (!wifiConnecte) { mqttConnecte = false; return; }   // pas de Wi-Fi = pas de MQTT

  mqtt.loop();                               // garde la connexion vivante + reçoit les messages
  mqttConnecte = mqtt.connected();
  if (mqttConnecte) return;                  // déjà connecté : rien à faire

  if (millis() - dernierEssaiMqtt < ATTENTE_MQTT_MS) return;   // on attend 5 s
  dernierEssaiMqtt = millis();

  Serial.print("Connexion MQTT (TLS 8883)...\r\n");
  if (mqtt.connect("esp32-pompe1", MQTT_USER, MQTT_PASS)) {
    mqttConnecte = true;
    Serial.print("MQTT connecte au broker\r\n");
    mqtt.subscribe(TOPIC_BUZ_CMD, 1);        // écouter les ordres (QoS 1)
    relaisAPublier = true;                   // renvoyer les états actuels
    buzzerAPublier = true;
  } else {
    Serial.printf("MQTT echec : erreur %d, code broker %d\r\n",
                  mqtt.lastError(), mqtt.returnCode());
  }
}

// ===== Publier une mesure en JSON =====
// {"valeur":22.0,"unite":"°C","qualite":"ok","ts":"2026-10-07T14:10:02Z"}
void publierMesure(const char* topic, float valeur, int decimales,
                   const char* unite, bool valide, const String& ts) {
  JsonDocument doc;
  if (isnan(valeur)) doc["valeur"] = nullptr;                  // pas de NaN dans un JSON
  else               doc["valeur"] = serialized(String(valeur, decimales));
  doc["unite"]   = unite;
  doc["qualite"] = valide ? "ok" : "erreur";
  doc["ts"]      = ts;

  char texte[160];
  serializeJson(doc, texte, sizeof(texte));
  mqtt.publish(topic, texte, false, 0);      // non retenu, QoS 0
}

// ===== Publier les états des actionneurs (QoS 1, retenus) =====
void publierEtats() {
  String ts = horodatage();
  if (!mqttConnecte || ts.length() == 0) return;   // on réessaiera au prochain tour

  if (relaisAPublier) {
    JsonDocument doc;
    doc["etat"]  = moteurEnMarche ? "marche" : "coupe";
    doc["cause"] = causeRelais;
    doc["ts"]    = ts;
    char texte[160];
    serializeJson(doc, texte, sizeof(texte));
    if (mqtt.publish(TOPIC_RELAIS_ETAT, texte, true, 1)) relaisAPublier = false;
  }
  if (buzzerAPublier) {
    JsonDocument doc;
    doc["etat"] = buzzerActif ? "ON" : "OFF";
    doc["ts"]   = ts;
    char texte[100];
    serializeJson(doc, texte, sizeof(texte));
    if (mqtt.publish(TOPIC_BUZ_ETAT, texte, true, 1)) buzzerAPublier = false;
  }
}

// ===== NOUVEAU (⑥) : publier une alerte « coupure locale » =====
// {"type":"coupure_locale","grandeur":"temperature","valeur":95.7,"seuil":85.0,"ts":"…"}
void publierUneAlerte(AlerteCoupure &alerte, const char* grandeur,
                      float seuil, int decimales) {
  if (!alerte.aPublier) return;
  String ts = alerte.ts.length() ? alerte.ts : horodatage();   // heure de la coupure si connue
  if (ts.length() == 0) return;                                // pas d'heure : on attend

  JsonDocument doc;
  doc["type"]     = "coupure_locale";
  doc["grandeur"] = grandeur;
  doc["valeur"]   = serialized(String(alerte.valeur, decimales));
  doc["seuil"]    = serialized(String(seuil, decimales));
  doc["ts"]       = ts;
  char texte[200];
  serializeJson(doc, texte, sizeof(texte));
  if (mqtt.publish(TOPIC_ALERTES, texte, false, 1)) {          // QoS 1, NON retenu
    alerte.aPublier = false;
    Serial.printf("Alerte envoyee : coupure locale (%s)\r\n", grandeur);
  }
}

void publierAlertes() {
  if (!mqttConnecte) return;                 // pas de connexion : l'alerte attend
  publierUneAlerte(alerteTemp, "temperature", SEUIL_CRITIQUE_TEMP, 1);
  publierUneAlerte(alerteVib,  "vibration",   SEUIL_CRITIQUE_VIB,  2);
}

// ===== Appliquer la commande reçue =====
void appliquerCommande() {
  if (commandeRecue.length() == 0) return;
  if (commandeRecue == "ON") buzzerOn(); else buzzerOff();
  Serial.printf(">>> COMMANDE BUZZER : %s (source : %s)\r\n",
                commandeRecue.c_str(), sourceCommande.c_str());
  commandeRecue = "";
  buzzerAPublier = true;                     // on confirme l'état, même s'il n'a pas changé
}

// ===== Coupure au seuil critique =====
void verifierSeuilCritique(float temp, float vib) {
  bool tempCritique = temperatureValide(temp) && temp > SEUIL_CRITIQUE_TEMP;
  bool vibCritique  = vibrationValide(vib)    && vib  > SEUIL_CRITIQUE_VIB;
  if ((tempCritique || vibCritique) && !defautCritique) {
    defautCritique = true;
    if (tempCritique && vibCritique) moteurCoupe("temperature et vibration critiques");
    else if (tempCritique)           moteurCoupe("temperature critique");
    else                             moteurCoupe("vibration critique");
    Serial.printf(">>> COUPURE LOCALE : temp = %.1f C, vib = %.2f g\r\n", temp, vib);

    // NOUVEAU (⑥) : on prépare l'alerte (envoyée dès que MQTT le permet)
    String ts = horodatage();
    if (tempCritique) { alerteTemp.aPublier = true; alerteTemp.valeur = temp; alerteTemp.ts = ts; }
    if (vibCritique)  { alerteVib.aPublier  = true; alerteVib.valeur  = vib;  alerteVib.ts  = ts; }
  }
}

// ===== Bouton « Réarmement » =====
void gererBoutonRearmement() {
  int etat = digitalRead(PIN_BOUTON);
  if (etat != etatBoutonPrecedent && millis() - dernierChangementBouton > 50) {
    dernierChangementBouton = millis();
    etatBoutonPrecedent = etat;
    if (etat == LOW) {
      if (!defautCritique) {
        Serial.println("Bouton : aucun defaut a rearmer\r");
      } else if (derniereTemp < SEUIL_REARM_TEMP && derniereVib < SEUIL_REARM_VIB) {
        defautCritique = false;
        moteurMarche("rearmement manuel");
        Serial.println(">>> REARMEMENT ACCEPTE : moteur redemarre\r");
      } else {
        Serial.println(">>> REARMEMENT REFUSE : valeurs encore trop elevees\r");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAIS, OUTPUT);
  pinMode(PIN_BOUTON, INPUT_PULLUP);
  moteurCoupe("demarrage");                  // position sûre au démarrage
  ledcSetup(CANAL_BUZZER, 2000, 8);          // prépare le PWM AVANT tout usage
  ledcAttachPin(PIN_BUZZER, CANAL_BUZZER);
  buzzerOff();

  capteurTemp.begin();
  capteurTemp.setWaitForConversion(false);
  capteurTemp.requestTemperatures();

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!mpu.begin()) {
    Serial.println("ERREUR : MPU6050 introuvable\r");
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);

  buzzerOn(); delay(300); buzzerOff();       // bip de test
  moteurMarche("demarrage");

  // Wi-Fi : on le LANCE, mais on n'attend pas
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);       // canal 6 = plus rapide dans Wokwi

  // Préparation de MQTT (la connexion se fera dans loop)
  reseauTLS.setCACert(CA_CERT);              // on ne fait confiance qu'à notre CA
  mqtt.begin(MQTT_HOST, MQTT_PORT, reseauTLS);   // host.wokwi.internal : 8883
  mqtt.onMessage(recevoirMessage);           // qui traite les messages reçus

  Serial.println("Demarrage : surveillance active, connexion Wi-Fi en cours...\r");
}

void loop() {
  // ① Vibration : une lecture toutes les 20 ms
  if (millis() - dernierEchantillon >= ECHANTILLON_MS) {
    dernierEchantillon = millis();
    sensors_event_t accel, gyro, tempMpu;
    mpu.getEvent(&accel, &gyro, &tempMpu);
    float x = accel.acceleration.x / G;
    float y = accel.acceleration.y / G;
    float z = accel.acceleration.z / G;
    sommeCarres += x * x + y * y + (z - 1) * (z - 1);
    nbEchantillons++;
  }

  // ② Bouton, Wi-Fi, MQTT, commandes, états et alertes : à chaque tour
  gererBoutonRearmement();
  surveillerWifi();
  surveillerMqtt();
  appliquerCommande();
  publierEtats();
  publierAlertes();                          // NOUVEAU (⑥)

  // ③ Toutes les 2 s : mesures, décision, publication, affichage
  if (millis() - derniereMesure >= PERIODE_MS) {
    derniereMesure = millis();

    derniereTemp = capteurTemp.getTempCByIndex(0);
    capteurTemp.requestTemperatures();
    derniereVib = (nbEchantillons > 0) ? sqrt(sommeCarres / nbEchantillons) : 0;
    sommeCarres = 0;
    nbEchantillons = 0;

    verifierSeuilCritique(derniereTemp, derniereVib);   // la sécurité d'abord

    String ts = horodatage();

    bool publie = false;
    if (mqttConnecte && ts.length() > 0) {
      publierMesure(TOPIC_TEMP, derniereTemp, 1, "°C",
                    temperatureValide(derniereTemp), ts);
      publierMesure(TOPIC_VIB, derniereVib, 2, "g",
                    vibrationValide(derniereVib), ts);
      publie = true;
    }

    Serial.printf("[%s] Temp : %.1f C | Vib : %.2f g | Moteur : %s | Buzzer : %s | Wi-Fi : %s | MQTT : %s%s\r\n",
                  ts.length() ? ts.c_str() : "heure inconnue",
                  derniereTemp, derniereVib,
                  moteurEnMarche ? "MARCHE" : "COUPE",
                  buzzerActif ? "ON" : "OFF",
                  wifiConnecte ? "OK" : "...",
                  mqttConnecte ? "OK" : "...",
                  publie ? " | publie" : "");
  }
}