import json
import os

# === НАСТРОЙКИ ===
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
GT_PATH = os.path.join(BASE_DIR, "ground_truth_masks.json")
PLUGIN_PATH = os.path.join(BASE_DIR, "plugin_masks.json")

def evaluate():
    if not os.path.exists(GT_PATH) or not os.path.exists(PLUGIN_PATH):
        print("❌ Ошибка: Не найдены файлы масок. Сначала запусти Step 1 и Step 2!")
        return

    with open(GT_PATH, 'r') as f:
        gt_data = json.load(f)
    with open(PLUGIN_PATH, 'r') as f:
        plugin_data = json.load(f)

    overall_stats = {"tp": 0, "tn": 0, "fp": 0, "fn": 0}
    category_stats = {}

    print(f"{'Файл':<30} | {'Точность':<10} | {'Тип ошибки'}")
    print("-" * 60)

    for filename, gt_info in gt_data.items():
        if filename not in plugin_data:
            print(f"⚠️ Пропуск: {filename} не найден в результатах плагина")
            continue

        gt_mask = gt_info['mask']
        pl_mask = plugin_data[filename]['mask']

        # Выравниваем длину масок (на случай микро-разрывов в конце файла)
        min_len = min(len(gt_mask), len(pl_mask))
        
        tp, tn, fp, fn = 0, 0, 0, 0
        
        for i in range(min_len):
            g = gt_mask[i]
            p = pl_mask[i]
            
            if g == '1' and p == '1': tp += 1
            elif g == '0' and p == '0': tn += 1
            elif g == '0' and p == '1': fp += 1
            elif g == '1' and p == '0': fn += 1

        file_total = tp + tn + fp + fn
        file_acc = (tp + tn) / file_total * 100 if file_total > 0 else 0
        
        # Группировка по папкам (noise_only, speech_clean и т.д.)
        category = filename.split('/')[0]
        if category not in category_stats:
            category_stats[category] = {"tp": 0, "tn": 0, "fp": 0, "fn": 0}
        
        for key in ["tp", "tn", "fp", "fn"]:
            category_stats[category][key] += locals()[key]
            overall_stats[key] += locals()[key]

        error_msg = ""
        if fp > fn: error_msg = f"Больше шума (+{fp})"
        elif fn > fp: error_msg = f"Ест слова (-{fn})"

        print(f"{filename:<30} | {file_acc:>8.2f}% | {error_msg}")

    print("\n" + "="*60)
    print("📊 ИТОГОВЫЙ ОТЧЕТ ПО КАТЕГОРИЯМ:")
    
    for cat, s in category_stats.items():
        cat_acc = (s['tp'] + s['tn']) / (s['tp'] + s['tn'] + s['fp'] + s['fn']) * 100
        print(f"📁 {cat:<15}: {cat_acc:.2f}%")

    total_frames = sum(overall_stats.values())
    total_accuracy = (overall_stats['tp'] + overall_stats['tn']) / total_frames * 100

    print("\n" + "="*60)
    print(f"🏆 ОБЩАЯ ТОЧНОСТЬ (Frame-level Accuracy): {total_accuracy:.2f}%")
    
    target = 94.0
    if total_accuracy >= target:
        print(f"✅ ЦЕЛЬ ДОСТИГНУТА! Вы перешагнули порог в {target}%")
    else:
        print(f"❌ НУЖНА ДОРАБОТКА. До цели не хватило {target - total_accuracy:.2f}%")
    print("="*60)

if __name__ == "__main__":
    evaluate()