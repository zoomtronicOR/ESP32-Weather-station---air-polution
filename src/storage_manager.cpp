#include "storage_manager.h"

#include <LittleFS.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "log.h"
#include "time_manager.h"

namespace storage_manager {

static const char *TAG = "istorija";

static constexpr uint32_t MAGIC = 0x4D545231;  // "MTR1"
static constexpr uint16_t VERSION = 1;
static constexpr size_t METRICS = (size_t)MetricId::Count;

struct __attribute__((packed)) RingHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t metricCount;
    uint16_t recordSize;
    uint16_t capacity;
    uint16_t head;   // indeks sledećeg upisa
    uint16_t count;  // broj važećih zapisa
};

struct Ring {
    const char *path;
    uint16_t capacity;
    uint32_t interval;
    RingHeader hdr;
    bool ok;
};

static Ring s_rings[2] = {
    {"/hist5.bin", HISTORY_5MIN_RECORDS, 300, {}, false},
    {"/hist60.bin", HISTORY_HOUR_RECORDS, 3600, {}, false},
};

static SemaphoreHandle_t s_mutex = nullptr;

static Ring &ring(Resolution r) { return s_rings[r == Resolution::Min5 ? 0 : 1]; }

// ---------------------------------------------------------------------------
// Kružni fajl
// ---------------------------------------------------------------------------

static bool createRing(Ring &rg) {
    File f = LittleFS.open(rg.path, "w");
    if (!f) return false;
    rg.hdr = {MAGIC, VERSION, (uint16_t)METRICS, (uint16_t)sizeof(HistRecord), rg.capacity, 0, 0};
    f.write((const uint8_t *)&rg.hdr, sizeof(rg.hdr));
    // Fajl se odmah pravi u punoj veličini, da kasniji upisi ne menjaju njegovu dužinu
    HistRecord empty = {};
    for (uint16_t i = 0; i < rg.capacity; i++) {
        f.write((const uint8_t *)&empty, sizeof(empty));
        if ((i & 0x1F) == 0) esp_task_wdt_reset();  // prvo pravljenje fajla traje i do nekoliko sekundi
    }
    f.close();
    return true;
}

static bool openRing(Ring &rg) {
    size_t expected = sizeof(RingHeader) + (size_t)rg.capacity * sizeof(HistRecord);
    File f = LittleFS.open(rg.path, "r");
    if (f) {
        bool good = f.size() == expected && f.read((uint8_t *)&rg.hdr, sizeof(rg.hdr)) == sizeof(rg.hdr) &&
                    rg.hdr.magic == MAGIC && rg.hdr.version == VERSION && rg.hdr.metricCount == METRICS &&
                    rg.hdr.recordSize == sizeof(HistRecord) && rg.hdr.capacity == rg.capacity &&
                    rg.hdr.head < rg.capacity && rg.hdr.count <= rg.capacity;
        f.close();
        if (good) return true;
        // Promenjen format (npr. nova metrika) ili oštećen fajl -- istorija se počinje iznova
        LOG_W(TAG, "%s: nepoznat ili oštećen format, pravi se novi fajl", rg.path);
    }
    return createRing(rg);
}

static bool appendRecord(Ring &rg, const HistRecord &rec) {
    File f = LittleFS.open(rg.path, "r+");
    if (!f) return false;
    bool ok = f.seek(sizeof(RingHeader) + (size_t)rg.hdr.head * sizeof(HistRecord)) &&
              f.write((const uint8_t *)&rec, sizeof(rec)) == sizeof(rec);
    if (ok) {
        rg.hdr.head = (rg.hdr.head + 1) % rg.capacity;
        if (rg.hdr.count < rg.capacity) rg.hdr.count++;
        ok = f.seek(0) && f.write((const uint8_t *)&rg.hdr, sizeof(rg.hdr)) == sizeof(rg.hdr);
    }
    f.close();
    return ok;
}

static bool readRecordLocked(Ring &rg, File &f, size_t indexFromOldest, HistRecord &out) {
    if (indexFromOldest >= rg.hdr.count) return false;
    size_t oldest = (rg.hdr.head + rg.capacity - rg.hdr.count) % rg.capacity;
    size_t slot = (oldest + indexFromOldest) % rg.capacity;
    return f.seek(sizeof(RingHeader) + slot * sizeof(HistRecord)) &&
           f.read((uint8_t *)&out, sizeof(out)) == sizeof(out);
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
    File f = LittleFS.open(rg.path, "r");
    if (!f) return;
    HistRecord rec;
    size_t n = rg.hdr.count;
    size_t start = n > 12 ? n - 12 : 0;
    for (size_t i = start; i < n; i++) {
        if (!readRecordLocked(rg, f, i, rec) || rec.ts / 3600 != hour) continue;
        for (size_t m = 0; m < METRICS; m++) {
            const MetricAgg &a = rec.m[m];
            if (a.count == 0) continue;
            s_hourAcc[m].sum += (double)a.avg * a.count;
            s_hourAcc[m].count += a.count;
            s_hourAcc[m].min = min(s_hourAcc[m].min, a.min);
            s_hourAcc[m].max = max(s_hourAcc[m].max, a.max);
        }
    }
    f.close();
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
    if (!LittleFS.begin(true)) {
        LOG_E(TAG, "LittleFS nije dostupan -- istorija se ne čuva");
        return;
    }
    for (Ring &rg : s_rings) {
        rg.ok = openRing(rg);
        if (rg.ok) {
            LOG_I(TAG, "%s: %u / %u zapisa (interval %lu s)", rg.path, rg.hdr.count, rg.capacity,
                  (unsigned long)rg.interval);
        } else {
            LOG_E(TAG, "%s: fajl nije dostupan", rg.path);
        }
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
        // Vreme je upravo postalo pouzdano: tekući interval se zatvara, a zatim se prelazi na poravnate
        closeBucket(now);
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
    for (Ring &rg : s_rings) {
        LittleFS.remove(rg.path);
        rg.ok = createRing(rg);
    }
    accReset(s_hourAcc);
    s_hourId = 0;
    s_pendingCount = 0;
    xSemaphoreGive(s_mutex);
    LOG_W(TAG, "Istorija je obrisana");
}

size_t count(Resolution r) { return ring(r).hdr.count; }

size_t capacity(Resolution r) { return ring(r).capacity; }

uint32_t intervalSeconds(Resolution r) { return ring(r).interval; }

size_t pendingCount() { return s_pendingCount; }

void forEach(Resolution r, uint32_t from, const std::function<void(const HistRecord &)> &fn) {
    Ring &rg = ring(r);
    if (!rg.ok) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    File f = LittleFS.open(rg.path, "r");
    if (f) {
        HistRecord rec;
        for (size_t i = 0; i < rg.hdr.count; i++) {
            if (readRecordLocked(rg, f, i, rec) && rec.ts >= from) fn(rec);
        }
        f.close();
    }
    xSemaphoreGive(s_mutex);
}

size_t readRange(Resolution r, size_t start, HistRecord *out, size_t n) {
    Ring &rg = ring(r);
    if (!rg.ok) return 0;
    size_t got = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    File f = LittleFS.open(rg.path, "r");
    if (f) {
        while (got < n && readRecordLocked(rg, f, start + got, out[got])) got++;
        f.close();
    }
    xSemaphoreGive(s_mutex);
    return got;
}

bool readAt(Resolution r, size_t index, HistRecord &out) {
    Ring &rg = ring(r);
    if (!rg.ok) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = false;
    File f = LittleFS.open(rg.path, "r");
    if (f) {
        ok = readRecordLocked(rg, f, index, out);
        f.close();
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

}  // namespace storage_manager
