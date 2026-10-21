#include "road_tile_builder.h"
#include "road_download_stream.h"
#include "road_tile_plan.h"
#include "road_tile_clip.h"
#include "road_tile_upload.h"
#include <cassert>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

struct Input {
    const std::string &s; size_t pos = 0;
    int read() { return pos < s.size() ? (unsigned char)s[pos++] : -1; }
};
struct Sink {
    std::vector<std::vector<int16_t>> lines;
    bool line(const int16_t *p, unsigned n) { lines.emplace_back(p, p + 2*n); return true; }
};
struct ClippedSink {
    Sink &sink; int lat, lon, span = 1;
    bool line(const int16_t *p, unsigned n) { return road_tile_plan::clip(p, n, lat, lon, sink, span); }
};
static std::string binary(const Sink &sink) {
    std::string bytes;
    auto word = [&](unsigned n) { bytes += (char)n; bytes += (char)(n>>8); };
    unsigned points = 0;
    for (const auto &line : sink.lines) points += (unsigned)line.size()/2;
    word((unsigned)sink.lines.size()); word(points); word(points>>16);
    for (const auto &line : sink.lines) word((unsigned)line.size()/2);
    for (const auto &line : sink.lines) for (int16_t p : line) word((uint16_t)p);
    return bytes;
}
static void test_upload() {
    struct File {
        const std::string &data; size_t pos = 0;
        size_t read(uint8_t *out, size_t n) {
            if (n > data.size()-pos) n = data.size()-pos;
            memcpy(out,data.data()+pos,n); pos += n; return n;
        }
    };
    const Sink tile {{{7200,-20160,7245,-20115}}};
    const std::string good = binary(tile), empty(6,0);
    auto valid = [&](const std::string &bytes) { File f {bytes}; return road_tile_upload::validate(f,bytes.size(),160,-448,1); };
    assert(valid(good) && valid(empty));
    for (size_t i=0; i<good.size(); ++i) assert(!valid(good.substr(0,i)));
    std::string bad = good; bad[6] = 3; assert(!valid(bad)); // count sum mismatch
    bad = good; bad[8] = 0; bad[9] = 0; assert(!valid(bad)); // outside cell
    bad = good; bad[2] = 3; assert(!valid(bad)); // header size mismatch
    bad = empty; bad += 'x'; assert(!valid(bad));
    bad.assign(road_tile_upload::MAX_BYTES+1,0); assert(!valid(bad));
}
static void test_plan_and_clip() {
    using namespace road_tile_plan;
    Cell cells[MAX_CELLS];
    const unsigned count = build(40.2338, -111.6585, 58, cells);
    assert(count > 1 && count <= MAX_CELLS);
    assert(cells[0].lat == 160 && cells[0].lon == -447);
    assert(degree(-447) == -112 && degree(-448) == -112 && degree(-449) == -113);
    for (unsigned i = 0; i < count; ++i) {
        assert(cells[i].lat >= 156 && cells[i].lat < 168);
        assert(cells[i].lon >= -452 && cells[i].lon < -440);
        for (unsigned j = 0; j < i; ++j) assert(cells[i].lat != cells[j].lat || cells[i].lon != cells[j].lon);
    }
    assert(cells[1].lat == 160 && cells[1].lon == -448 && cells[1].span == 4);
    assert(build(0.125, 0.125, 1, cells) == 2);
    assert(build(0, 0, 10000, cells) == MAX_CELLS);
    assert(build(90, 0, 10, cells) == 0 && build(0, 0, -1, cells) == 0);
    const unsigned polar = build(89.99, 179.99, 58, cells);
    assert(polar > 0);
    for (unsigned i = 0; i < polar; ++i) assert(cells[i].lat < 360 && cells[i].lon < 720);
    char path[48]; road_tile_plan::path(path, sizeof(path), 160, -447, 1);
    assert(!strcmp(path, "/roads/q160_-447_t.bin"));
    road_tile_plan::path(path, sizeof(path), 160, -448, 0, 4);
    assert(!strcmp(path, "/roads/d40_-112_m.bin"));
    // Crossing a whole cell, including two outside endpoints; the adjacent cell
    // shares only the endpoint, never the entire road.
    const int16_t crossing[] = {20, -10, 20, 100};
    Sink left, right;
    assert(clip(crossing, 2, 0, 0, left) && clip(crossing, 2, 0, 1, right));
    assert((left.lines == std::vector<std::vector<int16_t>>{{20,0,20,45}}));
    assert((right.lines == std::vector<std::vector<int16_t>>{{20,45,20,90}}));
    Sink degree;
    assert(clip(crossing, 2, 0, 0, degree, 4));
    assert((degree.lines == std::vector<std::vector<int16_t>>{{20,0,20,100}}));
    const int16_t outside[] = {-20, 10, -10, 20};
    Sink empty; assert(clip(outside, 2, 0, 0, empty) && empty.lines.empty());
    const int16_t broken[] = {10,10,60,10,60,30,10,30};
    Sink split; assert(clip(broken, 4, 0, 0, split) && split.lines.size() == 2);
    std::vector<int16_t> longRun;
    for (int i = 0; i < 600; ++i) { longRun.push_back(20); longRun.push_back(i % 40); }
    Sink chunked; assert(clip(longRun.data(), 600, 0, 0, chunked));
    assert(chunked.lines.size() == 3);
    size_t segments = 0;
    for (const auto &line : chunked.lines) { assert(line.size() <= 512); segments += line.size()/2 - 1; }
    assert(segments == 599);
    struct Reject { bool line(const int16_t *, unsigned) { return false; } } reject;
    assert(!clip(crossing, 2, 0, 0, reject));
}
struct RejectAllocator : ArduinoJson::Allocator {
    void *allocate(size_t) override { return nullptr; }
    void deallocate(void *) override {}
    void *reallocate(void *, size_t) override { return nullptr; }
};
struct CountingAllocator : ArduinoJson::Allocator {
    road_tile_builder::BoundedAllocator budget {96*1024, std::malloc, std::realloc, std::free};
    unsigned allocations = 0, reallocations = 0;
    void *allocate(size_t n) override { ++allocations; return budget.allocate(n); }
    void deallocate(void *p) override { budget.deallocate(p); }
    void *reallocate(void *p, size_t n) override { ++reallocations; return budget.reallocate(p,n); }
};
static void test_recycling_allocator() {
    CountingAllocator counted;
    {
        road_tile_builder::RecyclingAllocator reuse(&counted);
        auto smallBlock = static_cast<unsigned char *>(reuse.allocate(32));
        assert(smallBlock); memset(smallBlock, 0x5a, 32);
        void *overlap = reuse.allocate(64); assert(overlap && overlap != smallBlock);
        auto larger = static_cast<unsigned char *>(reuse.reallocate(smallBlock, 256));
        assert(larger && larger != smallBlock);
        for (unsigned i = 0; i < 32; ++i) assert(larger[i] == 0x5a);
        void *again = reuse.allocate(32); assert(again == smallBlock);
        reuse.deallocate(again); reuse.deallocate(larger); reuse.deallocate(overlap);
        assert(counted.budget.held() == 128); // one reserved block, freed by destructor
        void *p = reuse.allocate(32);
        assert(!reuse.reallocate(p, 96*1024+1)); // failure preserves original data/ownership
        reuse.deallocate(p);
    }
    assert(counted.budget.held() == 0);
}
static road_tile_builder::Scratch scratch;
static bool build(const std::string &s, Sink &sink, ArduinoJson::Allocator *a = nullptr, const char **error = nullptr) {
    Input in {s}; const char *err = nullptr;
    const bool ok = road_tile_builder::generate(in, sink, scratch, a, err);
    if (error) *error = err;
    return ok;
}
struct TestClock {
    uint32_t tick = 0;
    uint32_t now() { return tick; }
    uint32_t micros() { return tick * 1000; }
    void pause() { ++tick; }
};
struct TestTransport {
    TestClock &clock;
    std::string data;
    size_t position = 0;
    bool retryRead = true, complete = true, keepOpen = false, cancel = false, badChunks = false;
    int transientResult = 0;
    uint32_t readDelay = 0;
    uint32_t initialWait = 3, gapUntil = 12;
    int available() {
        if (clock.now() < initialWait || (position >= 32 && clock.now() < gapUntil)) return 0;
        return (int)(data.size() - position);
    }
    int read(uint8_t *dst, size_t n) {
        clock.tick += readDelay;
        if (retryRead) { retryRead = false; return transientResult; }
        // Exercise short reads as well as empty reads and gaps between packets.
        n = n < 17 ? n : 17;
        memcpy(dst, data.data() + position, n); position += n;
        return (int)n;
    }
    bool connected() { return position < data.size() || keepOpen; }
    bool finished() { return complete && position == data.size(); }
    bool failed() { return badChunks; }
    bool cancelled() { return cancel; }
};
static void test_stream() {
    const std::string data(3000, 'x');
    TestClock clock;
    TestTransport source {clock, data};
    source.transientResult = -1;
    source.readDelay = 2;
    roads_download::BufferedInput<TestTransport, TestClock> input(source, clock, 20, 1000, 4000);
    std::string got; int c;
    while ((c = input.read()) >= 0) got += (char)c;
    assert(got == data && !input.error && input.bytes == data.size());
    assert(input.readCalls == 178 && input.emptyPolls > 0);
    assert(input.idleUs == input.emptyPolls*1000);
    assert(input.transportUs == input.idleUs + input.readCalls*2000);
    for (unsigned test = 0; test < 5; ++test) {
        TestClock t;
        TestTransport s {t, test == 3 ? "abc" : ""};
        s.complete = false; s.keepOpen = true; s.initialWait = 0;
        if (test == 1) { s.initialWait = 100; }
        if (test == 2) { s.keepOpen = false; }
        if (test == 4) { s.badChunks = true; }
        roads_download::BufferedInput<TestTransport, TestClock> in(s, t, 10, test == 1 ? 5 : 1000, 2);
        while (in.read() >= 0) {}
        const char *expected[] = {"map body idle timeout", "map body total timeout",
            "map connection closed before response completed", "map response exceeds byte limit", "invalid HTTP chunk framing"};
        assert(in.error && !strcmp(in.error, expected[test]));
    }
    TestClock wrap {UINT32_MAX - 3};
    TestTransport stalled {wrap, ""}; stalled.complete = false; stalled.keepOpen = true; stalled.initialWait = 0;
    roads_download::BufferedInput<TestTransport, TestClock> timeout(stalled, wrap, 8, 100);
    assert(timeout.read() == -1 && !strcmp(timeout.error, "map body idle timeout"));
    assert(timeout.transportUs == 8000 && timeout.idleUs == 8000);
}
int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_stream();
    test_recycling_allocator();
    test_plan_and_clip();
    test_upload();
    const std::string way = R"({"type":"way","id":1,"nodes":[1,2,3],"geometry":[{"lat":37.1,"lon":-113.2},{"lat":37.15,"lon":-113.2},{"lat":37.2,"lon":-113.2}]})";
    Sink sink;
    assert(build("{\"version\":0.6,\"osm3s\":{\"copyright\":\"a \\\"quote\\\"\"},\"elements\":[" + way + "]}", sink));
    assert(sink.lines.size() == 1 && sink.lines[0].size() == 4);
    assert(sink.lines[0][0] == 6678 && sink.lines[0][1] == -20376);
    assert(sink.lines[0][2] == 6696);
    Sink empty;
    assert(build("{ \"elements\" : [ ] }\n", empty));
    assert(empty.lines.empty());
    assert(build("{\"elements\":[" + way + "," + way + "]}", empty));
    assert(empty.lines.size() == 2);
    for (const std::string bad : {
        "{}", "{\"elements\":[]", "{\"elements\":[]}junk", "{\"elements\":[],\"elements\":[]}",
        "{\"remark\":\"timeout\",\"elements\":[]}", "{\"elements\":[],\"remark\":\"runtime error\"}",
        "{\"elements\":[{\"type\":\"way\",\"geometry\":[{\"lat\":91,\"lon\":1}]}]}",
        "{\"elements\":[{\"type\":\"way\",\"geometry\":[{\"lat\":null,\"lon\":1}]}]}",
        "{\"elements\":[{\"type\":\"way\",\"geometry\":[{\"lat\":1}]}]}"
    }) { Sink out; assert(!build(bad, out)); }
    // Every truncated prefix must fail, even after a complete way has been emitted.
    const std::string full = "{\"elements\":[" + way + "]}";
    TestClock streamClock;
    TestTransport streamingSource {streamClock, full};
    roads_download::BufferedInput<TestTransport, TestClock> streamingInput(streamingSource, streamClock);
    Sink streamed; const char *streamError = nullptr;
    assert(road_tile_builder::generate(streamingInput, streamed, scratch, nullptr, streamError));
    assert(!streamingInput.error && streamed.lines.size() == 1);
    for (size_t i = 0; i < full.size(); ++i) { Sink out; assert(!build(full.substr(0, i), out)); }
    RejectAllocator reject;
    Sink out; const char *error = nullptr;
    assert(!build(full, out, &reject, &error));
    assert(!strcmp(error, "road JSON: NoMemory"));
    const std::string serverFailure = R"({"version":0.6,"remark":"runtime error: Query ran out of memory in \"query\".\nIt would need at least 32 MB of RAM to continue.","elements":[]})";
    Input failureInput {serverFailure};
    Sink failedTile;
    char serverRemark[160];
    assert(!road_tile_builder::generate(failureInput, failedTile, scratch, nullptr, error,
                                        serverRemark, sizeof(serverRemark)));
    assert(error == serverRemark && strstr(error, "32 MB") && !strchr(error, '\n'));
    assert(failedTile.lines.empty());
    Input shortFailure {serverFailure}; char smallRemark[12];
    assert(!road_tile_builder::generate(shortFailure, failedTile, scratch, nullptr, error,
                                        smallRemark, sizeof(smallRemark)));
    assert(error == smallRemark && strlen(error) == sizeof(smallRemark)-1);
    road_tile_builder::BoundedAllocator budget(96 * 1024, std::malloc, std::realloc, std::free);
    Sink bounded; assert(build(full, bounded, &budget));
    assert(budget.held() == 0 && budget.peak() > 0 && budget.peak() <= 96 * 1024);
    assert(budget.allocate(96 * 1024 + 1) == nullptr);
    void *block = budget.allocate(100);
    assert(block && budget.held() == 100);
    assert(budget.reallocate(block, 96 * 1024 + 1) == nullptr);
    assert(budget.held() == 100);
    block = budget.reallocate(block, 200);
    assert(block && budget.held() == 200);
    budget.deallocate(block); assert(budget.held() == 0);
    // Quantized duplicates must not create a zero-length road.
    Sink duplicates;
    assert(build(R"({"elements":[{"type":"way","geometry":[{"lat":1,"lon":2},{"lat":1.00001,"lon":2}]}]})", duplicates));
    assert(duplicates.lines.empty());
    // Preserve a bend while removing intermediate collinear points.
    scratch.points[0] = {0,0}; scratch.points[1] = {0,0.5};
    scratch.points[2] = {0,1}; scratch.points[3] = {1,1};
    assert(road_tile_builder::simplify(scratch, 4) == 3);
    std::string many = "{\"elements\":[{\"type\":\"way\",\"geometry\":[";
    for (unsigned i = 0; i <= road_tile_builder::MAX_POINTS; ++i) {
        if (i) many += ',';
        many += "{\"lat\":1,\"lon\":2}";
    }
    many += "]}]}";
    Sink oversized; assert(!build(many, oversized));
    // A maximum-length road now decodes one point at a time, below an 8KB JSON
    // budget instead of building an entire large geometry array in the document.
    const size_t last = many.rfind(",{\"lat\":1,\"lon\":2}");
    many.erase(last, strlen(",{\"lat\":1,\"lon\":2}"));
    road_tile_builder::BoundedAllocator smallBudget(8192, std::malloc, std::realloc, std::free);
    Sink longRoad; assert(build(many, longRoad, &smallBudget));
    assert(smallBudget.held() == 0 && smallBudget.peak() <= 8192);
    CountingAllocator counted;
    Sink reused;
    assert(build(many, reused, &counted));
    assert(counted.allocations == 2 && counted.reallocations == 0 && counted.budget.held() == 0);
    // A long filtered key exercises growing the recycled string buffer beyond
    // its capacity, then using it again for ordinary coordinates.
    const std::string extra = "{\"elements\":[{\"type\":\"way\",\"geometry\":[{\"" + std::string(300,'x') +
        "\":\"ignored\",\"lat\":37.1,\"lon\":-113.2},{\"lat\":37.2,\"lon\":-113.2}]}]}";
    Sink longKey;
    assert(build(extra, longKey, &counted));
    assert(longKey.lines == sink.lines && counted.budget.held() == 0);
    if (argc == 2 || argc == 4 || argc == 6) {
        std::ifstream f(argv[1], std::ios::binary);
        assert(f.good());
        const std::string response((std::istreambuf_iterator<char>(f)), {});
        Sink live; assert(build(response, live, &budget));
        assert(budget.held() == 0);
        std::cout << "Live fixture: " << live.lines.size() << " roads; peak JSON memory " << budget.peak() << " bytes\n";
        if (argc >= 4) {
            Sink clipped; ClippedSink output {clipped, std::stoi(argv[2]), std::stoi(argv[3]), argc == 6 ? std::stoi(argv[4]) : 1};
            Input input {response}; const char *clipError = nullptr;
            assert(road_tile_builder::generate(input, output, scratch, &budget, clipError));
            size_t points = 0;
            for (const auto &line : clipped.lines) {
                points += line.size()/2;
                for (size_t i = 0; i < line.size(); i += 2) {
                    assert(line[i] >= output.lat*45 && line[i] <= (output.lat+output.span)*45);
                    assert(line[i+1] >= output.lon*45 && line[i+1] <= (output.lon+output.span)*45);
                }
            }
            std::cout << "Clipped cell: " << clipped.lines.size() << " roads, " << points << " points, "
                      << 6 + clipped.lines.size()*2 + points*4 << " cached bytes\n";
            if (argc == 6) {
                const auto bytes = binary(clipped);
                std::ofstream target(argv[5], std::ios::binary); target.write(bytes.data(), bytes.size());
                assert(target.good());
            }
        }
    }
    std::cout << "Road tile builder tests passed\n";
}
