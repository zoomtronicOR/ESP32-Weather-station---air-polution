#include "history_api.h"

#include <ArduinoJson.h>
#include <memory>
#include <vector>

#include "config.h"
#include "metrics.h"
#include "storage_manager.h"
#include "time_manager.h"

namespace history_api {

using storage_manager::HistRecord;
using storage_manager::Resolution;

static constexpr size_t METRICS = (size_t)MetricId::Count;

struct Period {
    const char *name;
    uint32_t seconds;
    Resolution res;
};

static const Period PERIODS[] = {
    {"1h", 3600, Resolution::Min5},
    {"6h", 6 * 3600, Resolution::Min5},
    {"24h", 24 * 3600, Resolution::Min5},
    {"7d", 7 * 86400, Resolution::Hour},
    {"30d", 30 * 86400, Resolution::Hour},
};

static const Period &findPeriod(AsyncWebServerRequest *req) {
    if (req->hasParam("period")) {
        String p = req->getParam("period")->value();
        for (const Period &pd : PERIODS) {
            if (p == pd.name) return pd;
        }
    }
    return PERIODS[2];  // 24h
}

static std::vector<uint8_t> parseMetrics(AsyncWebServerRequest *req) {
    std::vector<uint8_t> out;
    if (req->hasParam("metrics")) {
        String list = req->getParam("metrics")->value();
        int start = 0;
        while (start <= (int)list.length()) {
            int comma = list.indexOf(',', start);
            if (comma < 0) comma = list.length();
            String key = list.substring(start, comma);
            for (size_t i = 0; i < METRICS; i++) {
                if (key == metrics::def((MetricId)i).key) out.push_back(i);
            }
            start = comma + 1;
        }
    }
    if (out.empty()) {
        for (size_t i = 0; i < METRICS; i++) out.push_back(i);
    }
    return out;
}

static void appendNumber(String &s, const storage_manager::MetricAgg &a, float v, uint8_t decimals) {
    if (a.count == 0 || isnan(v)) {
        s += "null";
        return;
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    s += buf;
}

// ---------------------------------------------------------------------------
// Strimovanje zapisa u delovima
// ---------------------------------------------------------------------------

enum class Format : uint8_t { HistoryJson, ExportCsv, ExportJson };

struct Stream {
    Format format;
    Resolution res;
    uint32_t from;
    size_t next;   // redni broj sledećeg zapisa (od najstarijeg)
    size_t end;
    std::vector<uint8_t> metricIdx;
    String pending;
    uint8_t stage = 0;  // 0 = zaglavlje, 1 = zapisi, 2 = kraj, 3 = gotovo
    bool firstRow = true;
    String header;
};

static void appendRow(Stream &st, const HistRecord &rec) {
    String &s = st.pending;
    if (st.format == Format::ExportCsv) {
        char t[32];
        time_manager::formatIsoUtc(rec.ts, t, sizeof(t));
        s += t;
        s += ',';
        time_manager::formatIsoLocal(rec.ts, t, sizeof(t));
        s += t;
        for (uint8_t i : st.metricIdx) {
            const MetricDef &d = metrics::def((MetricId)i);
            const storage_manager::MetricAgg &a = rec.m[i];
            s += ',';
            if (a.count) appendNumber(s, a, a.avg, d.decimals + 1);
            s += ',';
            if (a.count) appendNumber(s, a, a.min, d.decimals);
            s += ',';
            if (a.count) appendNumber(s, a, a.max, d.decimals);
        }
        s += "\r\n";
        return;
    }
    // JSON red: [ts, avg, min, max, avg, min, max, ...]
    if (!st.firstRow) s += ',';
    st.firstRow = false;
    s += '[';
    s += String(rec.ts);
    for (uint8_t i : st.metricIdx) {
        const MetricDef &d = metrics::def((MetricId)i);
        const storage_manager::MetricAgg &a = rec.m[i];
        s += ',';
        appendNumber(s, a, a.avg, d.decimals + 1);
        s += ',';
        appendNumber(s, a, a.min, d.decimals);
        s += ',';
        appendNumber(s, a, a.max, d.decimals);
    }
    s += ']';
}

static void produce(Stream &st, size_t want) {
    while (st.pending.length() < want && st.stage < 3) {
        if (st.stage == 0) {
            st.pending += st.header;
            st.stage = 1;
        } else if (st.stage == 1) {
            HistRecord recs[4];
            size_t n = st.next < st.end ? storage_manager::readRange(st.res, st.next, recs, min<size_t>(4, st.end - st.next)) : 0;
            if (n == 0) {
                st.stage = 2;
                continue;
            }
            for (size_t i = 0; i < n; i++) {
                if (recs[i].ts >= st.from) appendRow(st, recs[i]);
            }
            st.next += n;
        } else {
            if (st.format != Format::ExportCsv) st.pending += "]}";
            st.stage = 3;
        }
    }
}

static void sendStream(AsyncWebServerRequest *req, std::shared_ptr<Stream> st, const char *contentType,
                       const char *filename) {
    AsyncWebServerResponse *res = req->beginChunkedResponse(contentType, [st](uint8_t *buf, size_t maxLen, size_t) -> size_t {
        produce(*st, maxLen);
        size_t n = min(maxLen, (size_t)st->pending.length());
        memcpy(buf, st->pending.c_str(), n);
        st->pending.remove(0, n);
        return n;  // 0 = kraj odgovora
    });
    res->addHeader("Cache-Control", "no-store");
    if (filename) {
        res->addHeader("Content-Disposition", String("attachment; filename=\"") + filename + "\"");
    }
    req->send(res);
}

static std::shared_ptr<Stream> makeStream(Format format, const Period &pd, std::vector<uint8_t> metricIdx) {
    auto st = std::make_shared<Stream>();
    st->format = format;
    st->res = pd.res;
    uint32_t now = (uint32_t)time_manager::nowUtc();
    st->from = now > pd.seconds ? now - pd.seconds : 0;
    size_t count = storage_manager::count(pd.res);
    size_t maxPoints = pd.seconds / storage_manager::intervalSeconds(pd.res) + 2;
    st->next = count > maxPoints ? count - maxPoints : 0;  // stariji zapisi se ionako filtriraju po vremenu
    st->end = count;
    st->metricIdx = std::move(metricIdx);
    return st;
}

static String metricsHeaderJson(const std::vector<uint8_t> &idx) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i : idx) {
        const MetricDef &d = metrics::def((MetricId)i);
        JsonObject o = arr.add<JsonObject>();
        o["key"] = d.key;
        o["name"] = d.name;
        o["unit"] = d.unit;
        o["decimals"] = d.decimals;
    }
    String s;
    serializeJson(doc, s);
    return s;
}

