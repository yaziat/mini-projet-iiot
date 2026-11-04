"""Analyse des mesures et détection d'anomalies (bonus, étape 15).

Ce que fait le script, pour chaque grandeur (température, vibration) :
  1. charge l'historique CSV (mesures valides seulement) ;
  2. calcule une moyenne mobile et un écart-type mobile sur la minute précédente ;
  3. détecte les anomalies par z-score : |z| > 3 = valeur très éloignée de son passé récent ;
  4. calcule la tendance des 30 dernières minutes (pente) et estime le temps
     restant avant le seuil d'alerte et le seuil critique (maintenance prédictive) ;
  5. trace un graphique avec les anomalies et les seuils, et écrit un petit rapport.

Lancement (depuis la racine du projet) :
    python 4-python/analyse.py                       (prend mesures_python.csv, sinon mesures_nodered.csv)
    python 4-python/analyse.py --fichier 5-data/mesures_nodered.csv
    python 4-python/analyse.py --depuis 60            (seulement les 60 dernières minutes)
    python 4-python/analyse.py --demo                 (données SIMULÉES : usure lente sur 7 jours)
"""
import argparse
from pathlib import Path

import matplotlib
matplotlib.use("Agg")                     # pas de fenêtre : on enregistre des images
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

RACINE = Path(__file__).resolve().parent.parent
SORTIE = Path(__file__).resolve().parent / "resultats"

FENETRE  = 30          # 30 mesures = 1 minute (une mesure toutes les 2 s)
Z_MAX    = 3.0         # au-delà de 3 écarts-types : anomalie
ECART_MIN = {"temperature": 0.5, "vibration": 0.02}   # évite de diviser par ~0 quand le signal est plat
TENDANCE_MIN = 30      # tendance calculée sur les 30 dernières minutes

NOMS = {"temperature": "Température", "vibration": "Vibration"}
TROU_FACTEUR = 10      # un écart 10 fois plus long que d'habitude = simulation arrêtée : on coupe la courbe

SEUILS = {             # mêmes valeurs que l'ESP32 et Node-RED
    "temperature": {"unite": "°C", "alerte": 75.0, "critique": 85.0},
    "vibration":   {"unite": "g",  "alerte": 0.5,  "critique": 1.0},
}


def charger(fichier):
    d = pd.read_csv(fichier, encoding="utf-8-sig")
    d["ts"] = pd.to_datetime(d["ts"], utc=True, errors="coerce")
    d["valeur"] = pd.to_numeric(d["valeur"], errors="coerce")
    nb_total = len(d)
    d = d[(d["qualite"] == "ok") & d["ts"].notna() & d["valeur"].notna()]
    return d.sort_values("ts"), nb_total - len(d)


def donnees_demo():
    """Données SIMULÉES pour illustrer la maintenance prédictive : 7 jours, une mesure toutes les 5 min.
    Le roulement s'use : la température monte lentement (55 → 68 °C) et la vibration aussi (0,15 → 0,38 g),
    avec du bruit et deux pics isolés (parasites)."""
    rng = np.random.default_rng(42)
    ts = pd.date_range("2026-11-01", periods=7 * 24 * 12, freq="5min", tz="UTC")
    k = np.linspace(0, 1, len(ts)) ** 1.6                       # usure qui s'accélère
    temp = 55 + 13 * k + rng.normal(0, 0.6, len(ts))
    vib = 0.15 + 0.23 * k + rng.normal(0, 0.015, len(ts))
    temp[600] += 9; vib[1400] += 0.25                           # deux parasites
    lignes = [pd.DataFrame({"ts": ts, "grandeur": g, "valeur": v, "qualite": "ok"})
              for g, v in (("temperature", temp), ("vibration", vib))]
    return pd.concat(lignes)


def couper_trous(serie):
    """Insère un trou (NaN) là où la simulation était arrêtée, pour ne pas relier deux séances par une droite."""
    ecarts = serie.index.to_series().diff().dt.total_seconds()
    trous = ecarts > TROU_FACTEUR * ecarts.median()
    if not trous.any():
        return serie
    nan = pd.Series(np.nan, index=serie.index[trous] - pd.Timedelta(seconds=1))
    return pd.concat([serie, nan]).sort_index()


