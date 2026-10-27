#include "storage_manager.h"

#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "log.h"
#include "time_manager.h"

namespace storage_manager {

static const char *TAG = "istorija";

static constexpr size_t METRICS = (size_t)MetricId::Count;

// Kružni niz je podeljen na segmente (zasebne male fajlove) u koje se zapisi samo dopisuju.
// LittleFS pri izmeni sredine fajla prepisuje sve od tog mesta do kraja, pa bi kružni niz u jednom
// fajlu sa zaglavljem na početku pri svakom upisu prepisivao ceo fajl (satni ~154 KB) -- to troši
// flash i ne staje na particiju. Segment je manji od jednog bloka flash-a.
static constexpr uint16_t SEG_RECORDS = 16;
static constexpr uint32_t SEG_MAGIC = 0x4D545232;  // "MTR2"

struct __attribute__((packed)) SegHeader {
    uint32_t magic;
    uint16_t metricCount;
    uint16_t recordSize;
    uint32_t seq;  // redni broj segmenta; najveći je tekući
};

static_assert(HISTORY_5MIN_RECORDS % SEG_RECORDS == 0, "kapacitet mora biti deljiv veličinom segmenta");
static_assert(HISTORY_HOUR_RECORDS % SEG_RECORDS == 0, "kapacitet mora biti deljiv veličinom segmenta");
static constexpr size_t MAX_SEGS = HISTORY_HOUR_RECORDS / SEG_RECORDS + 1;

struct Ring {
    const char *dir;
    const char *legacyPath;  // jedan fajl sa zaglavljem (format do v0.8.0), prenosi se pri prvom pokretanju
    uint16_t capacity;
    uint32_t interval;
    uint16_t segs;           // capacity / SEG_RECORDS + 1 (tekući segment se puni dok najstariji još važi)
    uint16_t head;           // tekući segment
    uint32_t headSeq;
    uint8_t fill[MAX_SEGS];  // broj zapisa po segmentu
    size_t total;            // ukupno zapisa u svim segmentima (do capacity + SEG_RECORDS)
    bool ok;
};

static Ring s_rings[2] = {
    {"/h5", "/hist5.bin", HISTORY_5MIN_RECORDS, 300, HISTORY_5MIN_RECORDS / SEG_RECORDS + 1, 0, 0, {}, 0, false},
    {"/h60", "/hist60.bin", HISTORY_HOUR_RECORDS, 3600, HISTORY_HOUR_RECORDS / SEG_RECORDS + 1, 0, 0, {}, 0, false},
};

static SemaphoreHandle_t s_mutex = nullptr;

// Posebna LittleFS particija "history" -- uploadfs/OTA web fajlova je ne dira
static fs::LittleFSFS s_fs;

static Ring &ring(Resolution r) { return s_rings[r == Resolution::Min5 ? 0 : 1]; }

static size_t ringCount(const Ring &rg) { return rg.total < rg.capacity ? rg.total : rg.capacity; }

// ---------------------------------------------------------------------------
// Kružni niz u segmentima
// ---------------------------------------------------------------------------

static void segPath(const Ring &rg, uint16_t seg, char *buf, size_t len) {
    snprintf(buf, len, "%s/%02u.bin", rg.dir, (unsigned)seg);
}

static bool startSegment(Ring &rg, uint16_t seg, uint32_t seq) {
    char path[24];
    segPath(rg, seg, path, sizeof(path));
    File f = s_fs.open(path, "w");
    if (!f) return false;
    SegHeader h = {SEG_MAGIC, (uint16_t)METRICS, (uint16_t)sizeof(HistRecord), seq};
    bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
    f.close();
    rg.total -= rg.fill[seg];
    rg.fill[seg] = 0;
    rg.head = seg;
    rg.headSeq = seq;
    return ok;
}

static bool appendRecord(Ring &rg, const HistRecord &rec) {
    if (rg.fill[rg.head] >= SEG_RECORDS) {
        // Tekući segment je pun: prelazi se na sledeći, čime se briše najstariji
        if (!startSegment(rg, (rg.head + 1) % rg.segs, rg.headSeq + 1)) return false;
    }
    char path[24];
    segPath(rg, rg.head, path, sizeof(path));
    File f = s_fs.open(path, "a");
    if (!f) return false;
    bool ok = f.write((const uint8_t *)&rec, sizeof(rec)) == sizeof(rec);
    f.close();
    if (ok) {
        rg.fill[rg.head]++;
        rg.total++;
    }
    return ok;
}

// Učitava stanje segmenata posle pokretanja
static bool openRing(Ring &rg) {
    if (!s_fs.exists(rg.dir) && !s_fs.mkdir(rg.dir)) return false;
    uint32_t seqs[MAX_SEGS] = {};
    bool any = false, foreign = false;
    for (uint16_t s = 0; s < rg.segs; s++) {
        rg.fill[s] = 0;
        char path[24];
        segPath(rg, s, path, sizeof(path));
        if (!s_fs.exists(path)) continue;
        File f = s_fs.open(path, "r");
        SegHeader h;
        bool good = f && f.size() >= sizeof(h) && f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) &&
                    h.magic == SEG_MAGIC && h.metricCount == METRICS && h.recordSize == sizeof(HistRecord);
        if (good) {
            size_t n = (f.size() - sizeof(h)) / sizeof(HistRecord);  // nepotpun poslednji zapis se zanemaruje
            rg.fill[s] = n > SEG_RECORDS ? SEG_RECORDS : n;
            seqs[s] = h.seq;
            if (!any || h.seq > rg.headSeq) {
                rg.head = s;
                rg.headSeq = h.seq;
            }
            any = true;
        }
        if (f) f.close();
        if (!good) {
            // Promenjen format (npr. nova metrika) ili oštećen segment
            foreign = true;
            s_fs.remove(path);
        }
    }
    if (foreign) LOG_W(TAG, "%s: segmenti nepoznatog formata su obrisani", rg.dir);
    rg.total = 0;
    if (!any) return startSegment(rg, 0, 1);
    // Važe samo segmenti čiji redni broj odgovara položaju iza tekućeg (ostali su zaostali)
    for (uint16_t back = 0; back < rg.segs; back++) {
        uint16_t s = (rg.head + rg.segs - back) % rg.segs;
        if (seqs[s] != rg.headSeq - back) rg.fill[s] = 0;
        rg.total += rg.fill[s];
    }
    return true;
}

