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

// --- NOUVEAU (09) : règles de validation ---
const float   TEMP_PIEGE     = 85.0;   // valeur renvoyée par un DS18B20 après une erreur d'alimentation
const float   SAUT_MAX_TEMP  = 10.0;   // un roulement ne chauffe pas de plus de 10 °C en 2 s
const float   NORME_MIN_G    = 0.2;    // un MPU vivant ressent toujours ~1 g (gravité)
const uint8_t ADRESSE_MPU    = 0x68;   // adresse I2C du MPU6050

// --- Topics MQTT ---
const char* TOPIC_TEMP        = "usine/ligne1/pompe1/capteurs/temperature";
const char* TOPIC_VIB         = "usine/ligne1/pompe1/capteurs/vibration";
const char* TOPIC_BUZ_CMD     = "usine/ligne1/pompe1/actionneurs/buzzer/commande";
const char* TOPIC_BUZ_ETAT    = "usine/ligne1/pompe1/actionneurs/buzzer/etat";
const char* TOPIC_RELAIS_ETAT = "usine/ligne1/pompe1/actionneurs/relais/etat";
const char* TOPIC_ALERTES     = "usine/ligne1/pompe1/alertes";
const char* TOPIC_ETAT        = "usine/ligne1/pompe1/etat";

// --- État de la carte (sans heure : préparés à l'avance) ---
const char* ETAT_EN_LIGNE   = "{\"etat\":\"en_ligne\"}";
const char* ETAT_HORS_LIGNE = "{\"etat\":\"hors_ligne\"}";
const int   KEEPALIVE_S     = 30;   // carte déclarée perdue après 1,5 × 30 = 45 s

// --- Capteurs ---
OneWire busOneWire(PIN_DS18B20);
DallasTemperature capteurTemp(&busOneWire);
Adafruit_MPU6050 mpu;

// --- Réseau (utilisé UNIQUEMENT par la tâche réseau, cœur 0) ---
WiFiClientSecure reseauTLS;
MQTTClient mqtt(512);
const unsigned long ATTENTE_MQTT_MS = 5000;   // un essai MQTT toutes les 5 s
unsigned long dernierEssaiMqtt = 0;
volatile bool wifiConnecte = false;           // écrits par le cœur 0, lus pour l'affichage
volatile bool mqttConnecte = false;

// ===================================================================
// La « boîte » partagée entre les deux cœurs + son verrou
// ===================================================================
SemaphoreHandle_t verrou;                     // le « bâton de parole » (mutex)
void verrouiller()   { xSemaphoreTake(verrou, portMAX_DELAY); }
void deverrouiller() { xSemaphoreGive(verrou); }

struct AlerteCoupure {
  uint32_t numero = 0;                        // augmente à chaque nouvelle coupure
  float    valeur = 0;
  String   ts     = "";                       // heure de la coupure
};

struct Partage {
  // sécurité (cœur 1) → réseau (cœur 0)
  float    temp = 0, vib = 0;
  bool     tempOk = false, vibOk = false;     // NOUVEAU (09) : validité décidée par le cœur 1
  String   tsMesure = "";
  uint32_t numMesure = 0;                     // compteur des mesures
  bool     moteurEnMarche = false;
  String   causeRelais = "demarrage";
  uint32_t numRelais = 0;                     // compteur des changements du relais
  bool     buzzerActif = false;
  uint32_t numBuzzer = 0;                     // compteur des changements du buzzer
  AlerteCoupure alerteTemp, alerteVib;
  // réseau (cœur 0) → sécurité (cœur 1)
  String   commande = "";                     // "ON" / "OFF" reçu
  String   source   = "";
};
Partage partage;

// Ce que le réseau a déjà publié (compteurs du dernier envoi réussi)
uint32_t pubMesure = 0, pubRelais = 0, pubBuzzer = 0, pubAlerteTemp = 0, pubAlerteVib = 0;

