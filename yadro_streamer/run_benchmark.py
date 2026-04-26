import os
import json
import subprocess
import re

# === НАСТРОЙКИ ===
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CLIPS_DIR = os.path.join(BASE_DIR, "benchmark", "benchmark_clips")
PLUGIN_DIR = os.path.join(BASE_DIR, "build") # Путь к твоей скомпилированной библиотеке
OUTPUT_JSON = os.path.join(BASE_DIR, "plugin_masks.json")

def run_plugin_benchmark():
    print("🚀 Запуск GStreamer плагина (Сбор масок)...")
    
    plugin_data = {}
    valid_extensions = ('.wav', '.mp3')
    
    # Настраиваем окружение для GStreamer
    env = os.environ.copy()
    env["GST_PLUGIN_PATH"] = PLUGIN_DIR
    env["GST_DEBUG"] = "yadrovad:4" # Включаем уровень INFO, чтобы перехватить вывод маски
    
    files_to_process = []
    
    # Рекурсивный поиск файлов (чтобы ключи совпадали с эталонным JSON)
    for root, dirs, files in os.walk(CLIPS_DIR):
        for file in files:
            if file.lower().endswith(valid_extensions):
                full_path = os.path.join(root, file)
                rel_path = os.path.relpath(full_path, CLIPS_DIR)
                files_to_process.append((full_path, rel_path))

    print(f"📁 Найдено файлов для обработки: {len(files_to_process)}")

    for full_path, rel_path in files_to_process:
        # Собираем пайплайн для терминала
        cmd = f'gst-launch-1.0 filesrc location="{full_path}" ! decodebin ! audioconvert ! audioresample ! yadrovad ! fakesink'
        
        # Запускаем как отдельный процесс
        result = subprocess.run(cmd, shell=True, env=env, capture_output=True, text=True)
        
        # Ищем нашу строку с телеметрией через регулярное выражение
        mask_match = re.search(r"Mask:\s*([01]+)", result.stdout + result.stderr)
        
        if mask_match:
            mask = mask_match.group(1)
            plugin_data[rel_path] = {
                "mask": mask,
                "total_frames": len(mask),
                "speech_frames": mask.count('1')
            }
            print(f"✅ Обработан: {rel_path:<25} | Кадров: {len(mask):<5}")
        else:
            print(f"⚠️ Ошибка: Телеметрия не найдена для {rel_path}!")
            plugin_data[rel_path] = {"mask": "", "total_frames": 0, "speech_frames": 0}

    # Сохраняем результаты работы плагина
    with open(OUTPUT_JSON, 'w', encoding='utf-8') as f:
        json.dump(plugin_data, f, indent=4)
        
    print(f"\n🎉 Сбор масок завершен! Результат сохранен в {OUTPUT_JSON}")

if __name__ == "__main__":
    run_plugin_benchmark()