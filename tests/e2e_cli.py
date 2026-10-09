#!/usr/bin/env python3
"""Сквозной тест: WAV-файл → playbin → yadrovad → статистика.

Генерирует тот же сигнал, что и test_yadrovad.cpp (тишина + три «фразы»),
записывает его в WAV и прогоняет через yadrovad_cli. Так проверяется
реальный конвейер с wavparse и декодером, а не только элемент в изоляции.

    e2e_cli.py ПУТЬ_К_yadrovad_cli
"""
import json
import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

RATE = 16000


def voice(sec):
    out = []
    for i in range(int(sec * RATE)):
        t = i / RATE
        v = 0.0
        for k in range(1, 26):
            f = k * 140.0
            formants = (math.exp(-((f - 700) / 300) ** 2) + 0.6 * math.exp(-((f - 1200) / 400) ** 2)
                        + 0.3 * math.exp(-((f - 2500) / 500) ** 2))
            v += formants * math.sin(2 * math.pi * f * t + k)
        out.append(int(round(4000 * (0.6 + 0.4 * math.sin(2 * math.pi * 4 * t)) * v)))
    return out


def three_phrases():
    s = [0] * int(0.6 * RATE)
    for i in range(3):
        s += voice(0.9)
        s += [0] * int((1.5 if i < 2 else 1.0) * RATE)
    return s


def main():
    cli = sys.argv[1]
    samples = three_phrases()
    with tempfile.TemporaryDirectory() as tmp:
        # Пробел и кавычка в имени: путь не должен ломать конвейер
        path = Path(tmp) / 'test "phrases".wav'
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
        out = subprocess.run([cli, "--null", "--json", str(path)],
                             capture_output=True, text=True)
    if out.returncode != 0:
        print(f"yadrovad_cli failed with code {out.returncode}:\n{out.stderr}", file=sys.stderr)
        sys.exit(1)
    stats = json.loads(out.stdout)
    mask = stats["mask"]
    expected = len(samples) * 2 // 960
    phrases = mask.count("01") + (1 if mask.startswith("1") else 0)
    print(f"frames {stats['total_frames']} (expected {expected}), kept {stats['frames_kept']}, phrases {phrases}")
    ok = (stats["total_frames"] == expected and len(mask) == expected and phrases == 3
          and stats["frames_kept"] + stats["frames_dropped"] == expected)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
