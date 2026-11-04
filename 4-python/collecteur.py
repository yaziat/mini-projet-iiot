"""Collecteur MQTT (bonus, étape 15).

Abonné indépendant au broker, avec le compte `python` (lecture seule, ACL).
Il enregistre son PROPRE historique, même si Node-RED est arrêté :
  - 5-data/mesures_python.csv     : toutes les mesures (température, vibration)
  - 5-data/evenements_python.csv  : états, commandes, alertes

Le mot de passe n'est jamais écrit dans le code : il est demandé au lancement
(ou lu dans la variable d'environnement MQTT_PASSWORD_PYTHON).

Lancement (depuis la racine du projet) :
    python 4-python/collecteur.py
Arrêt : Ctrl + C
"""
import csv
import getpass
import json
import os
import ssl
import time
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt

# --- Réglages ---
RACINE      = Path(__file__).resolve().parent.parent
CA_CERT     = RACINE / "2-broker" / "certs" / "ca.crt"
F_MESURES   = RACINE / "5-data" / "mesures_python.csv"
F_EVENEMENTS = RACINE / "5-data" / "evenements_python.csv"

BROKER      = "localhost"
PORT        = 8883                       # TLS uniquement
UTILISATEUR = "python"                   # compte en lecture seule (ACL)
CLIENT_ID   = "python-pompe1"
TOPIC       = "usine/ligne1/pompe1/#"
KEEPALIVE   = 30


def maintenant():
    """Heure UTC au même format que l'ESP32 : 2026-11-04T10:15:30Z."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def ajouter_ligne(fichier, entete, ligne):
    """Ajoute une ligne au fichier CSV (crée l'en-tête si le fichier est nouveau)."""
    nouveau = not fichier.exists()
    with fichier.open("a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if nouveau:
            w.writerow(entete)
        w.writerow(ligne)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:
        print(f"Connexion refusée : {reason_code}")
        return
    print(f"Connecté au broker {BROKER}:{PORT} en TLS (compte '{UTILISATEUR}')")
    client.subscribe(TOPIC, qos=1)
    print(f"Abonné à {TOPIC}")


def on_disconnect(client, userdata, flags, reason_code, properties):
    if reason_code.is_failure:            # coupure imprévue (broker arrêté, réseau…)
        print(f"Déconnecté ({reason_code}) : reconnexion automatique…")
    else:                                 # arrêt demandé par Ctrl + C
        print("Déconnecté proprement.")


def on_message(client, userdata, msg):
    recu = maintenant()
    try:
        p = json.loads(msg.payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        print(f"Message ignoré (pas du JSON) : {msg.topic}")
        return
    if not isinstance(p, dict):
        return

    grandeur = msg.topic.rsplit("/", 1)[-1]
    if "/capteurs/" in msg.topic:
        ajouter_ligne(F_MESURES,
                      ["ts", "grandeur", "valeur", "unite", "qualite", "recu"],
                      [p.get("ts", ""), grandeur, "" if p.get("valeur") is None else p.get("valeur"),
                       p.get("unite", ""), p.get("qualite", ""), recu])
        print(f"[{p.get('ts', '?')}] {grandeur:<11} = {p.get('valeur')} {p.get('unite', '')} ({p.get('qualite')})")
    else:
        if msg.retain:                    # état retenu rejoué à la connexion : pas un nouvel événement
            return
        ajouter_ligne(F_EVENEMENTS, ["recu", "topic", "contenu"],
                      [recu, msg.topic, json.dumps(p, ensure_ascii=False)])
        print(f"Événement : {msg.topic} {p}")


def main():
    mot_de_passe = os.environ.get("MQTT_PASSWORD_PYTHON") or getpass.getpass("Mot de passe du compte python : ")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=CLIENT_ID,
                         clean_session=False)          # session gardée : événements QoS 1 rattrapés
    client.username_pw_set(UTILISATEUR, mot_de_passe)
    client.tls_set(ca_certs=str(CA_CERT), tls_version=ssl.PROTOCOL_TLS_CLIENT)   # vérifie le certificat du broker
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    F_MESURES.parent.mkdir(exist_ok=True)
    client.connect(BROKER, PORT, KEEPALIVE)
    client.loop_start()                   # le réseau tourne dans un fil séparé
    try:
        while True:                       # le fil principal attend : Ctrl + C l'interrompt (aussi sous Windows)
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nArrêt demandé.")
    finally:
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    main()
