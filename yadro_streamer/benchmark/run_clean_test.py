import os
import subprocess
import re

# Настраиваем пути
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DIR_CLEAN = os.path.join(BASE_DIR, "speech_clean")

# Точный путь к твоему плагину на Ubuntu
GST_PLUGIN_PATH = "/home/ivanchez/Рабочий стол/YADRO_progect/yadro_streamer/build"

def process_file(filepath):
    filename = os.path.basename(filepath)
    # Создаем папку, если её нет
    os.makedirs(os.path.join(BASE_DIR, "processed_results"), exist_ok=True)
    
    out_path = os.path.join(BASE_DIR, "processed_results", "processed_" + filename + ".wav")
    
    # Важно: берем путь в кавычки для GStreamer внутри location
    # Важно: "filesink" и "location=..." теперь разделены запятой!
    cmd = [
        "gst-launch-1.0", "-q",
        "filesrc", f"location={filepath}", "!",
        "decodebin", "!", "audioconvert", "!", "audioresample", "!",
        "yadrovad", "vad-mode=3", "hangover-time=60", "!", 
        "audioconvert", "!", "wavenc", "!", 
        "filesink", f"location={out_path}"
    ]
    
    env = os.environ.copy()
    env["GST_PLUGIN_PATH"] = GST_PLUGIN_PATH
    env["GST_DEBUG"] = "yadrovad:4"

    result = subprocess.run(cmd, env=env, capture_output=True, text=True)

    # Если регулярка не сработала, давай посмотрим, что сказал GStreamer
    match = re.search(r"Telemetry sent! Total: (\d+), Kept: (\d+), Dropped: (\d+)", result.stderr)
    
    if not match:
        print(f"\n❌ ОШИБКА GStreamer на файле {filename}:")
        # Печатаем последние 3 строки ошибки для диагностики
        print("\n".join(result.stderr.splitlines()[-5:])) 
        return 0, 0, 0
        
    return int(match.group(1)), int(match.group(2)), int(match.group(3))

def run_basic_benchmark():
    print("🚀 Запуск базового теста VAD (Чистая речь)...\n")
    
    total_passed = 0
    total_files = 0

    if not os.path.exists(DIR_CLEAN) or not os.listdir(DIR_CLEAN):
        print(f"❌ Ошибка: Папка {DIR_CLEAN} не существует или пуста.")
        return

    for filename in os.listdir(DIR_CLEAN):
        if filename.endswith(('.wav', '.mp3', '.flac')):
            total_files += 1
            path = os.path.join(DIR_CLEAN, filename)
            
            # Прогоняем файл
            total, kept, dropped = process_file(path)
            
            if total > 0:
                kept_percent = (kept / total) * 100.0
                
                # Допуск: в нормальной речи 5-30% времени занимают паузы (вдохи, остановки)
                # Если VAD оставил от 70% до 95% кадров - он сработал отлично.
                if 65.0 <= kept_percent <= 95.0:
                    print(f"  [✅ PASS] {filename}: Сохранено {kept_percent:.1f}% (паузы успешно вырезаны)")
                    total_passed += 1
                else:
                    print(f"  [❌ FAIL] {filename}: Сохранено {kept_percent:.1f}% (Аномалия: либо вырезал слова, либо оставил лишнее)")
            else:
                print(f"  [⚠️ WARN] {filename}: Плагин не вернул телеметрию.")

    if total_files > 0:
        accuracy = (total_passed / total_files) * 100.0
        print("\n========================================")
        print("📊 ИТОГИ ТЕСТА (ЧИСТАЯ РЕЧЬ)")
        print("========================================")
        print(f"Проверено файлов: {total_files}")
        print(f"Успешно (Pass):   {total_passed}")
        print(f"Точность:         {accuracy:.1f}%")
        print("========================================")

if __name__ == "__main__":
    run_basic_benchmark()