// Logički indeks (0 = najstariji od poslednjih capacity zapisa) -> segment i položaj u njemu
static bool locate(const Ring &rg, size_t index, uint16_t &seg, size_t &pos) {
    size_t count = ringCount(rg);
    if (index >= count) return false;
    size_t g = (rg.total - count) + index;
    for (uint16_t j = 1; j <= rg.segs; j++) {
        uint16_t s = (rg.head + j) % rg.segs;  // od najstarijeg ka tekućem
        if (g < rg.fill[s]) {
            seg = s;
            pos = g;
            return true;
        }
        g -= rg.fill[s];
    }
    return false;
}

// Sekvencijalno čitanje: fajl segmenta ostaje otvoren dok se čita iz istog segmenta
struct Reader {
    File f;
    int seg = -1;
};

static bool readRecordLocked(Ring &rg, Reader &rd, size_t index, HistRecord &out) {
    uint16_t seg;
    size_t pos;
    if (!locate(rg, index, seg, pos)) return false;
    if (rd.seg != seg) {
        if (rd.f) rd.f.close();
        char path[24];
        segPath(rg, seg, path, sizeof(path));
        rd.f = s_fs.open(path, "r");
        rd.seg = seg;
    }
    return rd.f && rd.f.seek(sizeof(SegHeader) + pos * sizeof(HistRecord)) &&
           rd.f.read((uint8_t *)&out, sizeof(out)) == sizeof(out);
}

