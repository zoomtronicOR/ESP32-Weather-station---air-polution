# ESP32 meteo stanica sa merenjem zagađenosti vazduha

Samostalna Wi-Fi meteo stanica na ESP32: meri temperaturu, vlažnost, pritisak, čestice PM1.0/PM2.5/PM4/PM10,
UV indeks i osvetljenost, prikazuje ih na OLED ekranu i u web interfejsu, čuva istoriju, šalje podatke u
Home Assistant preko MQTT-a i ažurira se preko Wi-Fi-ja (OTA). Radi i bez interneta, MQTT-a i Home Assistant-a.

**Autor:** Zoran Nikolić (Zoomtronic)

## Mogućnosti

- **Merenje:** BME680 (T, RH, p, gasni otpor), SPS30 (PM1.0-PM10, na zasebnoj I2C magistrali),
  LTR-390 (UV indeks, lux), MPU6500 (nagib, pomeranje). Svako merenje ima vreme, status i oznaku validnosti.
- **Kvalitet vazduha:** nivo po skali EAQI (1-6) iz kliznog proseka PM2.5/PM10 od 10 min, sa histerezom.
- **OLED 0,96" 128×64 dvobojni:** žuti pojas sa vremenom i datumom, plavi deo sa stranicama po 3 reda.
- **Web interfejs (srpski, radi bez interneta):** kontrolna tabla (kartice ili instrumenti sa kazaljkama),
  istorija sa grafikonima (1 h - 30 dana), statistika, senzori sa šemom povezivanja, MQTT, Wi-Fi, podešavanja,
  alarmiranje, sistem (resursi, logovi, OTA).
- **Istorija:** 5-minutni agregati za 24 h i satni za 30 dana u LittleFS (posebna particija), izvoz CSV/JSON.
- **Alarmi:** 12 pravila sa pragom, histerezom i minimalnim trajanjem (temperatura, vlažnost, pritisak,
  PM, UV, senzori, Wi-Fi, MQTT, vreme, memorija).
- **Home Assistant:** MQTT Discovery za sve senzore, alarme, LED indikator i dugme za restart.
- **Vreme:** NTP, zona Europe/Belgrade, letnje/zimsko računanje vremena.
- **WS2812B LED indikator:** boja po kvalitetu vazduha, noćni režim, ograničenje struje (opciono).
- **OTA:** ažuriranje firmware-a i web fajlova preko Wi-Fi-ja, sa vraćanjem na prethodnu verziju
  ako novi firmware ne radi stabilno.
- **Statusna LED na ploči:** kod treptanja (1 bljesak / 2 s = sve ispravno).

## Hardver

Ploča: NodeMCU ESP-32S V1.1 (ESP32-D0WD-V3, 4 MB flash, bez PSRAM-a).

| Komponenta | Interfejs | Pinovi | Napajanje |
|---|---|---|---|
| BME680 | I2C0 (0x77) | SDA P21, SCL P22 | 3,3 V |
| MPU6500 | I2C0 (0x68) | SDA P21, SCL P22 | 3,3 V |
| LTR-390 | I2C0 (0x53) | SDA P21, SCL P22 | 3,3 V |
| OLED SSD1306 128×64 | I2C0 (0x3C) | SDA P21, SCL P22 | 3,3 V |
| SPS30 | I2C1 (0x69) | SDA P16, SCL P17, SEL → GND, pull-up 4,7-10 kΩ na 3,3 V | 5 V |
| WS2812B (opciono) | RMT | DIN P27 preko 330-470 Ω | 5 V |

Svi pinovi su u [`include/pins.h`](include/pins.h). Fotografije ploče su u folderu [`docs/`](docs/).

## Izgradnja i prvo pokretanje

Potreban je [PlatformIO](https://platformio.org/) (Arduino-ESP32 3.1.3, pioarduino platforma).

```
pio run -t upload        # firmware preko USB-a
pio run -t uploadfs      # web interfejs (folder data/) preko USB-a
```

1. Stanica bez podešene mreže pokreće pristupnu tačku **METEO-ESP32-SETUP**; lozinka je prikazana na OLED-u.
2. Na telefonu se povežite na nju, otvorite `http://192.168.4.1`, izaberite Wi-Fi mrežu i zadajte
   administratorsku lozinku.
3. Posle povezivanja stanica je dostupna na prikazanoj IP adresi i na `http://meteo-esp32.local`.

> **Bezbednost:** podrazumevane lozinke se izvode iz MAC adrese uređaja i nisu tajne. Posle prvog
> podešavanja obavezno promenite **administratorsku lozinku i lozinku pristupne tačke**
> (Podešavanja → Pristup). Uređaj nije namenjen direktnom izlaganju internetu.

## Ažuriranje preko Wi-Fi-ja (OTA)

Iz web interfejsa: Sistem → Ažuriranje firmware-a (`firmware.bin` ili `littlefs.bin`).

Iz PlatformIO-a: kopirajte `ota.example.ini` u `secrets/ota.ini` (folder je van git-a) i upišite adresu
i administratorsku lozinku, zatim:

```
pio run -e ota -t upload      # firmware
pio run -e ota -t uploadfs    # web interfejs (istorija merenja ostaje sačuvana)
```

## REST API i MQTT

- REST: `/api/v1/status`, `readings`, `sensors`, `history`, `statistics`, `alarms`, `time`, `wifi`, `mqtt`,
  `config`, `system`, `logs`, `led`. Izmene (POST) traže administratorsku prijavu (HTTP Basic, korisnik `admin`).
- MQTT: osnovni topic `meteo/esp32_r201/`, pojedinačne vrednosti (`.../temperature`, `.../pm2_5`, ...),
  objedinjeni JSON `.../state`, dostupnost `.../availability` (LWT), alarmi `.../alarm/<id>`.

## Struktura projekta

```
src/           firmware (moduli po funkciji, drajveri senzora u src/sensors/)
include/       config.h (konstante), pins.h (raspored pinova), log.h
data/          web interfejs (HTML/CSS/JS, bez spoljnih biblioteka)
test/          testovi pojedinačnih senzora na samoj ploči (pio test -f test_<senzor>)
tools/         OTA upload i alat za dugotrajni test
docs/          fotografije ploče
partitions.csv tabela particija (2 × OTA, LittleFS web, LittleFS istorija)
```

Vrednosti i statistika su lokalno izmerene i izračunate; nivo kvaliteta vazduha je lokalna procena,
a ne zvanični indeks ili meteorološka prognoza.
