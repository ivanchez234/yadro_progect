#!/usr/bin/env python3
"""Маски решений плагина yadrovad для каждого клипа.

Запускает yadrovad_cli --null --json (тот же конвейер, что в плеере:
playbin → audioconvert → audioresample → yadrovad) и сохраняет маску
'1' = кадр оставлен, '0' = вырезан. По умолчанию hangover выключен,
чтобы сравнивать с эталоном само решение VAD, а не удержание.
"""
import argparse
import json
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clips", type=Path, default=HERE / "clips")
    ap.add_argument("--cli", type=Path, default=ROOT / "build" / "tools" / "yadrovad_cli",
                    help="путь к yadrovad_cli")
    ap.add_argument("--vad-mode", type=int, nargs="+", default=[0, 1, 2, 3])
    ap.add_argument("--hangover", type=int, default=0, help="мс, по умолчанию 0")
    ap.add_argument("--out-dir", type=Path, default=HERE / "results")
    args = ap.parse_args()

    if not args.cli.exists():
        raise SystemExit(f"не найден {args.cli}: соберите проект (cmake --build build)")

    files = sorted(args.clips.rglob("*.wav"))
    if not files:
        raise SystemExit(f"нет WAV в {args.clips}: сначала запустите build_dataset.py")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for mode in args.vad_mode:
        result = {}
        for path in files:
            out = subprocess.run(
                [str(args.cli), "--null", "--json", "--vad-mode", str(mode),
                 "--hangover", str(args.hangover), str(path)],
                check=True, capture_output=True, text=True)
            stats = json.loads(out.stdout)
            result[path.relative_to(args.clips).as_posix()] = {"mask": stats["mask"]}
        dst = args.out_dir / f"plugin_mode{mode}_h{args.hangover}.json"
        dst.write_text(json.dumps(result, indent=1))
        print(f"vad-mode {mode}: {len(result)} файлов → {dst}")


if __name__ == "__main__":
    main()
