#include "icarus/agents/nav.h"

#include "icarus/util/rng.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace icarus {

namespace {
constexpr size_t kTableSize = 1u << 18;
constexpr int kDir8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

inline float octile(const Vec3i& a, const Vec3i& b) {
    float dx = (float)std::abs(a.x - b.x), dz = (float)std::abs(a.z - b.z);
    float dy = (float)std::abs(a.y - b.y);
    return std::max(dx, dz) + 0.41421f * std::min(dx, dz) + dy * 0.5f;
}
}  // namespace

Nav::Nav(World& w) : w_(w), table_(kTableSize) {}

bool Nav::passable(const Vec3i& p) {
    if (!w_.in_bounds(p)) return p.y >= w_.size_y();  // open sky above the world is free
    const Material& m = w_.reg().mat(w_.mat(p));
    return !m.solid || m.passable;
}

bool Nav::standable(const Vec3i& p) {
    if (p.y < 1 || !w_.in_bounds(p)) return false;
    Vec3i below{p.x, p.y - 1, p.z};
    const Material& b = w_.reg().mat(w_.mat(below));
    if (!b.solid || b.passable) return false;
    return passable(p) && passable({p.x, p.y + 1, p.z}) && passable({p.x, p.y + 2, p.z});
}

float Nav::step_cost(const Vec3i& p) {
    const CoreMats& M = w_.reg().m();
    MatId feet = w_.mat(p);
    MatId ground = w_.mat({p.x, p.y - 1, p.z});
    float c = 1.0f;
    if (ground == M.path || ground == M.planks || ground == M.stone_brick) c = 0.72f;
    if (feet == M.water) c = w_.mat({p.x, p.y + 1, p.z}) == M.water ? 6.0f : 2.5f;
    if (feet == M.crop) c += 0.4f;
    if (vburning(w_.get(p)) || vburning(w_.get({p.x, p.y - 1, p.z}))) c += 20.0f;
    return c;
}

bool Nav::find_standable_near(const Vec3i& p, Vec3i& out, int radius) {
    for (int r = 0; r <= radius; ++r) {
        for (int dy = 0; dy <= 3; ++dy) {
            for (int s : {1, -1}) {
                int yy = p.y + dy * s;
                for (int dz = -r; dz <= r; ++dz)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                        Vec3i q{p.x + dx, yy, p.z + dz};
                        if (standable(q)) {
                            out = q;
                            return true;
                        }
                    }
                if (dy == 0) break;
            }
        }
    }
    return false;
}

void Nav::on_changes(const std::vector<VoxelChange>& changes) {
    const Registry& reg = w_.reg();
    for (const VoxelChange& c : changes) {
        MatId a = vmat(c.before), b = vmat(c.after);
        if (a == b) continue;
        const Material& ma = reg.mat(a);
        const Material& mb = reg.mat(b);
        if (ma.solid != mb.solid || ma.passable != mb.passable) {
            if (c.cause) {
                major_dirty = true;
                return;
            }
            minor_dirty = true;
        }
    }
}

int Nav::neighbors(const Vec3i& p, Vec3i* out, float* cost) {
    int n = 0;
    for (int d = 0; d < 8; ++d) {
        int dx = kDir8[d][0], dz = kDir8[d][1];
        bool diag = dx != 0 && dz != 0;
        // Try: same level, step up 1-2, drops down to 3.
        Vec3i cand;
        bool have = false;
        for (int dy : {0, 1, -1, 2, -2, -3}) {
            Vec3i q{p.x + dx, p.y + dy, p.z + dz};
            if (dy >= 1 && !passable({p.x, p.y + 3, p.z})) continue;  // head room for the hop
            if (dy == 2 && (diag || !passable({p.x, p.y + 4, p.z}))) continue;  // climbing needs more room
            if (dy < 0) {
                // Need a clear column at the target xz from our level down.
                bool clear = true;
                for (int k = 0; k > dy && clear; --k)
                    clear = passable({q.x, p.y + k, q.z}) && passable({q.x, p.y + k + 1, q.z}) && passable({q.x, p.y + k + 2, q.z});
                if (!clear) continue;
            }
            if (standable(q)) {
                cand = q;
                have = true;
                break;
            }
            if (dy == 0 && !passable(q)) continue;
        }
        if (!have) continue;
        if (diag) {
            // No corner cutting.
            int top = std::max(p.y, cand.y);
            if (!passable({p.x + dx, top, p.z}) || !passable({p.x, top, p.z + dz}) || !passable({p.x + dx, top + 1, p.z}) ||
                !passable({p.x, top + 1, p.z + dz}))
                continue;
        }
        float c = step_cost(cand) * (diag ? 1.41421f : 1.0f);
        if (cand.y > p.y) c += cand.y - p.y >= 2 ? 2.0f : 0.6f;
        if (cand.y < p.y) c += 0.2f * (float)(p.y - cand.y);
        out[n] = cand;
        cost[n] = c;
        ++n;
    }
    return n;
}

