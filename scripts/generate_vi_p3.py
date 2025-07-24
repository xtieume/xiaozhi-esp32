import json
import os
from gtts import gTTS
import subprocess

# Đường dẫn tương đối khi chạy từ thư mục scripts/
LANG_JSON = "../main/assets/vi-VN/language.json"
OUTPUT_DIR = "../main/assets/vi-VN"
CONVERT_SCRIPT = "p3_tools/convert_audio_to_p3.py"

# 1. Đọc language.json
with open(LANG_JSON, "r", encoding="utf-8") as f:
    data = json.load(f)

for key, text in data["strings"].items():
    mp3_path = os.path.join(OUTPUT_DIR, f"{key}.mp3")
    p3_path = os.path.join(OUTPUT_DIR, f"{key}.p3")
    # 2. TTS ra mp3 nếu chưa có
    if not os.path.exists(mp3_path):
        tts = gTTS(text, lang='vi')
        tts.save(mp3_path)
        print(f"Đã tạo {mp3_path}")
    # 3. Chuyển mp3 sang p3
    if not os.path.exists(p3_path):
        subprocess.run([
            "python", CONVERT_SCRIPT,
            mp3_path, p3_path
        ])
        print(f"Đã tạo {p3_path}")