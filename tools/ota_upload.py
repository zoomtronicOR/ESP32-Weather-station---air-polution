"""OTA upload preko HTTP-a (web interfejs uređaja: /api/v1/ota/firmware i /api/v1/ota/fs).

Poziva ga PlatformIO (env:ota, upload_protocol = custom) za `upload` i `uploadfs`:
    python tools/ota_upload.py <putanja do .bin>
Adresu i administratorsku lozinku čita iz secrets/ota.ini (van git-a); lozinku nikada ne ispisuje.
Slika web fajlova (littlefs.bin) ide na /api/v1/ota/fs, sve ostalo na /api/v1/ota/firmware.
"""

import base64
import configparser
import os
import sys
import time
import urllib.error
import urllib.request
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_secret():
    cfg = configparser.ConfigParser()
    path = os.path.join(ROOT, "secrets", "ota.ini")
    if not cfg.read(path, encoding="utf-8") or not cfg.has_section("ota_secret"):
        sys.exit(f"Nema fajla {path} sa sekcijom [ota_secret] (vidi ota.example.ini).")
    host = cfg.get("ota_secret", "host", fallback="").strip()
    password = cfg.get("ota_secret", "password", fallback="").strip()
    if not host or not password:
        sys.exit("U secrets/ota.ini nedostaje host ili password.")
    return host, password


def ascii_text(s):
    # PlatformIO prosleđuje izlaz na Windows konzolu (cp1252), koja ne ume č/ć/š/ž/đ
    table = str.maketrans("čćšžđČĆŠŽĐ", "ccszdCCSZD")
    return s.translate(table).encode("ascii", "replace").decode()


def main():
    if len(sys.argv) != 2:
        sys.exit("Upotreba: ota_upload.py <fajl.bin>")
    image = sys.argv[1]
    is_fs = "littlefs" in os.path.basename(image).lower() or "spiffs" in os.path.basename(image).lower()
    host, password = load_secret()
    url = f"http://{host}/api/v1/ota/{'fs' if is_fs else 'firmware'}"

    with open(image, "rb") as f:
        data = f.read()
    boundary = uuid.uuid4().hex
    body = (
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{os.path.basename(image)}\"\r\n"
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + data + f"\r\n--{boundary}--\r\n".encode()

    auth = base64.b64encode(f"admin:{password}".encode()).decode()
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Authorization": f"Basic {auth}",
    })
    what = "web fajlovi" if is_fs else "firmware"
    print(f"OTA ({what}): {os.path.basename(image)}, {len(data)} B -> {url}", flush=True)
    start = time.time()
    try:
        with urllib.request.urlopen(req, timeout=240) as resp:
            print(ascii_text(f"{resp.status}: {resp.read().decode('utf-8', 'replace')} ({time.time() - start:.1f} s)"))
    except urllib.error.HTTPError as e:
        sys.exit(ascii_text(f"Greska {e.code}: {e.read().decode('utf-8', 'replace')}"))
    except Exception as e:  # mreža, timeout
        sys.exit(ascii_text(f"OTA nije uspeo: {e}"))


if __name__ == "__main__":
    main()