static void handleHistory(AsyncWebServerRequest *req) {
    const Period &pd = findPeriod(req);
    auto st = makeStream(Format::HistoryJson, pd, parseMetrics(req));
    st->header = String("{\"period\":\"") + pd.name + "\",\"resolution_s\":" +
                 String(storage_manager::intervalSeconds(pd.res)) + ",\"time_valid\":" +
                 (time_manager::isValid() ? "true" : "false") + ",\"now\":" + String((uint32_t)time_manager::nowUtc()) +
                 ",\"local_estimate\":true,\"fields\":[\"avg\",\"min\",\"max\"],\"metrics\":" +
                 metricsHeaderJson(st->metricIdx) + ",\"points\":[";
    sendStream(req, st, "application/json", nullptr);
}

static void handleExport(AsyncWebServerRequest *req) {
    const Period &pd = findPeriod(req);
    bool csv = !req->hasParam("format") || req->getParam("format")->value() != "json";
    auto st = makeStream(csv ? Format::ExportCsv : Format::ExportJson, pd, parseMetrics(req));
    char filename[48];
    snprintf(filename, sizeof(filename), "meteo_%s_%s.%s", DEVICE_NAME, pd.name, csv ? "csv" : "json");
    if (csv) {
        String h = "time_utc,time_local";
        for (uint8_t i : st->metricIdx) {
            const char *k = metrics::def((MetricId)i).key;
            h += String(',') + k + "_avg," + k + "_min," + k + "_max";
        }
        st->header = h + "\r\n";
        sendStream(req, st, "text/csv; charset=utf-8", filename);
    } else {
        st->header = String("{\"device\":\"") + DEVICE_NAME + "\",\"period\":\"" + pd.name + "\",\"resolution_s\":" +
                     String(storage_manager::intervalSeconds(pd.res)) +
                     ",\"note\":\"lokalno izmerene i izracunate vrednosti\",\"fields\":[\"avg\",\"min\",\"max\"],\"metrics\":" +
                     metricsHeaderJson(st->metricIdx) + ",\"points\":[";
        sendStream(req, st, "application/json", filename);
    }
}

