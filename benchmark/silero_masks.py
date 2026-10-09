#!/usr/bin/env python3
"""Эталонная разметка: Silero VAD → маска речи по кадрам 30 мс.

Silero — нейросетевой VAD, заметно точнее WebRTC VAD, поэтому его решения
берутся за эталон. Это не ручная разметка: итоговая метрика — согласие
плагина с Silero, а не «абсолютная точность».

Нужны torch и soundfile (см. requirements.txt). Модель скачивается через
torch.hub при первом запуске.
"""
import argparse
import json
import math
from pathlib import Path

import soundfile as sf
import torch

HERE = Path(__file__).resolve().parent
RATE = 16000
FRAME_MS = 30


def timestamps_to_mask(timestamps, n_samples):
    frame = RATE * FRAME_MS // 1000
    n_frames = n_samples // frame  # как в плагине: только целые кадры
    mask = ["0"] * n_frames
    for seg in timestamps:
        first = seg["start"] // frame
        last = math.ceil(seg["end"] / frame)
        for i in range(first, min(last, n_frames)):
            mask[i] = "1"
    return "".join(mask)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clips", type=Path, default=HERE / "clips")
    ap.add_argument("--out", type=Path, default=HERE / "results" / "silero.json")
    args = ap.parse_args()

    model, utils = torch.hub.load("snakers4/silero-vad", "silero_vad", trust_repo=True)
    get_speech_timestamps = utils[0]

    files = sorted(p for p in args.clips.rglob("*.wav"))
    if not files:
        raise SystemExit(f"нет WAV в {args.clips}: сначала запустите build_dataset.py")

    result = {}
    for path in files:
        audio, rate = sf.read(path, dtype="float32")
        if rate != RATE or audio.ndim != 1:
            raise SystemExit(f"{path}: ожидается 16 кГц моно")
        ts = get_speech_timestamps(torch.from_numpy(audio), model, sampling_rate=RATE)
        mask = timestamps_to_mask(ts, len(audio))
        key = path.relative_to(args.clips).as_posix()
        result[key] = {"mask": mask}
        print(f"  {key:<28} кадров {len(mask):>5}, речь {mask.count('1'):>5}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=1))
    print(f"Сохранено: {args.out}")


if __name__ == "__main__":
    main()
