#pragma once
// Portable streaming Overpass -> existing little-endian road-tile geometry.
// One way is held at a time; JSON is decoded one coordinate object at a time.
#include <ArduinoJson.h>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace road_tile_builder {
class BoundedAllocator : public ArduinoJson::Allocator {
    struct alignas(std::max_align_t) Header { size_t size; };
    size_t limit_, held_ = 0, peak_ = 0;
    void *(*alloc_)(size_t);
    void *(*realloc_)(void *, size_t);
    void (*free_)(void *);
public:
    BoundedAllocator(size_t limit, void *(*a)(size_t), void *(*r)(void *, size_t), void (*f)(void *))
        : limit_(limit), alloc_(a), realloc_(r), free_(f) {}
    size_t held() const { return held_; }
    size_t peak() const { return peak_; }
    void *allocate(size_t n) override {
        if (n > limit_ - held_) return nullptr;
        auto p = (Header *)alloc_(n + sizeof(Header));
        if (!p) return nullptr;
        p->size = n; held_ += n; if (held_ > peak_) peak_ = held_; return p + 1;
    }
    void deallocate(void *ptr) override {
        if (!ptr) return;
        auto p = (Header *)ptr - 1; held_ -= p->size; free_(p);
    }
    void *reallocate(void *ptr, size_t n) override {
        if (!ptr) return allocate(n);
        const size_t old = ((Header *)ptr - 1)->size;
        if (n > limit_ - (held_ - old)) return nullptr;
        auto p = (Header *)realloc_((Header *)ptr - 1, n + sizeof(Header));
        if (!p) return nullptr;
        held_ = held_ - old + n; if (held_ > peak_) peak_ = held_;
        p->size = n; return p + 1;
    }
};
// ArduinoJson creates a short-lived string buffer per deserializeJson() call.
// Retain one small block across coordinates; the upstream budget still accounts
// for its full capacity. Larger/overlapping allocations use the normal allocator.
class RecyclingAllocator : public ArduinoJson::Allocator {
    static constexpr size_t CAPACITY = 128;
    ArduinoJson::Allocator *upstream_;
    void *small_ = nullptr;
    bool busy_ = false;
    void *alloc(size_t n) { return upstream_ ? upstream_->allocate(n) : std::malloc(n); }
    void release(void *p) { if (upstream_) upstream_->deallocate(p); else std::free(p); }
public:
    explicit RecyclingAllocator(ArduinoJson::Allocator *upstream) : upstream_(upstream) {}
    RecyclingAllocator(const RecyclingAllocator &) = delete;
    RecyclingAllocator &operator=(const RecyclingAllocator &) = delete;
    ~RecyclingAllocator() { if (small_) release(small_); }
    void *allocate(size_t n) override {
        if (n <= CAPACITY && !busy_) {
            if (!small_) small_ = alloc(CAPACITY);
            if (small_) { busy_ = true; return small_; }
        }
        return alloc(n);
    }
    void deallocate(void *p) override {
        if (!p) return;
        if (p == small_) busy_ = false;
        else release(p);
    }
    void *reallocate(void *p, size_t n) override {
        if (!p) return allocate(n);
        if (p == small_) {
            if (n <= CAPACITY) return p;
            void *larger = alloc(n);
            if (larger) { std::memcpy(larger, p, CAPACITY); busy_ = false; }
            return larger;
        }
        return upstream_ ? upstream_->reallocate(p, n) : std::realloc(p, n);
    }
};
constexpr unsigned MAX_POINTS = 2048;
constexpr unsigned MAX_POLYS = 16000; // roads_sd's table capacity
struct Point { double lat, lon; };
struct Scratch {
    Point points[MAX_POINTS];
    uint16_t stack[MAX_POINTS * 2];
    bool keep[MAX_POINTS];
    int16_t encoded[MAX_POINTS * 2];
};

