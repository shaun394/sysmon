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
        # CPU
        cpu = data["cpu_percent"]
        # RAM
        total = data["mem_total_mb"]
        free = data["mem_free_mb"]
        used = data["mem_used_mb"]
        mem_pct = data["mem_used_percent"]
        # DISK
        disk_total = data["disk_total_gb"]
        disk_free = data["disk_free_gb"]
        disk_used = data["disk_used_gb"]
        disk_pct = data["disk_used_percent"]

        print("=== SysMon (C + Python) ===")
        print(f"CPU: {cpu:.1f}%")
        print(f"RAM: {used} / {total} MB ({mem_pct:.1f}%)")
        print(f"DISK: {disk_used:.1f} / {disk_total:.1f} GB used ({disk_pct:.1f}%)  |  Free: {disk_free:.1f} GB")

        time.sleep(1)