import json, sys, urllib.parse, urllib.request
OUT = sys.argv[1]
SPEAKER = 3  # ずんだもん ノーマル
LINES = {
    "co2_caution":   "二酸化炭素が多くなってきたのだ。窓を開けて換気するのだ！",
    "co2_warning":   "二酸化炭素がすごく多いのだ！今すぐ窓を開けて換気するのだ！",
    "heat_caution":  "熱中症に気をつけるのだ。こまめに水を飲むのだ。",
    "heat_severe":   "熱中症の危険が高まってるのだ！エアコンをつけて、水分と塩分をとるのだ！",
    "heat_danger":   "熱中症の危険があるのだ！すぐに部屋を涼しくするのだ！",
    "humidity_low":  "空気がカラカラなのだ。加湿するといいのだ。",
    "humidity_high": "じめじめしてきたのだ。カビや結露に気をつけるのだ。",
    "temp_low":      "部屋が寒くなってきたのだ。暖かくして過ごすのだ。",
    "pressure_drop": "気圧が急に下がってるのだ。体調の変化に気をつけるのだ。",
}
BASE = "http://localhost:50021"
for name, text in LINES.items():
    q = urllib.parse.urlencode({"speaker": SPEAKER, "text": text})
    with urllib.request.urlopen(urllib.request.Request(f"{BASE}/audio_query?{q}", method="POST")) as r:
        query = json.load(r)
    query["outputSamplingRate"] = 16000
    query["outputStereo"] = False
    req = urllib.request.Request(f"{BASE}/synthesis?speaker={SPEAKER}", data=json.dumps(query).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req) as r, open(f"{OUT}/{name}.wav", "wb") as f:
        f.write(r.read())
    print(name, "ok")
with open(f"{OUT}/lines.json", "w", encoding="utf-8") as f:
    json.dump({"engine": "VOICEVOX", "speaker": "ずんだもん(ノーマル)", "speaker_id": SPEAKER, "lines": LINES}, f, ensure_ascii=False, indent=2)
