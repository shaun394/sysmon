import subprocess
import json

result = subprocess.run(
    ["../c-core/collector.exe"],
    capture_output=True,
    text=True
)

raw_output = result.stdout.strip()
data = json.loads(raw_output)

if not data.get("ok"):
    print("C program reporterd an error:", data)
else:
    total = data["mem_total_mb"]
    free = data["mem_free_mb"]
    used = data["mem_used_mb"]
    pct = data["mem_used_percent"]

    print(f"RAM Used: {used} MB / {total} MB ({pct}%)")
    print(f"RAM Free: {free} MB")