// Istorija iz jednog fajla (format do v0.8.0) se prenosi u segmente, a stari fajl briše
static void migrateLegacy(Ring &rg) {
    struct __attribute__((packed)) LegacyHeader {
        uint32_t magic;
        uint16_t version, metricCount, recordSize, capacity, head, count;
    } h;
    if (!s_fs.exists(rg.legacyPath)) return;
    File f = s_fs.open(rg.legacyPath, "r");
    size_t moved = 0;
    if (f && f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) && h.magic == 0x4D545231 && h.version == 1 &&
        h.metricCount == METRICS && h.recordSize == sizeof(HistRecord) && h.capacity > 0 &&
        h.head < h.capacity && h.count <= h.capacity && rg.total == 0) {
        size_t oldest = (h.head + h.capacity - h.count) % h.capacity;
        HistRecord rec;
        for (size_t i = 0; i < h.count; i++) {
            size_t slot = (oldest + i) % h.capacity;
            if (!f.seek(sizeof(h) + slot * sizeof(HistRecord)) || f.read((uint8_t *)&rec, sizeof(rec)) != sizeof(rec))
                break;
            if (rec.ts != 0 && appendRecord(rg, rec)) moved++;
            esp_task_wdt_reset();
        }
    }
    if (f) f.close();
    s_fs.remove(rg.legacyPath);
    LOG_I(TAG, "%s: preneto %u zapisa u novi format (%s)", rg.legacyPath, (unsigned)moved, rg.dir);
}

// ---------------------------------------------------------------------------
// Agregacija
// ---------------------------------------------------------------------------

struct Acc {
    double sum;
    float min;
    float max;
    uint32_t count;
};

static void accReset(Acc *acc) {
    for (size_t i = 0; i < METRICS; i++) acc[i] = {0.0, INFINITY, -INFINITY, 0};
}

static void accToRecord(const Acc *acc, uint32_t ts, HistRecord &rec) {
    rec.ts = ts;
    for (size_t i = 0; i < METRICS; i++) {
        if (acc[i].count == 0) {
            rec.m[i] = {NAN, NAN, NAN, 0};
        } else {
            rec.m[i] = {(float)(acc[i].sum / acc[i].count), acc[i].min, acc[i].max,
                        (uint16_t)min<uint32_t>(acc[i].count, 65535)};
        }
    }
}

// Tekući 5-minutni interval
static Acc s_acc[METRICS];
static uint32_t s_lastSeenTs[METRICS];
static bool s_bucketAligned = false;   // granice po satu (pouzdano vreme) ili po uptime-u
static uint32_t s_bucketId = 0;        // epoch/300 ako je poravnat
static uint32_t s_bucketStartMs = 0;   // uptime početka intervala

// Satni agregat koji se puni iz 5-minutnih zapisa
static Acc s_hourAcc[METRICS];
static uint32_t s_hourId = 0;  // epoch/3600, 0 = prazno

// Intervali zatvoreni pre nego što je vreme bilo pouzdano (specifikacija §12.7)
struct Pending {
    uint32_t startMs;
    HistRecord rec;
};
static Pending s_pending[HISTORY_PENDING_MAX];
static size_t s_pendingCount = 0;

static void addToHour(const HistRecord &rec) {
    uint32_t hour = rec.ts / 3600;
    if (s_hourId != 0 && hour != s_hourId) {
        HistRecord h;
        accToRecord(s_hourAcc, s_hourId * 3600, h);
        if (!appendRecord(ring(Resolution::Hour), h)) LOG_E(TAG, "Upis satnog zapisa nije uspeo");
        accReset(s_hourAcc);
    }
    s_hourId = hour;
    for (size_t i = 0; i < METRICS; i++) {
        const MetricAgg &a = rec.m[i];
        if (a.count == 0) continue;
        s_hourAcc[i].sum += (double)a.avg * a.count;  // ponderisano brojem merenja
        s_hourAcc[i].count += a.count;
        s_hourAcc[i].min = min(s_hourAcc[i].min, a.min);
        s_hourAcc[i].max = max(s_hourAcc[i].max, a.max);
    }
}

