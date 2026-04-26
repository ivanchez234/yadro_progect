import os
import csv
import subprocess
import re

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(BASE_DIR, "benchmark_labels.csv")
PLUGIN_DIR = os.path.join(BASE_DIR, "../build") # Путь к твоей папке сборки VS Code

def run_test():
    print("🚀 Запускаем точечный процентный анализ VAD...\n")
    
    ground_truth = {}
    with open(CSV_PATH, 'r') as f:
        reader = csv.reader(f)
        next(reader) 
        for row in reader:
            filepath, label = row[0], row[1]
            ground_truth[filepath] = label

    correct_predictions = 0
    total_files = len(ground_truth)

    env = os.environ.copy()
    env["GST_PLUGIN_PATH"] = PLUGIN_DIR
    env["GST_DEBUG"] = "yadrovad:4" 

    for rel_path, true_label in ground_truth.items():
        abs_path = os.path.join(BASE_DIR, "benchmark_clips", rel_path)
        
        cmd = f'gst-launch-1.0 filesrc location="{abs_path}" ! decodebin ! audioconvert ! audioresample ! yadrovad ! fakesink'
        
        result = subprocess.run(cmd, shell=True, env=env, capture_output=True, text=True)
        
        mask_match = re.search(r"Mask:\s*([01]+)", result.stdout + result.stderr)
        
        if mask_match:
            mask = mask_match.group(1)
            total_frames = len(mask)
            ones_count = mask.count('1')
            
            # Считаем, какой процент файла плагин распознал как речь
            speech_percentage = (ones_count / total_frames) * 100 if total_frames > 0 else 0
            
            # НОВАЯ ЛОГИКА: Если речи больше 15% от длины файла - это голосовой файл.
            # Если меньше 15% - это просто шум с редкими ложными срабатываниями.
            predicted_label = "speech" if speech_percentage > 15.0 else "noise"
            
            is_correct = (predicted_label == true_label)
            if is_correct:
                correct_predictions += 1
                status = "✅ ПАС "
            else:
                status = "❌ ФЕЙЛ"

            print(f"{status} | Файл: {os.path.basename(rel_path):<12} | Найдено речи: {speech_percentage:>5.1f}% | Ожидали: {true_label:<6}")
        else:
            print(f"⚠️ Ошибка: Телеметрия не найдена для {rel_path}!")

    accuracy = (correct_predictions / total_files) * 100
    
    print("\n" + "="*45)
    print("📊 ИТОГИ БЕНЧМАРКА (с учетом порогов):")
    print(f"Обработано файлов: {total_files}")
    print(f"Итоговая Точность (Accuracy): {accuracy:.2f}%")
    
    if accuracy >= 94.0:
        print("🏆 ЦЕЛЬ ДОСТИГНУТА! Точность >= 94%. Высший пилотаж!")
    else:
        print(f"⚠️ Недотянули. До 94% осталось {94.0 - accuracy:.2f}%")
    print("="*45)

if __name__ == "__main__":
    run_test()