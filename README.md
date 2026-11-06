# Mini-projet IIoT — Surveillance d'une pompe centrifuge

Surveillance et maintenance prédictive du **palier d'une pompe centrifuge** (température et vibration),
entièrement **simulée** : ESP32 dans Wokwi → MQTT chiffré (TLS) → Mosquitto → Node-RED + Python.

| | |
|---|---|
| **Réalisé par** | MOHAMED ELYAZID ATTARCA |
| **Encadré par** | Prof. Touil |
| **Module / filière** | IIoT — Transformation Digitale Industrielle (TDI), 2ᵉ année |
| **Établissement** | ENSA Béni Mellal — année 2026/2027 |
| **Vidéo de démonstration** | https://youtu.be/dGGZYHPi2UU (9 min 49) |
| **Rapport** | [`6-docs/rapport/`](6-docs/rapport/) (PDF et DOCX, 12 pages) |

> **Principe directeur :** prévenir d'abord, couper ensuite, et ne jamais faire dépendre la sécurité du réseau.

---

## 1. Architecture

```
 PERCEPTION              RÉSEAU                    EDGE / SUPERVISION              ANALYSE
┌──────────────┐   MQTT + TLS 1.2   ┌─────────────┐   TLS 1.3   ┌────────────────────────┐
│ ESP32 (Wokwi)│ ─────────────────► │  Mosquitto  │ ──────────► │ Node-RED + Dashboard 2 │
│ DS18B20      │      port 8883     │  (broker)   │ ◄────────── │ validation, alertes,   │
│ MPU6050      │ ◄───────────────── │  TLS + ACL  │  commande   │ acquittement, journal  │
│ relais, buzz.│   commande buzzer  └──────┬──────┘   buzzer    └────────────────────────┘
└──────────────┘                           │ TLS 1.3 (lecture seule)
  coupure locale                           ▼
  > 85 °C / > 1 g                 ┌─────────────────────┐
  (sans réseau)                   │ Python : collecteur │
                                  │ + analyse (z-score, │
                                  │   tendance)         │
                                  └─────────────────────┘
```

| Couche | Rôle | Décide quoi |
|---|---|---|
| ESP32 | Mesure, valide, coupe le moteur | **Coupure critique** (> 85 °C ou > 1 g), même sans réseau |
| Mosquitto | Transporte les messages, contrôle l'accès | Qui a le droit de lire / écrire (ACL) |
| Node-RED | Supervise, alerte, trace | **Alerte** (> 75 °C ou > 0,5 g en moyenne), buzzer, acquittement |
| Python | Analyse l'historique | Anomalies et temps estimé avant les seuils |

## 2. Organisation du dossier

| Dossier | Contenu |
|---|---|
| `1-firmware/` | Code ESP32 (`src/main.cpp`), circuit Wokwi (`diagram.json`, `wokwi.toml`), `platformio.ini`, modèle `include/secrets.example.h` |
| `2-broker/` | `mosquitto.conf`, `acl`, certificats **publics** `certs/ca.crt` et `certs/server.crt` |
| `3-node-red/` | `flows.json` (validation, décision, alertes, tableau de bord, journal) |
| `4-python/` | `collecteur.py`, `analyse.py`, `requirements.txt`, graphiques dans `resultats/` |
| `5-data/` | Données produites : mesures, journal horodaté, événements (CSV) |
| `6-docs/` | Rapport, présentation, captures par étape, `segmentation.md` |

**Jamais publiés** (exclus par `.gitignore`) : `secrets.h`, `passwd`, clés privées `*.key`, `flows_cred.json`, logs du broker.

## 3. Matériel simulé (Wokwi)

| Composant | Broche ESP32 | Rôle |
|---|---|---|
| DS18B20 | GPIO 4 | Température du palier (°C) |
| MPU6050 | SDA 21 / SCL 22 | Vibration (RMS, en g) |
| Relais | GPIO 26 | Moteur de la pompe (sécurité positive) |
| Buzzer | GPIO 27 | Alarme sonore (commandée par Node-RED) |
| Bouton | GPIO 25 | Réarmement manuel |

## 4. Topics MQTT

Format des mesures : `{"valeur": 62.4, "unite": "°C", "qualite": "ok", "ts": "2026-11-04T12:27:49Z"}`

| Topic (`usine/ligne1/pompe1/…`) | Publié par | QoS | Retenu |
|---|---|---|---|
| `capteurs/temperature` | ESP32 | 0 | non |
| `capteurs/vibration` | ESP32 | 0 | non |
| `actionneurs/relais/etat` | ESP32 | 1 | oui |
| `actionneurs/buzzer/etat` | ESP32 | 1 | oui |
| `actionneurs/buzzer/commande` | Node-RED | 1 | non |
| `alertes` | ESP32 (coupure locale), Node-RED | 1 | non |
| `etat` (en ligne / hors ligne, Last Will) | ESP32, broker | 1 | oui |