// Posle restarta se tekući sat dopunjuje 5-minutnim zapisima iz fajla
static void rebuildHour(uint32_t hour) {
    Ring &rg = ring(Resolution::Min5);
    accReset(s_hourAcc);
    s_hourId = hour;
    Reader rd;
    HistRecord rec;
    size_t n = ringCount(rg);
    size_t start = n > 12 ? n - 12 : 0;
    for (size_t i = start; i < n; i++) {
        if (!readRecordLocked(rg, rd, i, rec) || rec.ts / 3600 != hour) continue;
        for (size_t m = 0; m < METRICS; m++) {
            const MetricAgg &a = rec.m[m];
            if (a.count == 0) continue;
            s_hourAcc[m].sum += (double)a.avg * a.count;
            s_hourAcc[m].count += a.count;
            s_hourAcc[m].min = min(s_hourAcc[m].min, a.min);
            s_hourAcc[m].max = max(s_hourAcc[m].max, a.max);
        }
    }
    if (rd.f) rd.f.close();
}

static void storeRecord(const HistRecord &rec) {
    if (s_hourId == 0) rebuildHour(rec.ts / 3600);
    if (!appendRecord(ring(Resolution::Min5), rec)) {
        LOG_E(TAG, "Upis 5-minutnog zapisa nije uspeo");
        return;
    }
    addToHour(rec);
}

static bool hasData(const Acc *acc) {
    for (size_t i = 0; i < METRICS; i++) {
        if (acc[i].count) return true;
    }
    return false;
}

static void closeBucket(uint32_t now) {
    if (!hasData(s_acc)) {
        accReset(s_acc);
        return;
    }
    HistRecord rec;
    if (s_bucketAligned) {
        accToRecord(s_acc, s_bucketId * 300, rec);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        storeRecord(rec);
        xSemaphoreGive(s_mutex);
    } else {
        // Vreme još nije pouzdano: interval čeka u RAM-u, vremenska oznaka se računa kasnije
        if (s_pendingCount == HISTORY_PENDING_MAX) {
            memmove(&s_pending[0], &s_pending[1], sizeof(Pending) * (HISTORY_PENDING_MAX - 1));
            s_pendingCount--;
            LOG_W(TAG, "Vreme nije sinhronizovano -- najstariji interval bez vremena je odbačen");
        }
        accToRecord(s_acc, 0, s_pending[s_pendingCount].rec);
        s_pending[s_pendingCount].startMs = s_bucketStartMs;
        s_pendingCount++;
    }
    accReset(s_acc);
}

static void flushPending() {
    if (s_pendingCount == 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_pendingCount; i++) {
        // Retroaktivno vreme iz uptime-a (isto pokretanje uređaja)
        s_pending[i].rec.ts = (uint32_t)time_manager::epochFromUptime(s_pending[i].startMs);
        storeRecord(s_pending[i].rec);
    }
    xSemaphoreGive(s_mutex);
    LOG_I(TAG, "Upisano %u intervala iz perioda pre sinhronizacije vremena", (unsigned)s_pendingCount);
    s_pendingCount = 0;
}

// ---------------------------------------------------------------------------

void begin() {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_fs.begin(true, "/hist", 4, "history")) {
        LOG_E(TAG, "LittleFS nije dostupan -- istorija se ne čuva");
        return;
    }
    for (Ring &rg : s_rings) {
        rg.ok = openRing(rg);
        if (rg.ok) {
            migrateLegacy(rg);
            LOG_I(TAG, "%s: %u / %u zapisa (interval %lu s)", rg.dir, (unsigned)ringCount(rg), rg.capacity,
                  (unsigned long)rg.interval);
        } else {
            LOG_E(TAG, "%s: istorija nije dostupna", rg.dir);
        }
    }
    // Zaostali web fajlovi sa ranije pogrešno upisane slike particije
    for (const char *junk : {"/index.html", "/app.js", "/style.css", "/favicon.svg"}) {
        if (s_fs.exists(junk)) s_fs.remove(junk);
    }
    accReset(s_acc);
    accReset(s_hourAcc);
    s_bucketStartMs = millis();
}

