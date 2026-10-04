# Mini projet IIoT — Surveillance d'une pompe centrifuge

Surveillance et maintenance prédictive du roulement d'une pompe centrifuge
(température + vibration), entièrement simulée.

## Architecture

ESP32 simulé (Wokwi dans VS Code) → MQTT (Mosquitto, TLS) → Node-RED (dashboard, alertes) + script Python

## Organisation du dépôt

| Dossier    | Contenu                                                     |
| ---------- | ----------------------------------------------------------- |
| 1-firmware | Code de l'ESP32 (PlatformIO + Wokwi)                        |
| 2-broker   | Configuration de Mosquitto (sans mots de passe)             |
| 3-node-red | Flows Node-RED                                              |
| 4-python   | Script d'analyse (bonus)                                    |
| 5-data     | Données produites par le système (non publiées)             |
| 6-docs     | Documentation : topics, schéma, cahier des charges, rapport |

## Auteur

Yazid — ENSA Béni Mellal, TDI, module IIoT