inline unsigned simplify(Scratch &s, unsigned n) {
    if (n < 2) return 0;
    memset(s.keep, 0, n);
    s.keep[0] = s.keep[n - 1] = true;
    unsigned top = 0;
    s.stack[top++] = 0; s.stack[top++] = (uint16_t)(n - 1);
    while (top) {
        const unsigned hi = s.stack[--top], lo = s.stack[--top];
        const Point a = s.points[lo], b = s.points[hi];
        const double dx = b.lon - a.lon, dy = b.lat - a.lat;
        const double length2 = dx * dx + dy * dy;
        double best = 0.0025 * 0.0025;
        unsigned index = 0;
        for (unsigned i = lo + 1; i < hi; ++i) {
            const Point p = s.points[i];
            double t = length2 ? ((p.lon-a.lon)*dx + (p.lat-a.lat)*dy)/length2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            const double ex = p.lon - a.lon - t * dx, ey = p.lat - a.lat - t * dy;
            if (ex * ex + ey * ey > best) { best = ex * ex + ey * ey; index = i; }
        }
        if (index) {
            s.keep[index] = true;
            s.stack[top++] = (uint16_t)lo; s.stack[top++] = (uint16_t)index;
            s.stack[top++] = (uint16_t)index; s.stack[top++] = (uint16_t)hi;
        }
    }
    unsigned count = 0;
    for (unsigned i = 0; i < n; ++i) if (s.keep[i]) {
        const int16_t lat = (int16_t)std::lround(s.points[i].lat * 180);
        const int16_t lon = (int16_t)std::lround(s.points[i].lon * 180);
        if (count && s.encoded[count*2-2] == lat && s.encoded[count*2-1] == lon) continue;
        s.encoded[count*2] = lat; s.encoded[count*2+1] = lon; ++count;
    }
    return count >= 2 ? count : 0;
}

template<class Input> struct Reader {
    Input &in;
    int cached = -2;
    int peek() { if (cached == -2) cached = in.read(); return cached; }
    int read() { const int c = peek(); cached = -2; return c; }
    size_t readBytes(char *dst, size_t n) {
        size_t i = 0;
        for (; i < n; ++i) { int c = read(); if (c < 0) break; dst[i] = (char)c; }
        return i;
    }
    int space() { int c; do { c = peek(); if (c == ' ' || c == '\r' || c == '\n' || c == '\t') read(); else return c; } while (true); }
    bool take(int c) { if (space() != c) return false; read(); return true; }
    bool string(char *out, size_t capacity) {
        if (!take('"')) return false;
        size_t n = 0; int c;
        while ((c = read()) != '"') {
            if (c < 32 || c == '\\' || n + 1 >= capacity) return false;
            out[n++] = (char)c;
        }
        out[n] = 0;
        return true;
    }
    // Skip metadata without retaining it. Strings and containers are bounded in depth;
    // the network reader independently bounds bytes and time.
    bool skip(unsigned depth = 0) {
        if (depth > 24) return false;
        int c = space();
        if (c == '"') {
            read();
            while ((c = read()) >= 0) {
                if (c == '"') return true;
                if (c < 32) return false;
                if (c == '\\' && read() < 0) return false;
            }
            return false;
        }
        if (c == '{' || c == '[') {
            read(); const int end = c == '{' ? '}' : ']';
            if (take(end)) return true;
            do {
                if (c == '{' && (!skip(depth+1) || !take(':'))) return false;
                if (!skip(depth+1)) return false;
                if (take(end)) return true;
            } while (take(','));
            return false;
        }
        unsigned n = 0;
        while ((c = peek()) >= 0 && c != ',' && c != '}' && c != ']' && c > 32) { read(); if (++n > 64) return false; }
        return n != 0;
    }
};

inline const char *json_error(DeserializationError e) {
    if (e == DeserializationError::NoMemory) return "road JSON: NoMemory";
    if (e == DeserializationError::IncompleteInput) return "road JSON: IncompleteInput";
    if (e == DeserializationError::EmptyInput) return "road JSON: EmptyInput";
    if (e == DeserializationError::TooDeep) return "road JSON: TooDeep";
    return "road JSON: InvalidInput";
}

