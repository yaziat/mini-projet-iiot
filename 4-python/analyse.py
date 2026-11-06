"""Analyse des mesures et détection d'anomalies (bonus, étape 15).

Ce que fait le script, pour chaque grandeur (température, vibration) :
  1. charge l'historique CSV (mesures valides seulement) ;
  2. calcule la « valeur habituelle » (médiane) et l'« écart habituel » (MAD) de la minute précédente ;
  3. détecte les anomalies par z-score robuste : |z| > 3 (loin de l'habitude) et saut > 3 écarts
     par rapport à la mesure précédente (un nouveau palier compte une seule fois) ;
     (médiane et MAD au lieu de moyenne et écart-type : un premier saut ne « gonfle » pas la référence
      et ne cache donc pas les sauts suivants) ;
  4. calcule la tendance des 30 dernières minutes (pente) et estime le temps
     restant avant le seuil d'alerte et le seuil critique (maintenance prédictive) ;
  5. trace un graphique avec les anomalies et les seuils, et écrit un petit rapport.

Bibliothèques : numpy et matplotlib seulement (pandas retiré : Windows Smart App Control
bloquait ses fichiers compilés).

Lancement (depuis la racine du projet) :
    python 4-python/analyse.py                       (prend mesures_python.csv, sinon mesures_nodered.csv)
    python 4-python/analyse.py --fichier 5-data/mesures_nodered.csv
    python 4-python/analyse.py --depuis 60            (seulement les 60 dernières minutes)
    python 4-python/analyse.py --demo                 (données SIMULÉES : usure lente sur 7 jours)
"""
import argparse
import csv
import math
from datetime import datetime, timezone
from pathlib import Path

import matplotlib
matplotlib.use("Agg")                     # pas de fenêtre : on enregistre des images
import matplotlib.pyplot as plt
import numpy as np

RACINE = Path(__file__).resolve().parent.parent
SORTIE = Path(__file__).resolve().parent / "resultats"

FENETRE  = "60s"       # référence = la minute PRÉCÉDENTE (fenêtre de temps : après un arrêt, on ne compare pas à de vieilles mesures)
FENETRE_S = 60         # la même fenêtre, en secondes
MIN_MESURES = 3        # il faut au moins 3 mesures dans la fenêtre pour juger
Z_MAX    = 3.0         # au-delà de 3 « écarts habituels » : anomalie
K_MAD    = 1.4826      # MAD × 1,4826 = écart-type équivalent (pour un bruit normal)
ECART_MIN = {"temperature": 0.5, "vibration": 0.02}   # évite de diviser par ~0 quand le signal est plat
TENDANCE_MIN = 30      # tendance calculée sur les 30 dernières minutes
DUREE_MIN_TENDANCE = 0.5   # il faut des mesures sur au moins la moitié de cette durée, sinon pas de prévision

NOMS = {"temperature": "Température", "vibration": "Vibration"}
TROU_FACTEUR = 10      # un écart 10 fois plus long que d'habitude = simulation arrêtée : on coupe la courbe

SEUILS = {             # mêmes valeurs que l'ESP32 et Node-RED
    "temperature": {"unite": "°C", "alerte": 75.0, "critique": 85.0},
    "vibration":   {"unite": "g",  "alerte": 0.5,  "critique": 1.0},
}

SECONDE = np.timedelta64(1, "s")


def lire_date(texte):
    """« 2026-11-04T12:27:49Z » → datetime64 en UTC (None si illisible)."""
    try:
        t = datetime.fromisoformat(texte.strip().replace("Z", "+00:00"))
    except (ValueError, AttributeError):
        return None
    if t.tzinfo is not None:
        t = t.astimezone(timezone.utc).replace(tzinfo=None)
    return np.datetime64(t, "us")


def lire_nombre(texte):
    try:
        v = float(texte)
    except (ValueError, TypeError):
        return None
    return None if math.isnan(v) else v


