// Rule & content registry, loaded from JSON data files (game/data/*.json).
// Everything the kernel needs to know about materials, items, drives, spells,
// technologies and buildings lives here, so rules stay data-driven.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "icarus/util/json.h"
#include "icarus/util/types.h"

namespace icarus {

using MatId = u8;
using ItemId = u16;
constexpr ItemId kNoItem = 0xFFFF;

struct Material {
    MatId id = 0;
    std::string key;
    std::string name;       // display name (zh)
    u32 color = 0xFF00FF;   // 0xRRGGBB
    float color_var = 0.05f;
    bool solid = true;      // blocks movement
    bool opaque = true;     // hides neighbour faces
    bool fluid = false;
    bool granular = false;  // falls when unsupported below
    bool passable = false;  // solid-looking but walkable (doors)
    bool anchor = false;    // levistone: floats and anchors structures
    bool rigid = true;      // participates in structural connectivity
    float flammability = 0; // 0..1 ignition chance factor
    int burn_ticks = 0;     // ticks a burning voxel lasts
    std::string burn_to_key = "air";
    MatId burn_to = 0;
    int hardness = 10;      // base work ticks to dig
    int toughness = 3;      // damage steps before destruction (1..7)
    float density = 1.0f;
    std::string drop_item;  // item produced when dug
    int drop_count = 0;
    ItemId drop_item_id = kNoItem;
    bool fertile = false;   // plants can grow on it
    bool diggable = true;
    bool render_transparent = false;
};

struct ItemDef {
    ItemId id = 0;
    std::string key;
    std::string name;
    float weight = 1.0f;
    float nutrition = 0.0f;   // hunger restored when eaten (0 = inedible)
    float joy = 0.0f;         // satisfaction bonus when consumed
    float spoil_per_day = 0.0f;
    float value = 1.0f;       // abstract exchange value
    std::string place_mat;    // material placed when used for construction
    float power = 0.0f;       // tools: work speed factor; weapons: harm per hit (body fraction)
    float armor = 0.0f;       // armour: share of harm absorbed
    float range = 0.0f;       // weapons: reach in cubes (bows shoot)
    float carry = 0.0f;       // carts: extra carrying capacity (weight units)
    std::vector<std::string> tags;
    bool has_tag(const std::string& t) const {
        for (auto& x : tags) if (x == t) return true;
        return false;
    }
};

// Material ids the kernel refers to directly. Resolved after loading.
struct CoreMats {
    MatId air = 0, levistone = 0, stone = 0, dirt = 0, grass = 0, sand = 0, gravel = 0, clay = 0,
          water = 0, log = 0, leaves = 0, planks = 0, rubble = 0, brick = 0, copper_ore = 0, iron_ore = 0,
          coal = 0, farmland = 0, crop = 0, ash = 0, path = 0, thatch = 0, meteorite = 0, basalt = 0,
          spring = 0, door = 0, berry_bush = 0, stone_brick = 0, glass = 0, magma = 0;
};

class Registry {
public:
    // Load from a map of file-name -> JSON text. Throws std::runtime_error on invalid data.
    void load(const std::map<std::string, std::string>& files);
    void load_from_dir(const std::string& dir);  // convenience for CLI/tests

    const Material& mat(MatId id) const { return mats_[id]; }
    size_t mat_count() const { return mats_.size(); }
    MatId mat_id(const std::string& key) const;  // throws if unknown
    bool has_mat(const std::string& key) const { return mat_index_.count(key) != 0; }

    const ItemDef& item(ItemId id) const { return items_[id]; }
    size_t item_count() const { return items_.size(); }
    ItemId item_id(const std::string& key) const;  // throws if unknown
    ItemId find_item(const std::string& key) const;  // kNoItem if unknown

    const CoreMats& m() const { return core_; }

    // Raw JSON documents for subsystems that parse their own sections.
    const Json& doc(const std::string& name) const;

    u64 content_hash() const { return content_hash_; }

private:
    void parse_materials(const Json& j);
    void parse_items(const Json& j);

    std::vector<Material> mats_;
    std::map<std::string, MatId> mat_index_;
    std::vector<ItemDef> items_;
    std::map<std::string, ItemId> item_index_;
    std::map<std::string, Json> docs_;
    CoreMats core_;
    u64 content_hash_ = 0;
};

u32 parse_color(const std::string& hex);

}  // namespace icarus
