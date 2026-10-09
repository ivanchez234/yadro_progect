#!/usr/bin/env python3
"""Сравнивает маски плагина с эталоном Silero покадрово.

Для каждой категории и в целом считает:
  agreement  — доля кадров, где решения совпали ((TP+TN)/N);
  recall     — доля кадров речи (по Silero), которые плагин оставил;
               пропущенная речь = «съеденные» слова;
  precision  — доля оставленных кадров, которые действительно речь;
  removed    — доля вырезанных кадров (сколько времени экономится).

Печатает таблицу в Markdown — её можно вставить в README.
"""
import argparse
import json
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent


def confusion(ref, hyp):
    n = min(len(ref), len(hyp))
    c = {"tp": 0, "tn": 0, "fp": 0, "fn": 0}
    for r, h in zip(ref[:n], hyp[:n]):
        if r == "1":
            c["tp" if h == "1" else "fn"] += 1
        else:
            c["fp" if h == "1" else "tn"] += 1
    return c


def metrics(c):
    n = sum(c.values())
    pct = lambda a, b: 100.0 * a / b if b else float("nan")
    return {
        "agreement": pct(c["tp"] + c["tn"], n),
        "recall": pct(c["tp"], c["tp"] + c["fn"]),
        "precision": pct(c["tp"], c["tp"] + c["fp"]),
        "removed": pct(c["tn"] + c["fn"], n),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reference", type=Path, default=HERE / "results" / "silero.json")
    ap.add_argument("plugin", type=Path, nargs="*",
                    help="файлы plugin_mode*.json (по умолчанию все из results/)")
    args = ap.parse_args()

    ref = json.loads(args.reference.read_text())
    plugin_files = args.plugin or sorted((HERE / "results").glob("plugin_mode*.json"))
    if not plugin_files:
        raise SystemExit("нет масок плагина: сначала запустите plugin_masks.py")

    print("| Прогон | Категория | Совпадение | Recall речи | Precision | Вырезано |")
    print("|---|---|---:|---:|---:|---:|")
    for pf in plugin_files:
        hyp = json.loads(pf.read_text())
        per_cat = defaultdict(lambda: {"tp": 0, "tn": 0, "fp": 0, "fn": 0})
        for name, r in ref.items():
            if name not in hyp:
                print(f"<!-- нет {name} в {pf.name} -->")
                continue
            c = confusion(r["mask"], hyp[name]["mask"])
            for key in ("tp", "tn", "fp", "fn"):
                per_cat[name.split("/")[0]][key] += c[key]
                per_cat["всего"][key] += c[key]
        for cat in sorted(per_cat, key=lambda k: (k == "всего", k)):
            m = metrics(per_cat[cat])
            fmt = lambda v: "—" if v != v else f"{v:.1f}%"
            print(f"| {pf.stem} | {cat} | {fmt(m['agreement'])} | {fmt(m['recall'])} | "
                  f"{fmt(m['precision'])} | {fmt(m['removed'])} |")


if __name__ == "__main__":
    main()
