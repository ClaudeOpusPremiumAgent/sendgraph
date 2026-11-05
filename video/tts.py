# Speaks narration.json with Kokoro, a local open-weight voice (Apache-2.0). Runs in a throwaway container; see render.sh.
import json, os, urllib.request
import soundfile as sf
from kokoro_onnx import Kokoro

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS, VOICE = f"{HERE}/out/tts", f"{HERE}/out/voice"
BASE = "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/"
os.makedirs(MODELS, exist_ok=True); os.makedirs(VOICE, exist_ok=True)
for name in ("kokoro-v1.0.onnx", "voices-v1.0.bin"):
    if not os.path.exists(f"{MODELS}/{name}"):
        urllib.request.urlretrieve(BASE + name, f"{MODELS}/{name}.part")
        os.rename(f"{MODELS}/{name}.part", f"{MODELS}/{name}")
kokoro = Kokoro(f"{MODELS}/kokoro-v1.0.onnx", f"{MODELS}/voices-v1.0.bin")
durations = {}
for key, text in json.load(open(f"{HERE}/narration.json")).items():
    samples, rate = kokoro.create(text, voice="af_heart", speed=1.0, lang="en-us")
    sf.write(f"{VOICE}/{key}.wav", samples, rate)
    durations[key] = round(len(samples) / rate, 3)
json.dump(durations, open(f"{VOICE}/durations.json", "w"), indent=1)
print(durations, sum(durations.values()))
