import os
import json
import math
import torch
import torchaudio

# === НАСТРОЙКИ ===
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
# Указываем точный путь до папки с тремя подпапками
CLIPS_DIR = os.path.join(BASE_DIR, "benchmark", "benchmark_clips")
OUTPUT_JSON = os.path.join(BASE_DIR, "ground_truth_masks.json")

CHUNK_STEP_MS = 30 
CHUNK_STEP_SEC = CHUNK_STEP_MS / 1000.0

def convert_timestamps_to_mask(timestamps, total_samples, sample_rate):
    total_duration_sec = total_samples / sample_rate
    total_frames = math.ceil(total_duration_sec / CHUNK_STEP_SEC)
    mask = ['0'] * total_frames
    
    for segment in timestamps:
        start_sec = segment['start'] / sample_rate
        end_sec = segment['end'] / sample_rate
        
        start_frame = math.floor(start_sec / CHUNK_STEP_SEC)
        end_frame = math.ceil(end_sec / CHUNK_STEP_SEC)
        
        for i in range(start_frame, min(end_frame, total_frames)):
            mask[i] = '1'
            
    return "".join(mask)

def main():
    print("🚀 Загрузка модели Silero VAD...")
    model, utils = torch.hub.load(repo_or_dir='snakers4/silero-vad',
                                  model='silero_vad',
                                  force_reload=False)
    
    (get_speech_timestamps, _, read_audio, _, _) = utils
    ground_truth_data = {}
    valid_extensions = ('.wav', '.mp3')
    
    files_to_process = []
    
    # === НОВОЕ: Рекурсивный поиск файлов по всем подпапкам ===
    for root, dirs, files in os.walk(CLIPS_DIR):
        for file in files:
            if file.lower().endswith(valid_extensions):
                full_path = os.path.join(root, file)
                # Получаем относительный путь (например, "noise_only/noise_1.wav")
                rel_path = os.path.relpath(full_path, CLIPS_DIR)
                files_to_process.append((full_path, rel_path))
                
    print(f"📁 Найдено файлов для разметки: {len(files_to_process)}")
    
    for full_path, rel_path in files_to_process:
        try:
            wav = read_audio(full_path, sampling_rate=16000)
        except Exception as e:
            print(f"⚠️ Ошибка чтения файла {rel_path}: {e}")
            continue
            
        total_samples = wav.shape[0]
        speech_timestamps = get_speech_timestamps(wav, model, sampling_rate=16000)
        mask_string = convert_timestamps_to_mask(speech_timestamps, total_samples, 16000)
        
        # Записываем в JSON, используя относительный путь как ключ
        ground_truth_data[rel_path] = {
            "mask": mask_string,
            "total_frames": len(mask_string),
            "speech_frames": mask_string.count('1')
        }
        print(f"✅ Обработан: {rel_path:<25} | Кадров: {len(mask_string):<5} | Речь: {mask_string.count('1')}")
        
    with open(OUTPUT_JSON, 'w', encoding='utf-8') as f:
        json.dump(ground_truth_data, f, indent=4)
        
    print(f"\n🎉 Разметка завершена! Результат сохранен в {OUTPUT_JSON}")

if __name__ == "__main__":
    main()