import os
import subprocess
import re
import csv

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
GST_PLUGIN_PATH = "/home/ivanchez/Рабочий стол/YADRO_progect/yadro_streamer/build"

# Длительность одного кадра в секундах (т.к. мы берем 960 байт при 16000 Гц = 30 мс)
FRAME_SEC = 0.03 

def get_plugin_mask(filepath):
    """Прогоняет аудио через плагин и возвращает битовую маску (строку 0 и 1)"""
    cmd = [
        "gst-launch-1.0", "-q",
        "filesrc", f"location={filepath}", "!",
        "decodebin", "!", "audioconvert", "!", "audioresample", "!",
        "yadrovad", "vad-mode=3", "hangover-time=60", "!", 
        "fakesink"
    ]
    env = os.environ.copy()
    env["GST_PLUGIN_PATH"] = GST_PLUGIN_PATH
    env["GST_DEBUG"] = "yadrovad:4"

    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    
    # Ищем новую маску в логах: Mask: 00011100...
    match = re.search(r"Mask: ([01]+)", result.stderr)
    if match:
        return match.group(1)
    else:
        print("❌ Ошибка: Плагин не вернул маску. Лог GStreamer:")
        print(result.stderr)
        return ""

def load_ground_truth(csv_path, total_frames):
    """Читает CSV и генерирует идеальную маску из 0 и 1"""
    truth_mask = ['0'] * total_frames # По умолчанию всё тишина
    
    try:
        with open(csv_path, 'r') as file:
            reader = csv.DictReader(file)
            for row in reader:
                start = float(row['start_time'])
                end = float(row['end_time'])
                label = row['label'].strip().lower()
                
                if label == 'speech':
                    # Переводим секунды в индексы кадров
                    start_frame = int(start / FRAME_SEC)
                    end_frame = int(end / FRAME_SEC)
                    
                    # Заполняем этот отрезок единицами
                    for i in range(start_frame, min(end_frame, total_frames)):
                        truth_mask[i] = '1'
    except Exception as e:
        print(f"❌ Ошибка чтения CSV: {e}")
        
    return "".join(truth_mask)

def calculate_metrics(plugin_mask, truth_mask):
    """Математическое сравнение двух масок кадр-в-кадр"""
    TP, TN, FP, FN = 0, 0, 0, 0
    
    for p, t in zip(plugin_mask, truth_mask):
        if p == '1' and t == '1': TP += 1
        elif p == '0' and t == '0': TN += 1
        elif p == '1' and t == '0': FP += 1 # Ложное срабатывание (оставил шум)
        elif p == '0' and t == '1': FN += 1 # Пропуск (вырезал голос)
            
    return TP, TN, FP, FN

def run_test(audio_path, csv_path):
    print(f"🚀 Запуск покадрового анализа: {os.path.basename(audio_path)}\n")
    
    # 1. Получаем маску от GStreamer
    plugin_mask = get_plugin_mask(audio_path)
    if not plugin_mask:
        return
        
    total_frames = len(plugin_mask)
    
    # 2. Генерируем идеальную маску из CSV
    truth_mask = load_ground_truth(csv_path, total_frames)
    
    # 3. Считаем матрицу ошибок
    TP, TN, FP, FN = calculate_metrics(plugin_mask, truth_mask)
    
    # 4. Высчитываем ML метрики
    accuracy = (TP + TN) / total_frames if total_frames else 0
    precision = TP / (TP + FP) if (TP + FP) else 0 # Насколько можно доверять VADу, когда он говорит "это голос"
    recall = TP / (TP + FN) if (TP + FN) else 0    # Какую долю реального голоса мы не потеряли
    
    # F1-Score - главная метрика баланса
    f1_score = 0
    if precision + recall > 0:
        f1_score = 2 * (precision * recall) / (precision + recall)
        
    print("========================================")
    print("📊 CONFUSION MATRIX (Матрица ошибок)")
    print("========================================")
    print(f"✅ True Positive  (Голос сохранен) : {TP} кадров")
    print(f"✅ True Negative  (Шум вырезан)    : {TN} кадров")
    print(f"❌ False Positive (Оставлен шум)   : {FP} кадров")
    print(f"❌ False Negative (Потерян голос)  : {FN} кадров")
    print("----------------------------------------")
    print(f"🎯 Accuracy  (Точность общая) : {accuracy * 100:.2f}%")
    print(f"🎯 Precision (Отсутствие ФП)  : {precision * 100:.2f}%")
    print(f"🎯 Recall    (Чувствительность): {recall * 100:.2f}%")
    print(f"🏆 F1-Score  (Главный рейтинг) : {f1_score * 100:.2f}%")
    print("========================================")

if __name__ == "__main__":
    # Укажи тут пути к своим тестовым файлам
    AUDIO_FILE = os.path.join(BASE_DIR, "test_audio.wav") # Замени на свой файл
    CSV_FILE = os.path.join(BASE_DIR, "test_labels.csv")  # Твой файл с таймкодами
    
    if os.path.exists(AUDIO_FILE) and os.path.exists(CSV_FILE):
        run_test(AUDIO_FILE, CSV_FILE)
    else:
        print("Положите файлы test_audio.wav и test_labels.csv в папку со скриптом!")