template<class R>
bool read_way(R &r, Scratch &s, JsonVariant point, const JsonDocument &filter,
              unsigned &n, const char *&error) {
    if (!r.take('{')) return false;
    bool typeSeen = false, geometrySeen = false;
    n = 0;
    do {
        char key[64];
        if (!r.string(key, sizeof(key)) || !r.take(':')) return false;
        if (!strcmp(key, "type")) {
            char type[16];
            if (typeSeen || !r.string(type, sizeof(type)) || strcmp(type, "way")) return false;
            typeSeen = true;
        } else if (!strcmp(key, "geometry")) {
            if (geometrySeen || !r.take('[')) return false;
            geometrySeen = true;
            if (!r.take(']')) {
                do {
                    if (n == MAX_POINTS) { error = "road geometry exceeds point limit"; return false; }
                    if (r.space() != '{') return false;
                    // Decode just ONE lat/lon object, not the entire geometry array.
                    // This keeps JSON memory constant even for a long road.
                    const auto e = deserializeJson(point, r, DeserializationOption::Filter(filter),
                                                   DeserializationOption::NestingLimit(16));
                    if (e) { error = json_error(e); return false; }
                    if (!point["lat"].is<double>() || !point["lon"].is<double>()) return false;
                    const double lat = point["lat"], lon = point["lon"];
                    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
                    s.points[n++] = {lat, lon};
                } while (r.take(','));
                if (!r.take(']')) return false;
            }
        } else if (!r.skip()) return false;
        if (r.take('}')) return typeSeen && geometrySeen;
    } while (r.take(','));
    return false;
}

// Sink.line(int16 lat/lon pairs, point count) must consume/copy the data before returning.
// Any error, including an Overpass 'remark' on an otherwise HTTP-200 response, rejects
// the WHOLE tile. The caller publishes only after this function succeeds.
template<class Input, class Sink>
bool generate(Input &input, Sink &sink, Scratch &s, ArduinoJson::Allocator *allocator,
              const char *&error, char *serverRemark = nullptr, size_t remarkCapacity = 0) {
    Reader<Input> r { input };
    RecyclingAllocator reuse(allocator);
    JsonDocument doc(&reuse);
    // Deserialize into the root variant so coordinate-to-coordinate clearing
    // reuses slots instead of releasing/reallocating the document's memory pool.
    JsonVariant point = doc.to<JsonVariant>();
    JsonDocument filter;
    filter["lat"] = true;
    filter["lon"] = true;
    error = "invalid Overpass response";
    if (!r.take('{')) return false;
    bool found = false;
    unsigned polys = 0;
    do {
        char key[64];
        if (!r.string(key, sizeof(key))) return false;
        if (!r.take(':')) return false;
        if (!strcmp(key, "remark")) {
            error = "Overpass reported an error";
            if (serverRemark && remarkCapacity > 1) {
                const auto e = deserializeJson(doc, r, DeserializationOption::NestingLimit(2));
                if (!e && doc.is<const char *>()) {
                    const char *message = doc.as<const char *>();
                    size_t i = 0;
                    while (message[i] && i + 1 < remarkCapacity) {
                        serverRemark[i] = (unsigned char)message[i] < 32 ? ' ' : message[i]; ++i;
                    }
                    serverRemark[i] = 0;
                    if (i) error = serverRemark;
                }
            }
            return false; // a successful HTTP status never makes a partial tile usable
        }
        if (strcmp(key, "elements")) { if (!r.skip()) return false; }
        else {
            if (found || !r.take('[')) return false;
            found = true;
            if (!r.take(']')) {
                do {
                    unsigned n = 0;
                    if (!read_way(r, s, point, filter, n, error)) return false;
                    n = simplify(s, n);
                    if (n) {
                        if (++polys > MAX_POLYS) { error = "tile exceeds polyline limit"; return false; }
                        if (!sink.line(s.encoded, n)) { error = "tile output cancelled"; return false; }
                    }
                } while (r.take(','));
                if (!r.take(']')) return false;
            }
        }
        if (r.take('}')) {
            if (!found || r.space() >= 0) return false;
            error = nullptr;
            return true; // empty tiles are valid and cached too
        }
    } while (r.take(','));
    return false;
}
} // namespace road_tile_builder