// ===================================================================
// Variables de la tâche sécurité (cœur 1 uniquement)
// ===================================================================
const unsigned long PERIODE_MS     = 2000;   // mesures toutes les 2 s
const unsigned long ECHANTILLON_MS = 20;     // 1 lecture de vibration toutes les 20 ms
unsigned long derniereMesure     = 0;
unsigned long dernierEchantillon = 0;
float sommeCarres    = 0;
int   nbEchantillons = 0;
int   nbMuets        = 0;                    // NOUVEAU (09) : lectures « sans gravité »
const float G = 9.80665;
float derniereTemp = 0;
float derniereVib  = 0;
bool  derniereTempOk = false;                // NOUVEAU (09)
bool  derniereVibOk  = false;                // NOUVEAU (09)
float tempReference  = NAN;                  // NOUVEAU (09) : dernière température acceptée
float tempSuspecte   = NAN;                  // NOUVEAU (09) : saut en attente de confirmation
bool  mpuOk          = false;                // NOUVEAU (09) : MPU trouvé au démarrage ?
bool  moteurEnMarche = false;
bool  buzzerActif    = false;
bool  defautCritique = false;
int   etatBoutonPrecedent = HIGH;
unsigned long dernierChangementBouton = 0;

// ===================================================================
// NOUVEAU (09) : règles de validation (cœur 1)
// ===================================================================
// Plage plausible + rejet de la valeur piège 85,0 °C
bool temperaturePlausible(float t) {
  return !isnan(t) && t >= TEMP_MIN && t <= TEMP_MAX && t != TEMP_PIEGE;
}
bool vibrationPlausible(float v) { return !isnan(v) && v >= 0 && v <= VIB_MAX; }

// Le MPU6050 répond-il sur le bus I2C ?
bool mpuRepond() {
  Wire.beginTransmission(ADRESSE_MPU);
  return Wire.endTransmission() == 0;
}

// Contrôle du saut : > 10 °C en 2 s = suspect. Accepté si la mesure suivante le confirme.
bool controlerSaut(float t) {
  if (isnan(tempReference) || fabs(t - tempReference) <= SAUT_MAX_TEMP) {
    tempReference = t; tempSuspecte = NAN;
    return true;                                         // variation normale
  }
  if (!isnan(tempSuspecte) && fabs(t - tempSuspecte) <= SAUT_MAX_TEMP) {
    tempReference = t; tempSuspecte = NAN;
    Serial.printf("Saut confirme : nouvelle temperature %.1f C acceptee\r\n", t);
    return true;                                         // le saut se confirme : vrai changement
  }
  Serial.printf("Mesure suspecte ignoree : saut de %.1f C en 2 s\r\n", t - tempReference);
  tempSuspecte = t;
  return false;                                          // parasite probable
}

