#include "icarus/util/binio.h"
#include "icarus/util/json.h"
#include "icarus/util/rng.h"
#include "test_framework.h"

using namespace icarus;

TEST("json: parse and dump roundtrip") {
    std::string src = R"({"a": 1, "b": [true, false, null, "x\ny"], "c": {"d": -2.5, "e": "中文"}, // comment
    "f": 1e3,})";
    Json j = Json::parse(src);
    CHECK(j.is_object());
    CHECK_EQ(j.integer("a"), 1);
    CHECK_EQ(j["b"].size(), (size_t)4);
    CHECK(j["b"][0].as_bool());
    CHECK(j["b"][2].is_null());
    CHECK_EQ(j["b"][3].as_str(), std::string("x\ny"));
    CHECK_EQ(j["c"].num("d"), -2.5);
    CHECK_EQ(j["c"].str("e"), std::string("中文"));
    CHECK_EQ(j.num("f"), 1000.0);
    Json k = Json::parse(j.dump(2));
    CHECK_EQ(k.dump(), j.dump());
    bool threw = false;
    try {
        Json::parse("{\"a\": }");
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST("binio: varints, strings, sections, rle") {
    BinWriter w;
    w.varu(0);
    w.varu(300);
    w.vari(-12345);
    w.str("hello");
    size_t s = w.begin_section("TEST");
    w.u32v(0xDEADBEEF);
    w.end_section(s);
    std::vector<u16> data(1000, 7);
    data[500] = 9;
    auto rle = rle_encode_u16(data.data(), data.size());
    w.bytes(rle);

    BinReader r(w.data());
    CHECK_EQ(r.varu(), (u64)0);
    CHECK_EQ(r.varu(), (u64)300);
    CHECK_EQ(r.vari(), (i64)-12345);
    CHECK_EQ(r.str(), std::string("hello"));
    BinReader sec = r.section("TEST");
    CHECK_EQ(sec.u32v(), 0xDEADBEEFu);
    auto rle2 = r.bytes();
    std::vector<u16> out(1000);
    CHECK(rle_decode_u16(rle2, out.data(), out.size()));
    CHECK(out == data);
    CHECK(rle.size() < 20);
}

TEST("rng: deterministic and serialisable") {
    Rng a(42), b(42);
    for (int i = 0; i < 100; ++i) CHECK_EQ(a.next_u32(), b.next_u32());
    Rng c;
    c.set_raw(a.state(), a.inc());
    for (int i = 0; i < 10; ++i) CHECK_EQ(a.below(1000), c.below(1000));
    int hist[4] = {0};
    for (int i = 0; i < 4000; ++i) hist[a.below(4)]++;
    for (int k = 0; k < 4; ++k) CHECK(hist[k] > 850 && hist[k] < 1150);
}
