#!/usr/bin/env python3
"""Собирает тестовый набор из LibriSpeech dev-clean.

Три категории по N файлов (16 кГц, моно, WAV):
  speech_clean/  — чистая речь;
  speech_noisy/  — другая речь, смешанная с розовым шумом;
  noise_only/    — только розовый шум той же длины.

Выбор файлов детерминирован (сортировка путей), шум генерируется с
фиксированным seed, поэтому набор воспроизводится на любой машине.
Нужны ffmpeg и ffprobe.
"""
import argparse
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

URL = "https://www.openslr.org/resources/12/dev-clean.tar.gz"
HERE = Path(__file__).resolve().parent


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def duration_sec(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", str(path)],
        check=True, capture_output=True, text=True)
    return float(out.stdout.strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data-dir", type=Path, default=HERE / "data", help="куда скачать LibriSpeech")
    ap.add_argument("--out", type=Path, default=HERE / "clips", help="куда сложить клипы")
    ap.add_argument("-n", type=int, default=10, help="файлов на категорию")
    args = ap.parse_args()

    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            sys.exit(f"не найден {tool}: sudo apt install ffmpeg")

    args.data_dir.mkdir(parents=True, exist_ok=True)
    tar_path = args.data_dir / "dev-clean.tar.gz"
    if not tar_path.exists():
        print(f"Скачиваю {URL} (~340 МБ)...")
        urllib.request.urlretrieve(URL, tar_path)
    if not (args.data_dir / "LibriSpeech").exists():
        print("Распаковываю...")
        with tarfile.open(tar_path) as tar:
            tar.extractall(args.data_dir)

    flacs = sorted((args.data_dir / "LibriSpeech").rglob("*.flac"))
    if len(flacs) < 2 * args.n:
        sys.exit("не найдены файлы .flac — проверьте распаковку")

    for cat in ("speech_clean", "speech_noisy", "noise_only"):
        (args.out / cat).mkdir(parents=True, exist_ok=True)

    for i in range(args.n):
        clean_src = flacs[i]
        noisy_src = flacs[i + args.n]

        run(["ffmpeg", "-y", "-i", str(clean_src), "-ar", "16000", "-ac", "1",
             str(args.out / "speech_clean" / f"clean_{i}.wav")])

        dur = duration_sec(clean_src)
        run(["ffmpeg", "-y", "-f", "lavfi",
             "-i", f"anoisesrc=color=pink:r=16000:a=0.1:seed={1000 + i}", "-t", f"{dur:.3f}",
             "-ac", "1", str(args.out / "noise_only" / f"noise_{i}.wav")])

        run(["ffmpeg", "-y", "-i", str(noisy_src), "-f", "lavfi",
             "-i", f"anoisesrc=color=pink:r=16000:a=0.05:seed={2000 + i}",
             "-filter_complex", "amix=inputs=2:duration=first:dropout_transition=2",
             "-ar", "16000", "-ac", "1", str(args.out / "speech_noisy" / f"noisy_{i}.wav")])
        print(f"  [{i + 1}/{args.n}] {clean_src.name}, {noisy_src.name}")

    print(f"Готово: {args.out}")


if __name__ == "__main__":
    main()
