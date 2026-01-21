import subprocess
import json
import time
import os
import sys

def bar(label, percent, width=30):
    percent = max(0.0, min(100.0, float(percent)))
    filled = int((percent / 100) * width)
    empty = width - filled
    return f"{label}: [{'#' * filled}{'-' * empty}] {percent:5.1f}%"

def run_collector():
    result = subprocess.run(
        ["../c-core/collector.exe"],
        capture_output=True,
        text=True
    )
    raw = result.stdout.strip()
    if not raw:
        return {"ok": False, "error": "No output from collector.exe"}
    return json.loads(raw)

def clear_screen():
    os.system("cls")

def main():
    while True:
        result = subprocess.run(
            ["../c-core/collector.exe"],
            capture_output=True,
            text=True
        )

        data = json.loads(result.stdout.strip())

        clear_screen()
        print("=== SysMon (C + Python) ===\n")

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
            disk_pct = data["disk_active_percent"]


            print(bar("CPU", cpu))
            print(bar("RAM", mem_pct))
            print(bar("DISK (Active)", disk_pct))

            print()
            print(f"RAM  : {used} / {total} MB")
            print(f"DISK : {disk_used:.1f} / {disk_total:.1f} GB (Free: {disk_free:.1f} GB)")

            time.sleep(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        clear_screen()
        print("Stopped.")
        sys.exit(0)