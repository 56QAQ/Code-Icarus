#include "icarus/data/registry.h"

#include <filesystem>
#include <stdexcept>

#include "icarus/util/binio.h"
#include "icarus/util/rng.h"

namespace icarus {

u32 parse_color(const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) throw std::runtime_error("bad color '" + hex + "'");
    return (u32)std::stoul(h, nullptr, 16);
}

void Registry::load(const std::map<std::string, std::string>& files) {
    mats_.clear();
    mat_index_.clear();
    items_.clear();
    item_index_.clear();
    docs_.clear();
    content_hash_ = 1469598103934665603ull;
    for (const auto& [name, text] : files) {
        try {
            docs_[name] = Json::parse(text);
        } catch (const JsonError& e) {
            throw std::runtime_error("data file " + name + ": " + e.what());
        }
        content_hash_ = hash_combine(content_hash_, hash_string(name));
        content_hash_ = hash_combine(content_hash_, hash_string(text));
    }
    if (!docs_.count("materials")) throw std::runtime_error("missing data file: materials");
    if (!docs_.count("items")) throw std::runtime_error("missing data file: items");
    parse_items(docs_["items"]);
    parse_materials(docs_["materials"]);

    auto need = [&](const char* key) -> MatId { return mat_id(key); };
    core_.air = need("air");
    if (core_.air != 0) throw std::runtime_error("material 'air' must be the first entry (id 0)");
    core_.levistone = need("levistone");
    core_.stone = need("stone");
    core_.dirt = need("dirt");
    core_.grass = need("grass");
    core_.sand = need("sand");
    core_.gravel = need("gravel");
    core_.clay = need("clay");
    core_.water = need("water");
    core_.log = need("log");
    core_.leaves = need("leaves");
    core_.planks = need("planks");
    core_.rubble = need("rubble");
    core_.brick = need("brick");
    core_.copper_ore = need("copper_ore");
    core_.iron_ore = need("iron_ore");
    core_.coal = need("coal");
    core_.farmland = need("farmland");
    core_.crop = need("crop");
    core_.ash = need("ash");
    core_.path = need("path");
    core_.thatch = need("thatch");
    core_.meteorite = need("meteorite");
    core_.basalt = need("basalt");
    core_.spring = need("spring");
    core_.door = need("door");
    core_.berry_bush = need("berry_bush");
    core_.stone_brick = need("stone_brick");
    core_.glass = need("glass");
    core_.magma = need("magma");
    auto opt = [&](const char* key) -> MatId { return has_mat(key) ? mat_id(key) : core_.air; };
    core_.snow = opt("snow");
    core_.ice = opt("ice");
    core_.mud = opt("mud");
    core_.red_sand = opt("red_sand");
    core_.sandstone = opt("sandstone");
    core_.granite = opt("granite");
    core_.limestone = opt("limestone");
    core_.flint = opt("flint");
    core_.dry_grass = opt("dry_grass");
    core_.pine_log = opt("pine_log");
    core_.pine_leaves = opt("pine_leaves");
    core_.birch_log = opt("birch_log");
    core_.birch_leaves = opt("birch_leaves");
    core_.fruit_leaves = opt("fruit_leaves");
    core_.cactus = opt("cactus");
    core_.reeds = opt("reeds");
    core_.wild_grain = opt("wild_grain");
    core_.mushroom = opt("mushroom");
    core_.herb_plant = opt("herb_plant");
    core_.sapling = opt("sapling");
    core_.campfire = opt("campfire");
}

void Registry::load_from_dir(const std::string& dir) {
    std::map<std::string, std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() != ".json") continue;
        std::string text;
        if (!read_file(p.string(), text)) throw std::runtime_error("cannot read " + p.string());
        files[p.stem().string()] = text;
    }
    load(files);
}

MatId Registry::mat_id(const std::string& key) const {
    auto it = mat_index_.find(key);
    if (it == mat_index_.end()) throw std::runtime_error("unknown material '" + key + "'");
    return it->second;
}

ItemId Registry::item_id(const std::string& key) const {
    auto it = item_index_.find(key);
    if (it == item_index_.end()) throw std::runtime_error("unknown item '" + key + "'");
    return it->second;
}

ItemId Registry::find_item(const std::string& key) const {
    auto it = item_index_.find(key);
    return it == item_index_.end() ? kNoItem : it->second;
}

const Json& Registry::doc(const std::string& name) const {
    static const Json empty;
    auto it = docs_.find(name);
    return it == docs_.end() ? empty : it->second;
}