// ---------------------------------------------------------------------------
// Statistika (specifikacija §5.4)
// ---------------------------------------------------------------------------

struct Window {
    uint32_t from;
    uint32_t to;
};

struct WinAcc {
    double sum;
    uint32_t count;
    float min;
    float max;
};

static void accAdd(WinAcc &a, const storage_manager::MetricAgg &m) {
    if (m.count == 0) return;
    a.sum += (double)m.avg * m.count;
    a.count += m.count;
    a.min = min(a.min, m.min);
    a.max = max(a.max, m.max);
}

// Prozori: [0] period, [1] prethodni period, [2] prva trećina, [3] poslednja trećina
struct PeriodStats {
    const char *name;
    uint32_t seconds;
    Resolution res;
    Resolution prevRes;
    WinAcc acc[4][METRICS];
    Window win[4];
};

static void initPeriod(PeriodStats &p, uint32_t now) {
    for (auto &w : p.acc) {
        for (auto &a : w) a = {0.0, 0, INFINITY, -INFINITY};
    }
    uint32_t start = now > p.seconds ? now - p.seconds : 0;
    p.win[0] = {start, now};
    p.win[1] = {start > p.seconds ? start - p.seconds : 0, start};
    p.win[2] = {start, start + p.seconds / 3};
    p.win[3] = {now - p.seconds / 3, now};
}

static void feed(PeriodStats &p, const HistRecord &rec, Resolution res) {
    // Zapis pripada prozoru po početku svog intervala (prvi zapis posle pokretanja može biti kraći)
    for (int w = 0; w < 4; w++) {
        Resolution want = w == 1 ? p.prevRes : p.res;
        if (want != res || rec.ts < p.win[w].from || rec.ts >= p.win[w].to) continue;
        for (size_t i = 0; i < METRICS; i++) accAdd(p.acc[w][i], rec.m[i]);
    }
}