void update(uint32_t now) {
    if (!s_rings[0].ok) return;

    // Svako novo validno merenje ulazi u tekući interval
    for (size_t i = 0; i < METRICS; i++) {
        const Reading &r = metrics::get((MetricId)i);
        if (!r.valid || r.timestampMs == s_lastSeenTs[i]) continue;
        s_lastSeenTs[i] = r.timestampMs;
        Acc &a = s_acc[i];
        a.sum += r.value;
        a.count++;
        a.min = min(a.min, r.value);
        a.max = max(a.max, r.value);
    }

    bool timeValid = time_manager::isValid();
    if (timeValid && !s_bucketAligned) {
        // Vreme je upravo postalo pouzdano: raniji intervali dobijaju vreme unazad, a tekući se nastavlja
        // kao poravnat (da vremenske oznake u istoriji uvek rastu)
        flushPending();
        s_bucketAligned = true;
        s_bucketId = (uint32_t)time_manager::nowUtc() / 300;
        return;
    }

    if (s_bucketAligned) {
        uint32_t id = (uint32_t)time_manager::nowUtc() / 300;
        if (id != s_bucketId) {
            closeBucket(now);
            s_bucketId = id;
        }
    } else if (now - s_bucketStartMs >= 300000) {
        closeBucket(now);
        s_bucketStartMs = now;
    }
}

void clear() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Cela particija se formatira: brišu se i događaji alarma i eventualni zaostali fajlovi
    esp_task_wdt_reset();
    s_fs.end();
    bool formatted = s_fs.format();
    esp_task_wdt_reset();
    if (!s_fs.begin(true, "/hist", 4, "history")) LOG_E(TAG, "Particija istorije nije dostupna posle formatiranja");
    for (Ring &rg : s_rings) {
        rg.ok = openRing(rg);
    }
    if (!formatted) LOG_W(TAG, "Formatiranje nije uspelo -- fajlovi istorije su napravljeni iznova");
    accReset(s_hourAcc);
    s_hourId = 0;
    s_pendingCount = 0;
    xSemaphoreGive(s_mutex);
    LOG_W(TAG, "Istorija je obrisana");
}

size_t count(Resolution r) { return ringCount(ring(r)); }

size_t capacity(Resolution r) { return ring(r).capacity; }

uint32_t intervalSeconds(Resolution r) { return ring(r).interval; }

size_t pendingCount() { return s_pendingCount; }

size_t fsTotalBytes() { return s_fs.totalBytes(); }

size_t fsUsedBytes() { return s_fs.usedBytes(); }

bool fsReady() { return s_rings[0].ok; }

fs::FS &fs() { return s_fs; }

void forEach(Resolution r, uint32_t from, const std::function<void(const HistRecord &)> &fn) {
    Ring &rg = ring(r);
    if (!rg.ok) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Reader rd;
    HistRecord rec;
    size_t n = ringCount(rg);
    for (size_t i = 0; i < n; i++) {
        if (readRecordLocked(rg, rd, i, rec) && rec.ts >= from) fn(rec);
    }
    if (rd.f) rd.f.close();
    xSemaphoreGive(s_mutex);
}

size_t readRange(Resolution r, size_t start, HistRecord *out, size_t n) {
    Ring &rg = ring(r);
    if (!rg.ok) return 0;
    size_t got = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Reader rd;
    while (got < n && readRecordLocked(rg, rd, start + got, out[got])) got++;
    if (rd.f) rd.f.close();
    xSemaphoreGive(s_mutex);
    return got;
}

bool readAt(Resolution r, size_t index, HistRecord &out) {
    Ring &rg = ring(r);
    if (!rg.ok) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Reader rd;
    bool ok = readRecordLocked(rg, rd, index, out);
    if (rd.f) rd.f.close();
    xSemaphoreGive(s_mutex);
    return ok;
}

}  // namespace storage_manager