QoS 0 pour les mesures (une nouvelle arrive toutes les 2 s), QoS 1 pour ce qui ne doit pas se perdre.
Seuls les **états** sont retenus : un ordre retenu serait rejoué à tort.

## 5. Sécurité

| Protection | Contre quoi |
|---|---|
| TLS uniquement sur le port 8883 (1883 fermé), CA locale OpenSSL | Interception des données et des mots de passe |
| Pas d'accès anonyme, un compte par appareil (`esp32`, `nodered`, `python`) | Connexion d'un inconnu, usurpation |
| ACL par topic, compte `python` en lecture seule | Compte volé qui envoie de fausses commandes |
| Mots de passe hors du code et de Git | Fuite des identifiants par le dépôt |
| Validation des mesures (ESP32 puis Node-RED) | Fausse coupure due à un capteur défaillant |

## 6. Lancer le projet (Windows)

**Prérequis :** Mosquitto 2.x, Node.js + Node-RED 4, VS Code + PlatformIO + extension Wokwi, Python 3.10+.

> Les chemins de `mosquitto.conf` pointent vers `C:/Users/YAZID/Documents/mini-projet-iiot/` : les adapter si le dossier est ailleurs.

**1. Secrets (une seule fois)** — rien de tout cela n'est dans le dossier :
```powershell
# Comptes du broker (le mot de passe est demandé, masqué)
mosquitto_passwd -c 2-broker\passwd esp32
mosquitto_passwd    2-broker\passwd nodered
mosquitto_passwd    2-broker\passwd python
# Firmware : copier le modèle puis y mettre le mot de passe du compte esp32
copy 1-firmware\include\secrets.example.h 1-firmware\include\secrets.h
```
La clé privée du serveur (`server.key`) n'est pas fournie : regénérer une CA et un certificat serveur avec OpenSSL
(SAN : `host.wokwi.internal`, `localhost`, `127.0.0.1`), puis mettre le nouveau `ca.crt` dans `1-firmware/include/ca_cert.h`.

**2. Broker** (le port 8883 doit être libre) :
```powershell
& "C:\Program Files\mosquitto\mosquitto.exe" -c 2-broker\mosquitto.conf -v
```

**3. Node-RED :** `node-red`, puis importer `3-node-red/flows.json`, installer la palette `@flowfuse/node-red-dashboard`,
saisir le mot de passe du compte `nodered` dans le nœud « Mosquitto TLS ».
Tableau de bord : http://localhost:1880/dashboard/pompe1

**4. ESP32 :** ouvrir `1-firmware` dans VS Code → PlatformIO **Build** → `F1` → **Wokwi: Start Simulator**.
Au démarrage le moteur est coupé : appuyer sur le bouton de réarmement.

**5. Python (bonus) :**
```powershell
pip install -r 4-python\requirements.txt
python 4-python\collecteur.py          # mot de passe du compte python demandé, masqué
python 4-python\analyse.py             # analyse des vraies mesures  → 4-python\resultats\
python 4-python\analyse.py --demo      # usure simulée sur 7 jours   → 4-python\resultats\demo\
```

## 7. Tests réalisés

| Test | Scénario | Résultat |
|---|---|---|
| T1 | Chaîne complète : seuil, alerte, buzzer, coupure, acquittement, réarmement | ✓ 3 alertes traitées, liste vide à la fin |
| T2 | Connexion anonyme puis mauvais mot de passe | ✓ « not authorised » dans le log |
| T3 | Le compte `python` tente de publier une commande | ✓ Lecture autorisée, publication refusée |
| T4 | Arrêt brutal de la simulation | ✓ Chien de garde (10 s) puis Last Will |
| T5 | Redémarrage du broker | ✓ Coupure locale à 89,3 °C sans broker, reconnexion seule |
| T6 | Valeurs aberrantes (85,0 °C, saut, capteurs débranchés) | ✓ « erreur », aucune fausse coupure |
| T7 | Node-RED arrêté | ✓ Sécurité intacte, événements rattrapés |

Les preuves (captures) sont rangées par étape dans `6-docs/captures/`.

## 8. Limites

Projet **entièrement simulé sur un seul PC** : pas de vrai capteur, pas de vrai réseau d'usine.
La segmentation IT/OT (VLAN, DMZ, pare-feu, IEC 62443) est proposée **sous forme de schéma** seulement :
voir `6-docs/segmentation.md` et le chapitre 8 du rapport.
