import subprocess
import json
import time
import os

def clear_screen():
    os.system("cls")

while True:
    result = subprocess.run(
        ["../c-core/collector.exe"],
        capture_output=True,
        text=True
    )

    data = json.loads(result.stdout.strip())

    clear_screen()

    if not data.get("ok"):
        print("C program reporterd an error:", data)
    else:
        cpu = data["cpu_percent"]
        total = data["mem_total_mb"]
        free = data["mem_free_mb"]
        used = data["mem_used_mb"]
        mem_pct = data["mem_used_percent"]

        print("=== SysMon (C + Python) ===")
        print(f"CPU: {cpu:.1f}%")
        print(f"RAM: {used} / {total} MB ({mem_pct:.1f}%)")

        time.sleep(1)