def analyser(d, grandeur):
    s = SEUILS[grandeur]
    x = d[d["grandeur"] == grandeur].set_index("ts")["valeur"]

    # Moyenne et écart-type de la minute PRÉCÉDENTE (shift) : la mesure testée ne fausse pas sa propre référence
    moyenne = x.rolling(FENETRE, min_periods=FENETRE).mean().shift(1)
    ecart = x.rolling(FENETRE, min_periods=FENETRE).std().shift(1).clip(lower=ECART_MIN[grandeur])
    z = (x - moyenne) / ecart
    anomalies = x[z.abs() > Z_MAX]

    # Tendance sur les 30 dernières minutes : droite des moindres carrés
    recent = x[x.index >= x.index.max() - pd.Timedelta(minutes=TENDANCE_MIN)]
    heures = (recent.index - recent.index[0]).total_seconds() / 3600
    pente, origine = np.polyfit(heures, recent.values, 1) if len(recent) > 10 else (0.0, recent.mean())
    actuelle = recent.tail(5).mean()                            # valeur actuelle = moyenne des 5 dernières mesures

    def temps_avant(seuil):
        if actuelle >= seuil:
            return "déjà atteint"
        if pente <= 0:
            return "jamais (pas de montée)"
        h = (seuil - actuelle) / pente
        return f"environ {h:.1f} h" if h < 48 else f"environ {h / 24:.0f} jours"

    resume = {
        "grandeur": grandeur,
        "nb": len(x), "debut": x.index.min(), "fin": x.index.max(),
        "moyenne": x.mean(), "min": x.min(), "max": x.max(),
        "nb_anomalies": len(anomalies),
        "pente": pente, "actuelle": actuelle,
        "avant_alerte": temps_avant(s["alerte"]),
        "avant_critique": temps_avant(s["critique"]),
    }
    return x, moyenne, ecart, anomalies, (recent.index, origine + pente * heures), resume


def tracer(grandeur, x, moyenne, ecart, anomalies, droite, fichier):
    s = SEUILS[grandeur]
    fig, ax = plt.subplots(figsize=(12, 4.8))
    xc, mc = couper_trous(x), couper_trous(moyenne)
    ax.plot(xc.index, xc.values, lw=0.8, color="#64748b", label="mesure")
    ax.plot(mc.index, mc.values, lw=1.6, color="#2563eb", label=f"moyenne mobile ({FENETRE} mesures)")
    bas_z, haut_z = couper_trous(moyenne - Z_MAX * ecart), couper_trous(moyenne + Z_MAX * ecart)
    ax.fill_between(bas_z.index, bas_z.values, haut_z.values,
                    color="#2563eb", alpha=0.12, label="zone normale (±3 écarts-types)")
    ax.scatter(anomalies.index, anomalies.values, color="#dc2626", s=26, zorder=3,
               label=f"anomalies ({len(anomalies)})")
    ax.plot(droite[0], droite[1], "--", color="#7c3aed", lw=2, label=f"tendance (sur {TENDANCE_MIN} min)")
    ax.axhline(s["alerte"], color="#f59e0b", ls=":", lw=1.5, label=f"seuil d'alerte {s['alerte']} {s['unite']}")
    ax.axhline(s["critique"], color="#dc2626", ls=":", lw=1.5, label=f"seuil critique {s['critique']} {s['unite']}")
    ax.set_title(f"{NOMS[grandeur]} du palier — détection d'anomalies (z-score)")
    ax.set_ylabel(s["unite"])
    bas, haut = np.nanmin(x.values), np.nanmax(x.values)
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

    global FENETRE, TENDANCE_MIN
    if args.demo:
        d, nb_ignorees, nom = donnees_demo(), 0, "DONNÉES SIMULÉES (démonstration d'usure lente sur 7 jours)"
        FENETRE, TENDANCE_MIN = 36, 24 * 60                     # fenêtre 3 h, tendance sur le dernier jour
        sortie = SORTIE / "demo"
    else:
        d, nb_ignorees = charger(args.fichier)
        nom, sortie = args.fichier.name, SORTIE
        if args.depuis:
            d = d[d["ts"] >= d["ts"].max() - pd.Timedelta(minutes=args.depuis)]
            nom += f" (dernières {args.depuis} min)"
    sortie.mkdir(parents=True, exist_ok=True)
    lignes = [f"Analyse de {nom}",
              f"Mesures en erreur ou incomplètes ignorées : {nb_ignorees}", ""]

    for g in SEUILS:
        x, moyenne, ecart, anomalies, droite, r = analyser(d, g)
        tracer(g, x, moyenne, ecart, anomalies, droite, sortie / f"{g}.png")
        u = SEUILS[g]["unite"]
        lignes += [
            f"== {g.upper()} ==",
            f"Mesures valides : {r['nb']}  ({r['debut']:%d/%m %H:%M} → {r['fin']:%d/%m %H:%M} UTC)",
            f"Moyenne {r['moyenne']:.2f} {u} · min {r['min']:.2f} · max {r['max']:.2f}",
            f"Anomalies (|z| > {Z_MAX:g}) : {r['nb_anomalies']}",
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
