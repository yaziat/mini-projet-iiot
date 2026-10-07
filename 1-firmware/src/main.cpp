#include <Arduino.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// --- Broches ---
const int PIN_DS18B20 = 4;    // DQ du DS18B20
const int PIN_SDA     = 21;   // SDA du MPU6050
const int PIN_SCL     = 22;   // SCL du MPU6050
const int PIN_RELAIS  = 26;   // IN du relais
const int PIN_BUZZER  = 27;   // + du buzzer
const int PIN_BOUTON  = 25;   // bouton « Réarmement » (vers GND)

// --- Seuils (cahier des charges) ---
const float SEUIL_CRITIQUE_TEMP = 85.0;   // °C → coupure
const float SEUIL_CRITIQUE_VIB  = 1.0;    // g  → coupure
const float SEUIL_REARM_TEMP    = 80.0;   // °C → réarmement possible en dessous (hystérésis)
const float SEUIL_REARM_VIB     = 0.9;    // g  → réarmement possible en dessous (hystérésis)

// --- Plages plausibles (une valeur hors plage n'est JAMAIS comparée aux seuils) ---
const float TEMP_MIN = 0.0, TEMP_MAX = 120.0;
const float VIB_MAX  = 8.0;

// --- Capteurs ---
OneWire busOneWire(PIN_DS18B20);
DallasTemperature capteurTemp(&busOneWire);
Adafruit_MPU6050 mpu;

// --- Rythmes ---
const unsigned long PERIODE_MS     = 2000;
const unsigned long ECHANTILLON_MS = 20;
unsigned long derniereMesure     = 0;
unsigned long dernierEchantillon = 0;

// --- Calcul de la RMS ---
float sommeCarres    = 0;
int   nbEchantillons = 0;
const float G = 9.80665;

// --- Dernières valeurs mesurées ---
float derniereTemp = 0;
float derniereVib  = 0;

// --- États ---
bool moteurEnMarche = false;
bool buzzerActif    = false;
bool defautCritique = false;   // vrai = coupure locale en cours

// --- Bouton (anti-rebond) ---
int  etatBoutonPrecedent = HIGH;
unsigned long dernierChangementBouton = 0;

// ===== Actionneurs =====
// Sécurité positive : relais ACTIVÉ = moteur en MARCHE.
void moteurMarche() { digitalWrite(PIN_RELAIS, HIGH); moteurEnMarche = true;  }
void moteurCoupe()  { digitalWrite(PIN_RELAIS, LOW);  moteurEnMarche = false; }
void buzzerOn()     { tone(PIN_BUZZER, 2000); buzzerActif = true;  }
void buzzerOff()    { noTone(PIN_BUZZER);     buzzerActif = false; }

// ===== Décision locale (device) =====
void verifierSeuilCritique(float temp, float vib) {
  bool tempValide = (temp >= TEMP_MIN && temp <= TEMP_MAX);
  bool vibValide  = (vib >= 0 && vib <= VIB_MAX);

  bool critique = (tempValide && temp > SEUIL_CRITIQUE_TEMP) ||
                  (vibValide  && vib  > SEUIL_CRITIQUE_VIB);

  if (critique && !defautCritique) {
    defautCritique = true;
    moteurCoupe();
    Serial.printf(">>> COUPURE LOCALE : temp = %.1f C, vib = %.2f g\r\n", temp, vib);
  }
}

// ===== Bouton « Réarmement » =====
void gererBoutonRearmement() {
  int etat = digitalRead(PIN_BOUTON);
  if (etat != etatBoutonPrecedent && millis() - dernierChangementBouton > 50) {
    dernierChangementBouton = millis();
    etatBoutonPrecedent = etat;

    if (etat == LOW) {                       // le bouton vient d'être appuyé
      if (!defautCritique) {
        Serial.println("Bouton : aucun defaut a rearmer\r");
      } else if (derniereTemp < SEUIL_REARM_TEMP && derniereVib < SEUIL_REARM_VIB) {
        defautCritique = false;
        moteurMarche();
        Serial.println(">>> REARMEMENT ACCEPTE : moteur redemarre\r");
      } else {
        Serial.println(">>> REARMEMENT REFUSE : valeurs encore trop elevees\r");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Actionneurs et bouton : position sûre au démarrage
  pinMode(PIN_RELAIS, OUTPUT);
  pinMode(PIN_BOUTON, INPUT_PULLUP);       // relâché = HIGH, appuyé = LOW
  moteurCoupe();
  buzzerOff();

  // DS18B20
  capteurTemp.begin();
  capteurTemp.setWaitForConversion(false);
  capteurTemp.requestTemperatures();

  // MPU6050
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!mpu.begin()) {
    Serial.println("ERREUR : MPU6050 introuvable\r");
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);

  // Test des actionneurs
  buzzerOn(); delay(300); buzzerOff();
  moteurMarche();
  Serial.println("Demarrage : moteur en marche, surveillance active\r");
}

void loop() {
  // ① Toutes les 20 ms : une lecture de vibration
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

  // ② À chaque tour : surveiller le bouton
  gererBoutonRearmement();

  // ③ Toutes les 2 s : mesures + décision
  if (millis() - derniereMesure >= PERIODE_MS) {
    derniereMesure = millis();

    derniereTemp = capteurTemp.getTempCByIndex(0);
    capteurTemp.requestTemperatures();
    derniereVib = (nbEchantillons > 0) ? sqrt(sommeCarres / nbEchantillons) : 0;
    sommeCarres = 0;
    nbEchantillons = 0;

    verifierSeuilCritique(derniereTemp, derniereVib);

    Serial.printf("Temp : %.1f C | Vib : %.2f g | Moteur : %s | Defaut : %s\r\n",
                  derniereTemp, derniereVib,
                  moteurEnMarche ? "MARCHE" : "COUPE",
                  defautCritique ? "OUI" : "NON");
  }
}