int Nav::flood(const Vec3i& seed, int radius, int max_nodes, std::unordered_map<Vec3i, u16, Vec3iHash>& label, u16 id) {
    if (!standable(seed) || label.count(seed)) return 0;
    std::vector<Vec3i> queue;
    queue.push_back(seed);
    label[seed] = id;
    const i64 r2 = (i64)radius * radius;
    size_t head = 0;
    Vec3i nb[8];
    float nc[8];
    while (head < queue.size() && (int)queue.size() < max_nodes) {
        Vec3i p = queue[head++];
        int n = neighbors(p, nb, nc);
        for (int i = 0; i < n; ++i) {
            const Vec3i& q = nb[i];
            i64 dx = q.x - seed.x, dz = q.z - seed.z;
            if (dx * dx + dz * dz > r2) continue;
            auto [it, fresh] = label.emplace(q, id);
            if (fresh) queue.push_back(q);
        }
    }
    return (int)queue.size();
}

Nav::Node* Nav::slot(u64 key, bool create) {
    size_t h = (size_t)(splitmix64(key) & (kTableSize - 1));
    for (size_t i = 0; i < 64; ++i) {
        Node& n = table_[(h + i) & (kTableSize - 1)];
        if (n.gen != gen_) {
            if (!create) return nullptr;
            n.gen = gen_;
            n.key = key;
            n.g = 1e30f;
            n.parent = 0;
            n.closed = false;
            return &n;
        }
        if (n.key == key) return &n;
    }
    return nullptr;  // table congested; treat as unreachable
}

bool Nav::find_path(const Vec3i& start, const Vec3i& goal, bool adjacent_ok, Path& out, int max_expansions) {
    out.clear();
    stats.searches++;
    ++gen_;
    if (gen_ == 0) {
        for (auto& n : table_) n.gen = 0;
        gen_ = 1;
    }
    auto is_goal = [&](const Vec3i& p) {
        if (p == goal) return true;
        if (!adjacent_ok) return false;
        return std::abs(p.x - goal.x) <= 1 && std::abs(p.z - goal.z) <= 1 && p.y - goal.y <= 2 && goal.y - p.y <= 3;
    };
    struct QE {
        float f;
        float g;
        u64 key;
        bool operator<(const QE& o) const {
            if (f != o.f) return f > o.f;
            if (g != o.g) return g < o.g;
            return key > o.key;
        }
    };
    std::priority_queue<QE> open;
    u64 sk = pack(start);
    Node* sn = slot(sk, true);
    if (!sn) return false;
    sn->g = 0;
    open.push({octile(start, goal), 0, sk});
    int expansions = 0;
    u64 found = 0;
    bool ok = false;
    while (!open.empty()) {
        QE cur = open.top();
        open.pop();
        Node* cn = slot(cur.key, false);
        if (!cn || cn->closed || cur.g > cn->g + 1e-4f) continue;
        cn->closed = true;
        Vec3i p = unpack(cur.key);
        if (is_goal(p)) {
            found = cur.key;
            ok = true;
            break;
        }
        if (++expansions > max_expansions) break;
        Vec3i nb[8];
        float nc[8];
        int nn_count = neighbors(p, nb, nc);
        for (int i = 0; i < nn_count; ++i) {
            const Vec3i& cand = nb[i];
            float cost = nc[i];
            float ng = cur.g + cost;
            u64 ck = pack(cand);
            Node* nn = slot(ck, true);
            if (!nn || nn->closed || ng >= nn->g) continue;
            nn->g = ng;
            nn->parent = cur.key;
            open.push({ng + octile(cand, goal) * 0.9f, ng, ck});
        }
    }
    stats.expansions += (u64)expansions;
    if (!ok) {
        stats.failures++;
        return false;
    }
    std::vector<Vec3i> rev;
    u64 k = found;
    while (k != sk) {
        rev.push_back(unpack(k));
        Node* n = slot(k, false);
        if (!n) break;
        k = n->parent;
        if (rev.size() > 100000) break;
    }
    out.nodes.assign(rev.rbegin(), rev.rend());
    out.next = 0;
    return true;
}

}  // namespace icarus