// ===== L'heure (utilisable par les deux cœurs) =====
String horodatage() {
  time_t maintenant = time(nullptr);
  if (maintenant < 1700000000) return "";   // pas encore d'heure NTP
  struct tm t;
  gmtime_r(&maintenant, &t);
  char texte[25];
  strftime(texte, sizeof(texte), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(texte);
}

// ===================================================================
// TÂCHE SÉCURITÉ (cœur 1) : actionneurs — chaque changement est déposé dans la boîte
// ===================================================================
void moteurMarche(const char* cause) {
  digitalWrite(PIN_RELAIS, HIGH);
  moteurEnMarche = true;
  verrouiller();
  partage.moteurEnMarche = true; partage.causeRelais = cause; partage.numRelais++;
  deverrouiller();
}
void moteurCoupe(const char* cause) {
  digitalWrite(PIN_RELAIS, LOW);
  moteurEnMarche = false;
  verrouiller();
  partage.moteurEnMarche = false; partage.causeRelais = cause; partage.numRelais++;
  deverrouiller();
}
void buzzerOn(bool confirmer = false) {
  ledcWriteTone(CANAL_BUZZER, 2000);
  bool change = !buzzerActif;
  buzzerActif = true;
  if (change || confirmer) { verrouiller(); partage.buzzerActif = true; partage.numBuzzer++; deverrouiller(); }
}
void buzzerOff(bool confirmer = false) {
  ledcWriteTone(CANAL_BUZZER, 0);
  bool change = buzzerActif;
  buzzerActif = false;
  if (change || confirmer) { verrouiller(); partage.buzzerActif = false; partage.numBuzzer++; deverrouiller(); }
}

// Appliquer une commande reçue par le réseau (le buzzer reste piloté par le cœur 1)
void appliquerCommande() {
  verrouiller();
  String cmd = partage.commande, src = partage.source;
  partage.commande = "";
  deverrouiller();
  if (cmd.length() == 0) return;
  if (cmd == "ON") buzzerOn(true); else buzzerOff(true);   // true = confirmer l'état même s'il n'a pas changé
  Serial.printf(">>> COMMANDE BUZZER : %s (source : %s)\r\n", cmd.c_str(), src.c_str());
}

// Coupure au seuil critique (MODIFIÉ (09) : on utilise la validité décidée avant)
void verifierSeuilCritique(float temp, bool tempOk, float vib, bool vibOk) {
  bool tempCritique = tempOk && temp > SEUIL_CRITIQUE_TEMP;
  bool vibCritique  = vibOk  && vib  > SEUIL_CRITIQUE_VIB;
  if ((tempCritique || vibCritique) && !defautCritique) {
    defautCritique = true;
    if (tempCritique && vibCritique) moteurCoupe("temperature et vibration critiques");
    else if (tempCritique)           moteurCoupe("temperature critique");
    else                             moteurCoupe("vibration critique");
    Serial.printf(">>> COUPURE LOCALE : temp = %.1f C, vib = %.2f g\r\n", temp, vib);

    String ts = horodatage();
    verrouiller();
    if (tempCritique) { partage.alerteTemp.valeur = temp; partage.alerteTemp.ts = ts; partage.alerteTemp.numero++; }
    if (vibCritique)  { partage.alerteVib.valeur  = vib;  partage.alerteVib.ts  = ts; partage.alerteVib.numero++;  }
    deverrouiller();
  }
}

// Bouton « Réarmement » (MODIFIÉ (09) : mesures VALIDES obligatoires)
void gererBoutonRearmement() {
  int etat = digitalRead(PIN_BOUTON);
  if (etat != etatBoutonPrecedent && millis() - dernierChangementBouton > 50) {
    dernierChangementBouton = millis();
    etatBoutonPrecedent = etat;
    if (etat == LOW) {
      if (!defautCritique) {
        Serial.println("Bouton : aucun defaut a rearmer\r");
      } else if (!derniereTempOk || !derniereVibOk) {
        Serial.println(">>> REARMEMENT REFUSE : capteur en defaut (mesure invalide)\r");
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

// ===================================================================
// TÂCHE RÉSEAU (cœur 0)
// ===================================================================
// Réception d'un message (appelée par mqtt.loop(), donc sur le cœur 0)
void recevoirMessage(String &topic, String &texte) {
  if (topic != TOPIC_BUZ_CMD) return;
  JsonDocument doc;
  if (deserializeJson(doc, texte)) { Serial.print("Commande ignoree : JSON invalide\r\n"); return; }
  String cmd = doc["cmd"] | "";
  if (cmd != "ON" && cmd != "OFF") { Serial.print("Commande ignoree : cmd doit etre ON ou OFF\r\n"); return; }
  verrouiller();
  partage.commande = cmd;
  partage.source   = doc["source"] | "inconnue";
  deverrouiller();
}

void surveillerWifi() {
  bool connecte = (WiFi.status() == WL_CONNECTED);
  if (connecte && !wifiConnecte) {
    Serial.print("Wi-Fi connecte, IP = ");
    Serial.println(WiFi.localIP());
        configTime(0, 0, "pool.ntp.org", "time.google.com", "162.159.200.1");   // 3 serveurs NTP : 2 noms + 1 IP directe (Cloudflare)
  }
  if (!connecte && wifiConnecte) Serial.println("Wi-Fi perdu : la surveillance locale continue\r");
  wifiConnecte = connecte;
}

void surveillerMqtt() {
  if (!wifiConnecte) { mqttConnecte = false; return; }
  mqtt.loop();
  mqttConnecte = mqtt.connected();
  if (mqttConnecte) return;
  if (millis() - dernierEssaiMqtt < ATTENTE_MQTT_MS) return;
  dernierEssaiMqtt = millis();

  Serial.print("Connexion MQTT (TLS 8883)...\r\n");
  if (mqtt.connect("esp32-pompe1", MQTT_USER, MQTT_PASS)) {   // peut bloquer ~6 s : seul le cœur 0 attend
    mqttConnecte = true;
    Serial.print("MQTT connecte au broker\r\n");
    mqtt.publish(TOPIC_ETAT, ETAT_EN_LIGNE, true, 1);
    Serial.print("Etat publie : en_ligne\r\n");
    mqtt.subscribe(TOPIC_BUZ_CMD, 1);
    pubRelais = UINT32_MAX;                   // forcer le renvoi des états actuels
    pubBuzzer = UINT32_MAX;
  } else {
    Serial.printf("MQTT echec : erreur %d, code broker %d\r\n", mqtt.lastError(), mqtt.returnCode());
  }
}

// Publier ce qui a changé dans la boîte
void publierDepuisPartage() {
  verrouiller();
  Partage copie = partage;                    // copie rapide, puis on rend le verrou tout de suite
  deverrouiller();

  String maintenant = horodatage();
  char texte[200];

  // 1) Mesures (QoS 0, non retenues) — MODIFIÉ (09) : validité fournie par le cœur 1
  if (copie.numMesure != pubMesure && copie.tsMesure.length() > 0) {
    JsonDocument d;
    if (isnan(copie.temp)) d["valeur"] = nullptr;                   // pas de NaN dans un JSON
    else                   d["valeur"] = serialized(String(copie.temp, 1));
    d["unite"]   = "°C";
    d["qualite"] = copie.tempOk ? "ok" : "erreur";
    d["ts"]      = copie.tsMesure;
    serializeJson(d, texte, sizeof(texte));
    mqtt.publish(TOPIC_TEMP, texte, false, 0);

    JsonDocument v;
    if (isnan(copie.vib)) v["valeur"] = nullptr;                    // capteur muet : valeur inconnue
    else                  v["valeur"] = serialized(String(copie.vib, 2));
    v["unite"]   = "g";
    v["qualite"] = copie.vibOk ? "ok" : "erreur";
    v["ts"]      = copie.tsMesure;
    serializeJson(v, texte, sizeof(texte));
    mqtt.publish(TOPIC_VIB, texte, false, 0);
    pubMesure = copie.numMesure;              // QoS 0 : une mesure ratée sera remplacée
  }

  if (maintenant.length() == 0) return;       // états et alertes ont besoin d'une heure

  // 2) État du relais (QoS 1, retenu)
  if (copie.numRelais != pubRelais) {
    JsonDocument d;
    d["etat"]  = copie.moteurEnMarche ? "marche" : "coupe";
    d["cause"] = copie.causeRelais;
    d["ts"]    = maintenant;
    serializeJson(d, texte, sizeof(texte));
    if (mqtt.publish(TOPIC_RELAIS_ETAT, texte, true, 1)) pubRelais = copie.numRelais;
  }

  // 3) État du buzzer (QoS 1, retenu)
  if (copie.numBuzzer != pubBuzzer) {
    JsonDocument d;
    d["etat"] = copie.buzzerActif ? "ON" : "OFF";
    d["ts"]   = maintenant;
    serializeJson(d, texte, sizeof(texte));
    if (mqtt.publish(TOPIC_BUZ_ETAT, texte, true, 1)) pubBuzzer = copie.numBuzzer;
  }

  // 4) Alertes « coupure locale » (QoS 1, non retenues)
  struct { AlerteCoupure *a; uint32_t *pub; const char *grandeur; float seuil; int dec; } liste[2] = {
    { &copie.alerteTemp, &pubAlerteTemp, "temperature", SEUIL_CRITIQUE_TEMP, 1 },
    { &copie.alerteVib,  &pubAlerteVib,  "vibration",   SEUIL_CRITIQUE_VIB,  2 },
  };
  for (auto &e : liste) {
    if (e.a->numero == *e.pub) continue;
    JsonDocument d;
    d["type"]     = "coupure_locale";
    d["grandeur"] = e.grandeur;
    d["valeur"]   = serialized(String(e.a->valeur, e.dec));
    d["seuil"]    = serialized(String(e.seuil, e.dec));
    d["ts"]       = e.a->ts.length() ? e.a->ts : maintenant;   // heure de la coupure si connue
    serializeJson(d, texte, sizeof(texte));
    if (mqtt.publish(TOPIC_ALERTES, texte, false, 1)) {
      *e.pub = e.a->numero;
      Serial.printf("Alerte envoyee : coupure locale (%s)\r\n", e.grandeur);
    }
  }
}

// La tâche réseau : tourne en boucle sur le cœur 0
void tacheReseau(void *param) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);                 // reconnexion Wi-Fi automatique
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  reseauTLS.setCACert(CA_CERT);
  mqtt.begin(MQTT_HOST, MQTT_PORT, reseauTLS);
  mqtt.setKeepAlive(KEEPALIVE_S);
  mqtt.setWill(TOPIC_ETAT, ETAT_HORS_LIGNE, true, 1);
  mqtt.onMessage(recevoirMessage);

  for (;;) {
    surveillerWifi();
    surveillerMqtt();
    if (mqttConnecte) publierDepuisPartage();
    vTaskDelay(pdMS_TO_TICKS(20));            // laisse respirer le cœur 0 (20 ms)
  }
}

// ===================================================================
void setup() {
  Serial.begin(115200);
  verrou = xSemaphoreCreateMutex();           // le verrou AVANT tout le reste

  pinMode(PIN_RELAIS, OUTPUT);
  pinMode(PIN_BOUTON, INPUT_PULLUP);
  moteurCoupe("demarrage");                   // position sûre au démarrage
  ledcSetup(CANAL_BUZZER, 2000, 8);
  ledcAttachPin(PIN_BUZZER, CANAL_BUZZER);
  buzzerOff();

  capteurTemp.begin();
  capteurTemp.setWaitForConversion(false);
  capteurTemp.requestTemperatures();

  Wire.begin(PIN_SDA, PIN_SCL);
  mpuOk = mpu.begin();                        // MODIFIÉ (09) : on retient le résultat
  if (mpuOk) mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  else       Serial.println("ERREUR : MPU6050 introuvable (vibration invalide)\r");

  // DÉCISION B (11) : pas de redémarrage automatique après un reset.
  // Le moteur reste coupé jusqu'au réarmement par le bouton (mesures valides, sous 75 °C et 0,5 g).
  moteurCoupe("demarrage : attente du rearmement");
  defautCritique = true;

  // Lancer la tâche réseau sur le cœur 0 (pile de 12 Ko pour TLS)
  xTaskCreatePinnedToCore(tacheReseau, "reseau", 12288, NULL, 1, NULL, 0);

  Serial.println("Demarrage : securite sur le coeur 1, reseau sur le coeur 0\r");
}

// loop() = TÂCHE SÉCURITÉ, sur le cœur 1 : ne parle jamais au réseau
void loop() {
  // ① Vibration : une lecture toutes les 20 ms
  if (millis() - dernierEchantillon >= ECHANTILLON_MS) {
    dernierEchantillon = millis();
    if (mpuOk) {
      sensors_event_t accel, gyro, tempMpu;
      mpu.getEvent(&accel, &gyro, &tempMpu);
      float x = accel.acceleration.x / G;
      float y = accel.acceleration.y / G;
      float z = accel.acceleration.z / G;
      if (x * x + y * y + z * z < NORME_MIN_G * NORME_MIN_G) nbMuets++;   // NOUVEAU (09) : pas de gravité = muet
      sommeCarres += x * x + y * y + (z - 1) * (z - 1);
      nbEchantillons++;
    }
  }

  // ② Bouton et commandes reçues : à chaque tour
  gererBoutonRearmement();
  appliquerCommande();

  // ③ Toutes les 2 s : mesures, VALIDATION, décision, dépôt dans la boîte, affichage
  if (millis() - derniereMesure >= PERIODE_MS) {
    derniereMesure = millis();

    // --- Température ---
    float t = capteurTemp.getTempCByIndex(0);
    capteurTemp.requestTemperatures();
    bool tOk = temperaturePlausible(t) && controlerSaut(t);           // NOUVEAU (09)

    // --- Vibration ---
    bool mpuVivant = mpuOk && mpuRepond() && nbEchantillons > 0
                     && nbMuets <= nbEchantillons / 2;                 // NOUVEAU (09)
    float v = mpuVivant ? sqrt(sommeCarres / nbEchantillons) : NAN;   // capteur muet : valeur inconnue
    bool vOk = mpuVivant && vibrationPlausible(v);
    sommeCarres = 0; nbEchantillons = 0; nbMuets = 0;

    derniereTemp = t;  derniereTempOk = tOk;
    derniereVib  = v;  derniereVibOk  = vOk;

    verifierSeuilCritique(t, tOk, v, vOk);                            // la sécurité d'abord

    String ts = horodatage();
    verrouiller();                                                    // déposer la mesure pour le réseau
    partage.temp = t;   partage.tempOk = tOk;
    partage.vib  = v;   partage.vibOk  = vOk;
    partage.tsMesure = ts; partage.numMesure++;
    deverrouiller();

    Serial.printf("[%s] Temp : %.1f C%s | Vib : %.2f g%s | Moteur : %s | Buzzer : %s | Wi-Fi : %s | MQTT : %s\r\n",
                  ts.length() ? ts.c_str() : "heure inconnue",
                  t, tOk ? "" : " (erreur)",
                  v, vOk ? "" : " (erreur)",
                  moteurEnMarche ? "MARCHE" : "COUPE",
                  buzzerActif ? "ON" : "OFF",
                  wifiConnecte ? "OK" : "...",
                  mqttConnecte ? "OK" : "...");
  }
}