static void handleStatistics(AsyncWebServerRequest *req) {
    uint32_t nowMs = millis();
    uint32_t now = (uint32_t)time_manager::nowUtc();

    // 1 h i 24 h iz 5-minutnih zapisa; prethodnih 24 h, 7 i 30 dana iz satnih
    static PeriodStats *periods = nullptr;
    if (!periods) {
        periods = new PeriodStats[4]{
            {"1h", 3600, Resolution::Min5, Resolution::Min5, {}, {}},
            {"24h", 86400, Resolution::Min5, Resolution::Hour, {}, {}},
            {"7d", 7 * 86400, Resolution::Hour, Resolution::Hour, {}, {}},
            {"30d", 30 * 86400, Resolution::Hour, Resolution::Hour, {}, {}},
        };
    }
    for (int p = 0; p < 4; p++) initPeriod(periods[p], now);

    if (time_manager::isValid()) {
        storage_manager::forEach(Resolution::Min5, now - 2 * 86400, [&](const HistRecord &rec) {
            for (int p = 0; p < 4; p++) feed(periods[p], rec, Resolution::Min5);
        });
        storage_manager::forEach(Resolution::Hour, now > 60 * 86400 ? now - 60 * 86400 : 0, [&](const HistRecord &rec) {
            for (int p = 0; p < 4; p++) feed(periods[p], rec, Resolution::Hour);
        });
    }

    JsonDocument doc;
    doc["time_valid"] = time_manager::isValid();
    doc["local_estimate"] = true;
    doc["note"] = "Lokalno izračunata statistika; nije zvanična meteorološka prognoza.";
    JsonArray list = doc["metrics"].to<JsonArray>();
    char buf[20];
    auto num = [&](JsonVariant dst, double v, uint8_t dec) {
        snprintf(buf, sizeof(buf), "%.*f", dec, v);
        dst.set(serialized(String(buf)));
    };

    for (size_t i = 0; i < METRICS; i++) {
        MetricId id = (MetricId)i;
        const MetricDef &d = metrics::def(id);
        JsonObject m = list.add<JsonObject>();
        m["key"] = d.key;
        m["name"] = d.name;
        m["unit"] = d.unit;
        m["decimals"] = d.decimals;
        if (metrics::isFresh(id, nowMs)) {
            m["current"] = serialized(String(metrics::get(id).value, (unsigned int)d.decimals));
        } else {
            m["current"] = nullptr;
        }
        const Reading &r = metrics::get(id);
        if (r.everUpdated && time_manager::isValid()) {
            char ts[24];
            time_manager::formatIsoUtc(time_manager::epochFromUptime(r.timestampMs), ts, sizeof(ts));
            m["last_reading_utc"] = ts;
        }

        JsonObject per = m["periods"].to<JsonObject>();
        for (int p = 0; p < 4; p++) {
            PeriodStats &ps = periods[p];
            JsonObject o = per[ps.name].to<JsonObject>();
            const WinAcc &a = ps.acc[0][i];
            o["samples"] = a.count;
            if (a.count == 0) continue;
            double avg = a.sum / a.count;
            num(o["avg"].to<JsonVariant>(), avg, d.decimals + 1);
            num(o["min"].to<JsonVariant>(), a.min, d.decimals);
            num(o["max"].to<JsonVariant>(), a.max, d.decimals);
            const WinAcc &prev = ps.acc[1][i];
            if (prev.count) num(o["change"].to<JsonVariant>(), avg - prev.sum / prev.count, d.decimals + 1);
            const WinAcc &first = ps.acc[2][i], &last = ps.acc[3][i];
            if (first.count && last.count) {
                double diff = last.sum / last.count - first.sum / first.count;
                o["trend"] = diff > d.trendDelta ? "rising" : (diff < -d.trendDelta ? "falling" : "stable");
            }
        }
    }

    JsonObject h = doc["history"].to<JsonObject>();
    h["records_5min"] = storage_manager::count(Resolution::Min5);
    h["capacity_5min"] = storage_manager::capacity(Resolution::Min5);
    h["records_hour"] = storage_manager::count(Resolution::Hour);
    h["capacity_hour"] = storage_manager::capacity(Resolution::Hour);
    h["pending"] = storage_manager::pendingCount();

    AsyncResponseStream *res = req->beginResponseStream("application/json");
    res->addHeader("Cache-Control", "no-store");
    serializeJson(doc, *res);
    req->send(res);
}

void registerRoutes(AsyncWebServer &server) {
    // Podputanja pre roditeljske rute (AsyncURIMatcher)
    server.on("/api/v1/history/export", HTTP_GET, handleExport);
    server.on("/api/v1/history", HTTP_GET, handleHistory);
    server.on("/api/v1/statistics", HTTP_GET, handleStatistics);
}

}  // namespace history_api
