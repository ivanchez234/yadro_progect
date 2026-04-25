import os
import subprocess
import csv
import glob

# Настройки путей
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(BASE_DIR, "official_data")
OUT_DIR = os.path.join(BASE_DIR, "benchmark_clips")
CSV_PATH = os.path.join(BASE_DIR, "benchmark_labels.csv")

# Категории
CATEGORIES = ["speech_clean", "speech_noisy", "noise_only"]
for cat in CATEGORIES:
    os.makedirs(os.path.join(OUT_DIR, cat), exist_ok=True)

os.makedirs(DATA_DIR, exist_ok=True)

def build_dataset():
    tar_path = os.path.join(DATA_DIR, "dev-clean.tar.gz")
    url = "http://www.openslr.org/resources/12/dev-clean.tar.gz"

    # 1. Скачиваем официальный LibriSpeech (337 МБ)
    if not os.path.exists(tar_path):
        print(f"⬇️ Качаем эталонную базу LibriSpeech (~330 МБ)...")
        subprocess.run(f'wget -c {url} -O "{tar_path}"', shell=True)
        print("📦 Распаковываем архив...")
        subprocess.run(f'tar -xzf "{tar_path}" -C "{DATA_DIR}"', shell=True)
    else:
        print("✅ Архив LibriSpeech уже скачан. Идем дальше.")

    # 2. Ищем все аудиофайлы (.flac)
    flac_files = glob.glob(os.path.join(DATA_DIR, "LibriSpeech", "**", "*.flac"), recursive=True)
    if not flac_files:
        print("❌ Ошибка: файлы .flac не найдены. Проверьте распаковку.")
        return

    csv_data = [["filename", "label"]]
    
    print("🛠 Начинаем сборку датасета (по 10 файлов на категорию)...")

    for i in range(10):
        clean_flac = flac_files[i]
        
        # Узнаем длину файла
        res = subprocess.run(f'ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "{clean_flac}"', shell=True, capture_output=True, text=True)
        duration = float(res.stdout.strip())
        
        # --- A. ЧИСТАЯ РЕЧЬ (speech_clean) ---
        clean_wav = os.path.join(OUT_DIR, "speech_clean", f"clean_{i}.wav")
        subprocess.run(f'ffmpeg -y -i "{clean_flac}" -ar 16000 -ac 1 "{clean_wav}" -loglevel quiet', shell=True)
        csv_data.append([f"speech_clean/clean_{i}.wav", "speech"])

        # --- B. ЧИСТЫЙ ШУМ (noise_only) ---
        # Генерируем "розовый шум" (pink noise) - он лучше всего имитирует гул улицы/офиса
        noise_wav = os.path.join(OUT_DIR, "noise_only", f"noise_{i}.wav")
        subprocess.run(f'ffmpeg -y -f lavfi -i anoisesrc=color=pink:r=16000:a=0.1 -t {duration} "{noise_wav}" -loglevel quiet', shell=True)
        csv_data.append([f"noise_only/noise_{i}.wav", "noise"])

        # --- C. РЕЧЬ С ШУМОМ (speech_noisy) ---
        # Смешиваем чистую речь из другого файла с шумом
        noisy_flac = flac_files[i + 10] # берем следующие файлы
        mixed_wav = os.path.join(OUT_DIR, "speech_noisy", f"noisy_{i}.wav")
        
        # Фильтр amix смешивает 2 потока
        mix_cmd = f'ffmpeg -y -i "{noisy_flac}" -f lavfi -i anoisesrc=color=pink:r=16000:a=0.05 -filter_complex amix=inputs=2:duration=first:dropout_transition=2 -ar 16000 -ac 1 "{mixed_wav}" -loglevel quiet'
        subprocess.run(mix_cmd, shell=True)
        csv_data.append([f"speech_noisy/noisy_{i}.wav", "speech"]) # Для VAD это все равно речь!

    # 3. Сохраняем разметку
    with open(CSV_PATH, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerows(csv_data)

    print(f"\n🎉 Готово! Датасет собран.")
    print(f"📁 Файлы лежат в: {OUT_DIR}")
    print(f"📄 Файл разметки: {CSV_PATH}")

if __name__ == "__main__":
    build_dataset()