def charger(fichier):
    """Renvoie {grandeur: (dates, valeurs)} triés par date, et le nombre de lignes ignorées."""
    lignes, nb_total = [], 0
    with open(fichier, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            nb_total += 1
            t, v = lire_date(r.get("ts")), lire_nombre(r.get("valeur"))
            if r.get("qualite") == "ok" and t is not None and v is not None:
                lignes.append((t, r.get("grandeur"), v))
    lignes.sort(key=lambda l: l[0])
    d = {}
    for g in SEUILS:
        sel = [l for l in lignes if l[1] == g]
        d[g] = (np.array([l[0] for l in sel], dtype="datetime64[us]"), np.array([l[2] for l in sel], dtype=float))
    return d, nb_total - len(lignes)


def donnees_demo():
    """Données SIMULÉES pour illustrer la maintenance prédictive : 7 jours, une mesure toutes les 5 min.
    Le roulement s'use : la température monte lentement (55 → 68 °C) et la vibration aussi (0,15 → 0,38 g),
    avec du bruit et deux pics isolés (parasites)."""
    rng = np.random.default_rng(42)
    n = 7 * 24 * 12
    ts = np.datetime64("2026-11-01T00:00", "us") + np.arange(n) * np.timedelta64(5, "m")
    k = np.linspace(0, 1, n) ** 1.6                              # usure qui s'accélère
    temp = 55 + 13 * k + rng.normal(0, 0.6, n)
    vib = 0.15 + 0.23 * k + rng.normal(0, 0.015, n)
    temp[600] += 9; vib[1400] += 0.25                            # deux parasites
    return {"temperature": (ts, temp), "vibration": (ts.copy(), vib)}


def couper_trous(t, v):
    """Insère un trou (NaN) là où la simulation était arrêtée, pour ne pas relier deux séances par une droite."""
    if len(t) < 2:
        return t, v
    ecarts = np.diff(t) / SECONDE
    trous = np.flatnonzero(ecarts > TROU_FACTEUR * np.median(ecarts)) + 1
    if len(trous) == 0:
        return t, v
    t2 = np.concatenate([t, t[trous] - SECONDE])
    v2 = np.concatenate([v, np.full(len(trous), np.nan)])
    ordre = np.argsort(t2, kind="stable")
    return t2[ordre], v2[ordre]


def fenetre_precedente(t, x):
    """Pour chaque mesure : médiane et MAD des mesures de la fenêtre [t − FENETRE, t[
    (la mesure testée n'est pas dans sa propre référence). NaN s'il y a moins de MIN_MESURES mesures."""
    debut = np.searchsorted(t, t - np.timedelta64(FENETRE_S, "s"), side="left")
    mediane = np.full(len(x), np.nan)
    mad = np.full(len(x), np.nan)
    for i, a in enumerate(debut):
        if i - a >= MIN_MESURES:
            w = x[a:i]
            m = np.median(w)
            mediane[i] = m
            mad[i] = np.median(np.abs(w - m))
    return mediane, mad


def analyser(d, grandeur):
    s = SEUILS[grandeur]
    t, x = d[grandeur]

    moyenne, mad = fenetre_precedente(t, x)                      # valeur habituelle (médiane) et écart habituel (MAD)
    ecart = np.maximum(K_MAD * mad, ECART_MIN[grandeur])         # minimum imposé : pas de division par 0
    z = (x - moyenne) / ecart                                    # z-score robuste
    saut = np.abs(np.diff(x, prepend=np.nan)) / ecart            # écart avec la mesure d'avant
    # Anomalie = la mesure est loin de l'habitude (|z| > 3) ET elle vient de sauter (> 3 écarts) :
    # un nouveau palier n'est compté qu'une fois, au moment du saut, pas à chaque mesure qui suit.
    with np.errstate(invalid="ignore"):
        anomalie = (np.abs(z) > Z_MAX) & (saut > Z_MAX)

    # Tendance sur les 30 dernières minutes : droite des moindres carrés
    garde = t >= t.max() - np.timedelta64(TENDANCE_MIN, "m")
    tr, xr = t[garde], x[garde]
    heures = (tr - tr[0]) / SECONDE / 3600
    pente, origine = np.polyfit(heures, xr, 1) if len(xr) > 10 else (0.0, xr.mean())
    actuelle = xr[-5:].mean()                                    # valeur actuelle = moyenne des 5 dernières mesures

    duree_min = (tr.max() - tr.min()) / SECONDE / 60 if len(tr) else 0
    assez = duree_min >= DUREE_MIN_TENDANCE * TENDANCE_MIN   # une droite tirée sur 1 minute ne prédit rien

    def temps_avant(seuil):
        if actuelle >= seuil:
            return "déjà atteint"
        if not assez:
            return f"pas de prévision (seulement {duree_min:.0f} min de mesures récentes, il en faut {DUREE_MIN_TENDANCE * TENDANCE_MIN:.0f})"
        if pente <= 0:
            return "jamais (pas de montée)"
        h = (seuil - actuelle) / pente
        if h < 1:
            return f"environ {h * 60:.0f} min"
        if h < 48:
            return f"environ {h:.1f} h"
        return f"environ {h / 24:.1f} jours ({h:.0f} h)"

    resume = {
        "grandeur": grandeur,
        "nb": len(x), "debut": t.min().item(), "fin": t.max().item(),
        "moyenne": x.mean(), "min": x.min(), "max": x.max(),
        "nb_anomalies": int(anomalie.sum()),
        "pente": pente, "actuelle": actuelle,
        "avant_alerte": temps_avant(s["alerte"]),
        "avant_critique": temps_avant(s["critique"]),
    }
    return t, x, moyenne, ecart, anomalie, (tr, origine + pente * heures), resume


def tracer(grandeur, t, x, moyenne, ecart, anomalie, droite, fichier):
    s = SEUILS[grandeur]
    fig, ax = plt.subplots(figsize=(12, 4.8))
    ax.plot(*couper_trous(t, x), lw=0.8, color="#64748b", label="mesure")
    ax.plot(*couper_trous(t, moyenne), lw=1.6, color="#2563eb", label=f"médiane mobile ({FENETRE})")
    tz, bas_z = couper_trous(t, moyenne - Z_MAX * ecart)
    _, haut_z = couper_trous(t, moyenne + Z_MAX * ecart)
    ax.fill_between(tz, bas_z, haut_z,
                    color="#2563eb", alpha=0.12, label="zone normale (±3 écarts habituels)")
    ax.scatter(t[anomalie], x[anomalie], color="#dc2626", s=26, zorder=3,
               label=f"anomalies ({int(anomalie.sum())})")
    ax.plot(droite[0], droite[1], "--", color="#7c3aed", lw=2, label=f"tendance (sur {TENDANCE_MIN} min)")
    ax.axhline(s["alerte"], color="#f59e0b", ls=":", lw=1.5, label=f"seuil d'alerte {s['alerte']} {s['unite']}")
    ax.axhline(s["critique"], color="#dc2626", ls=":", lw=1.5, label=f"seuil critique {s['critique']} {s['unite']}")
    ax.set_title(f"{NOMS[grandeur]} du palier — détection d'anomalies (z-score robuste)")
    ax.set_ylabel(s["unite"])
    bas, haut = np.nanmin(x), np.nanmax(x)
    marge = (max(haut, s["critique"]) - bas) * 0.08
    ax.set_ylim(bas - marge, max(haut, s["critique"]) + marge)
    ax.grid(alpha=0.3)
    ax.legend(loc="upper left", fontsize=8, ncol=2)
    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(fichier, dpi=130)
    plt.close(fig)


def main():
    par_defaut = RACINE / "5-data" / "mesures_python.csv"
    if not par_defaut.exists():
        par_defaut = RACINE / "5-data" / "mesures_nodered.csv"
    ap = argparse.ArgumentParser(description="Analyse des mesures de la pompe 1")
    ap.add_argument("--fichier", type=Path, default=par_defaut)
    ap.add_argument("--depuis", type=int, metavar="MIN", help="analyser seulement les N dernières minutes")
    ap.add_argument("--demo", action="store_true", help="données simulées d'usure lente (illustration)")
    args = ap.parse_args()

    global FENETRE, FENETRE_S, TENDANCE_MIN
    if args.demo:
        d, nb_ignorees, nom = donnees_demo(), 0, "DONNÉES SIMULÉES (démonstration d'usure lente sur 7 jours)"
        FENETRE, FENETRE_S, TENDANCE_MIN = "3h", 3 * 3600, 24 * 60   # fenêtre 3 h, tendance sur le dernier jour
        sortie = SORTIE / "demo"
    else:
        d, nb_ignorees = charger(args.fichier)
        nom, sortie = args.fichier.name, SORTIE
        if args.depuis:
            fin = max(t.max() for t, _ in d.values() if len(t))
            limite = fin - np.timedelta64(args.depuis, "m")
            d = {g: (t[t >= limite], x[t >= limite]) for g, (t, x) in d.items()}
            nom += f" (dernières {args.depuis} min)"
    sortie.mkdir(parents=True, exist_ok=True)
    lignes = [f"Analyse de {nom}",
              f"Mesures en erreur ou incomplètes ignorées : {nb_ignorees}", ""]

    for g in SEUILS:
        if len(d[g][0]) == 0:
            lignes += [f"== {g.upper()} ==", "Aucune mesure valide.", ""]
            continue
        t, x, moyenne, ecart, anomalie, droite, r = analyser(d, g)
        tracer(g, t, x, moyenne, ecart, anomalie, droite, sortie / f"{g}.png")
        u = SEUILS[g]["unite"]
        lignes += [
            f"== {g.upper()} ==",
            f"Mesures valides : {r['nb']}  ({r['debut']:%d/%m %H:%M} → {r['fin']:%d/%m %H:%M} UTC)",
            f"Moyenne {r['moyenne']:.2f} {u} · min {r['min']:.2f} · max {r['max']:.2f}",
            f"Anomalies (sauts inhabituels, |z| > {Z_MAX:g}) : {r['nb_anomalies']}",
            f"Tendance (sur {TENDANCE_MIN} min) : {r['pente']:+.3f} {u}/h · valeur actuelle ≈ {r['actuelle']:.2f} {u}",
            f"Temps estimé avant le seuil d'alerte : {r['avant_alerte']}",
            f"Temps estimé avant le seuil critique : {r['avant_critique']}",
            "",
        ]

    rapport = "\n".join(lignes)
    (sortie / "rapport_anomalies.txt").write_text(rapport, encoding="utf-8")
    print(rapport)
    print(f"Graphiques et rapport enregistrés dans {sortie}")


if __name__ == "__main__":
    main()