void Registry::parse_items(const Json& j) {
    const Json& list = j["items"];
    if (!list.is_array()) throw std::runtime_error("items.json: 'items' must be an array");
    for (const Json& e : list.items()) {
        ItemDef d;
        d.id = (ItemId)items_.size();
        d.key = e.str("key");
        if (d.key.empty()) throw std::runtime_error("items.json: item without key");
        if (item_index_.count(d.key)) throw std::runtime_error("items.json: duplicate key " + d.key);
        d.name = e.str("name", d.key);
        d.weight = e.flt("weight", 1.0f);
        d.nutrition = e.flt("nutrition", 0.0f);
        d.joy = e.flt("joy", 0.0f);
        d.spoil_per_day = e.flt("spoil_per_day", 0.0f);
        d.value = e.flt("value", 1.0f);
        d.place_mat = e.str("place_mat");
        d.power = e.flt("power", 0.0f);
        d.armor = e.flt("armor", 0.0f);
        d.carry = e.flt("carry", 0.0f);
        d.range = e.flt("range", 0.0f);
        d.tool_kind = e.str("tool");
        d.durability = e.integer("durability", d.tool_kind.empty() ? 0 : 100);
        d.clothes_kind = e.str("clothes");
        d.warmth = e.flt("warmth", 0.0f);
        for (const Json& t : e["tags"].items()) d.tags.push_back(t.as_str());
        item_index_[d.key] = d.id;
        items_.push_back(d);
    }
}

void Registry::parse_materials(const Json& j) {
    const Json& list = j["materials"];
    if (!list.is_array()) throw std::runtime_error("materials.json: 'materials' must be an array");
    if (list.size() > 255) throw std::runtime_error("materials.json: too many materials (max 255)");
    for (const Json& e : list.items()) {
        Material m;
        m.id = (MatId)mats_.size();
        m.key = e.str("key");
        if (m.key.empty()) throw std::runtime_error("materials.json: material without key");
        if (mat_index_.count(m.key)) throw std::runtime_error("materials.json: duplicate key " + m.key);
        m.name = e.str("name", m.key);
        m.color = parse_color(e.str("color", "#ff00ff"));
        m.color_var = e.flt("color_var", 0.05f);
        m.solid = e.boolean("solid", true);
        m.opaque = e.boolean("opaque", m.solid);
        m.fluid = e.boolean("fluid", false);
        m.granular = e.boolean("granular", false);
        m.passable = e.boolean("passable", false);
        m.anchor = e.boolean("anchor", false);
        m.rigid = e.boolean("rigid", m.solid && !m.granular && !m.fluid);
        m.flammability = e.flt("flammability", 0.0f);
        m.burn_ticks = e.integer("burn_ticks", 0);
        m.burn_to_key = e.str("burn_to", "air");
        m.hardness = e.integer("hardness", 10);
        m.toughness = clampv(e.integer("toughness", 3), 1, 7);
        m.density = e.flt("density", 1.0f);
        m.drop_item = e.str("drop");
        m.drop_count = e.integer("drop_count", m.drop_item.empty() ? 0 : 1);
        m.fertile = e.boolean("fertile", false);
        m.diggable = e.boolean("diggable", true);
        m.render_transparent = e.boolean("transparent", !m.opaque);
        m.trunk = e.boolean("trunk", false);
        m.foliage = e.boolean("foliage", false);
        m.forage_key = e.str("forage");
        m.forage_count = e.integer("forage_count", m.forage_key.empty() ? 0 : 1);
        m.forage_to_key = e.str("forage_to", "air");
        m.sprite = e.str("sprite");
        m.furniture = e.str("furniture");
        m.made_of = e.str("made_of");
        mat_index_[m.key] = m.id;
        mats_.push_back(m);
    }
    int sprites = 0;
    for (auto& m : mats_) {
        m.burn_to = mat_id(m.burn_to_key);
        if (!m.drop_item.empty()) m.drop_item_id = item_id(m.drop_item);
        if (!m.forage_key.empty()) m.forage_item = item_id(m.forage_key);
        if (!m.made_of.empty()) m.made_of_item = item_id(m.made_of);
        m.forage_to = mat_id(m.forage_to_key);
        if (!m.sprite.empty()) m.sprite_layer = kSpriteBase + sprites++;
        m.holds_loose = !m.solid && !m.fluid && (!m.forage_key.empty() || !m.sprite.empty() || m.key == "crop" ||
                                                  m.key == "cactus");
    }
}

}  // namespace icarus
