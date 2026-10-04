# Mini-projet IIoT

Chaîne IIoT : ESP32 (Wokwi) → MQTT (Mosquitto) → Node-RED → stockage CSV / analyse Python.

## Structure

```
mini-projet-iiot/
├── 1-firmware/ projet PlatformIO de l'ESP32 (platformio.ini, src/, diagram.json, wokwi.toml)
├── 2-broker/   configuration de Mosquitto (mosquitto.conf, ACL), sans mots de passe
├── 3-node-red/ export des flows (flows.json)
├── 4-python/   script d'analyse (bonus)
├── 5-data/     historique des mesures et journal (CSV)
├── 6-docs/     tableau des topics, schéma, cahier des charges, rapport
├── .gitignore
└── README.md
```

## Démarrage

1. **Firmware** : ouvrir `1-firmware/` dans PlatformIO, compiler, lancer la simulation Wokwi.
2. **Broker** : lancer Mosquitto avec `2-broker/mosquitto.conf` (créer le fichier `passwd` localement avec `mosquitto_passwd`).
3. **Node-RED** : importer `3-node-red/flows.json`.
4. **Analyse** : `python 4-python/analyse.py`.
