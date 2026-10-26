"""Dugotrajni test stabilnosti (specifikacija §19, Faza 8): na svaki minut čita stanje uređaja
i upisuje ga u CSV. Restart se prepoznaje po smanjenju vremena rada (uptime).

    python tools/soak_monitor.py [host] [trajanje_h]

Izlaz: soak/soak_<datum>.csv (folder soak/ je u .gitignore). Koristi samo javne GET rute, bez lozinke.
"""

import csv
import datetime
import json
import os
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.88.152"
HOURS = float(sys.argv[2]) if len(sys.argv) > 2 else 72.0
INTERVAL_S = 60

FIELDS = ["time_local", "reachable", "firmware", "uptime_s", "reboot", "reset_reason", "boot_count", "heap_free",
          "heap_min_free", "heap_max_block", "rssi", "wifi", "mqtt", "time_status", "sensors_ok", "sensors_total",
          "alarms_active", "history_5min", "note"]


def get(path):
    with urllib.request.urlopen(f"http://{HOST}{path}", timeout=10) as r:
        return json.loads(r.read().decode("utf-8"))


def main():
    os.makedirs(os.path.join(ROOT, "soak"), exist_ok=True)
    path = os.path.join(ROOT, "soak", f"soak_{datetime.datetime.now():%Y%m%d_%H%M}.csv")
    end = time.time() + HOURS * 3600
    last_uptime = None
    reboots = 0
    print(f"Dugotrajni test: {HOST}, {HOURS:g} h -> {path}", flush=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        while time.time() < end:
            row = {"time_local": datetime.datetime.now().isoformat(timespec="seconds")}
            try:
                s = get("/api/v1/system")
                st = get("/api/v1/status")
                up = s["uptime_s"]
                reboot = last_uptime is not None and up < last_uptime
                if reboot:
                    reboots += 1
                    row["note"] = "RESTART"
                    print(f"{row['time_local']} RESTART #{reboots}: {s.get('reset_reason')}", flush=True)
                last_uptime = up
                row.update(reachable=1, firmware=s["firmware"], uptime_s=up, reboot=int(reboot),
                           reset_reason=s.get("reset_reason"), boot_count=s.get("boot_count"),
                           heap_free=s["heap_free"], heap_min_free=s["heap_min_free"],
                           heap_max_block=s["heap_max_block"], rssi=s.get("rssi"),
                           wifi=int(st["wifi"]["connected"]), mqtt=int(st["mqtt"]["connected"]),
                           time_status=st["time"]["status"], sensors_ok=st["sensors"]["ok"],
                           sensors_total=st["sensors"]["total"], alarms_active=st.get("alarms_active"),
                           history_5min=s.get("history_5min"))
            except Exception as e:  # uređaj nedostupan (mreža, restart)
                row.update(reachable=0, note=type(e).__name__)
            w.writerow(row)
            f.flush()
            time.sleep(INTERVAL_S)
    print(f"Kraj testa. Restarta: {reboots}. Rezultat: {path}", flush=True)


if __name__ == "__main__":
    main()
