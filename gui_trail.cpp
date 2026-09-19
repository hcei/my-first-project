// gui_trail.cpp — 实时轨迹与纸张边界的服务实现（独立于 gs::g_mu 的自带锁）
#include "gui_trail.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace gs { namespace trail {

namespace {

std::mutex g_tmux;                 // 独立锁：锁序 g_mu → g_tmux，本模块绝不反向获取 g_mu
constexpr size_t MAX_CMD = 80000;  // 下发轨迹点上限（约 1MB，超限置 truncated）
constexpr size_t MAX_ACT = 20000;  // 实测采样点上限

std::vector<Cpt> g_cmd;
std::vector<Apt> g_act;
uint64_t   g_epoch = 0;
bool       g_trunc = false;
Corner     g_corners[kCorners];

inline bool finite2(float x, float y) { return std::isfinite(x) && std::isfinite(y); }

} // anonymous namespace

void reset() {
    std::lock_guard<std::mutex> lk(g_tmux);
    g_cmd.clear(); g_act.clear(); g_trunc = false; ++g_epoch;
}
uint64_t epoch() { std::lock_guard<std::mutex> lk(g_tmux); return g_epoch; }

void addCommanded(const Point& p) {
    if (!finite2(p.x, p.y)) return;
    std::lock_guard<std::mutex> lk(g_tmux);
    if (g_cmd.size() >= MAX_CMD) { g_trunc = true; return; }
    g_cmd.push_back(Cpt{ p.x, p.y, (uint8_t)(p.isPenDown ? 1 : 0) });
}

void addCommandedBatch(const std::vector<Point>& pts, size_t n) {
    std::lock_guard<std::mutex> lk(g_tmux);
    size_t k = std::min(n, pts.size());
    for (size_t i = 0; i < k; ++i) {
        if (!finite2(pts[i].x, pts[i].y)) continue;
        if (g_cmd.size() >= MAX_CMD) { g_trunc = true; return; }
        g_cmd.push_back(Cpt{ pts[i].x, pts[i].y, (uint8_t)(pts[i].isPenDown ? 1 : 0) });
    }
}

void addActual(float x, float y) {
    if (!finite2(x, y)) return;
    std::lock_guard<std::mutex> lk(g_tmux);
    if (g_act.size() >= MAX_ACT) return;
    g_act.push_back(Apt{ x, y });
}

uint64_t fetchCommanded(uint64_t since, std::vector<Cpt>& out) {
    std::lock_guard<std::mutex> lk(g_tmux);
    if (since > g_cmd.size()) since = 0;   // 游标越界（多半是 reset 后），退回全量
    for (size_t i = (size_t)since; i < g_cmd.size(); ++i) out.push_back(g_cmd[i]);
    return (uint64_t)g_cmd.size();
}
uint64_t fetchActual(uint64_t since, std::vector<Apt>& out) {
    std::lock_guard<std::mutex> lk(g_tmux);
    if (since > g_act.size()) since = 0;
    for (size_t i = (size_t)since; i < g_act.size(); ++i) out.push_back(g_act[i]);
    return (uint64_t)g_act.size();
}

bool   truncated()      { std::lock_guard<std::mutex> lk(g_tmux); return g_trunc; }
size_t commandedSize()  { std::lock_guard<std::mutex> lk(g_tmux); return g_cmd.size(); }
size_t actualSize()     { std::lock_guard<std::mutex> lk(g_tmux); return g_act.size(); }

void bounds(float& minx, float& miny, float& maxx, float& maxy) {
    std::lock_guard<std::mutex> lk(g_tmux);
    if (g_cmd.empty()) {
        minx = g_devLimit.xmin; maxx = g_devLimit.xmax;
        miny = g_devLimit.ymin; maxy = g_devLimit.ymax;
        return;
    }
    minx = maxx = g_cmd.front().x;
    miny = maxy = g_cmd.front().y;
    for (const auto& c : g_cmd) {
        minx = std::min(minx, c.x); maxx = std::max(maxx, c.x);
        miny = std::min(miny, c.y); maxy = std::max(maxy, c.y);
    }
}

void setCorner(int i, float x, float y) {
    if (i < 0 || i >= kCorners) return;
    if (!finite2(x, y)) return;
    std::lock_guard<std::mutex> lk(g_tmux);
    g_corners[i].x = x; g_corners[i].y = y; g_corners[i].valid = true;
}
void clearCorner(int i) {
    if (i < 0 || i >= kCorners) return;
    std::lock_guard<std::mutex> lk(g_tmux);
    g_corners[i] = Corner{};
}
void clearCorners() {
    std::lock_guard<std::mutex> lk(g_tmux);
    for (int i = 0; i < kCorners; ++i) g_corners[i] = Corner{};
}
Corner corner(int i) {
    if (i < 0 || i >= kCorners) return Corner{};
    std::lock_guard<std::mutex> lk(g_tmux);
    return g_corners[i];
}
int cornerCount() {
    std::lock_guard<std::mutex> lk(g_tmux);
    int c = 0;
    for (int i = 0; i < kCorners; ++i) if (g_corners[i].valid) ++c;
    return c;
}
bool cornersBounds(float& minx, float& miny, float& maxx, float& maxy) {
    std::lock_guard<std::mutex> lk(g_tmux);
    bool any = false;
    for (int i = 0; i < kCorners; ++i) {
        if (!g_corners[i].valid) continue;
        float x = g_corners[i].x, y = g_corners[i].y;
        if (!any) { minx = maxx = x; miny = maxy = y; any = true; }
        else {
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);
        }
    }
    return any;
}
void getCorners(Corner out[kCorners]) {
    std::lock_guard<std::mutex> lk(g_tmux);
    for (int i = 0; i < kCorners; ++i) out[i] = g_corners[i];
}

nlohmann::json calibToJson() {
    std::lock_guard<std::mutex> lk(g_tmux);
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < kCorners; ++i)
        arr.push_back(nlohmann::json{ { "x", g_corners[i].x }, { "y", g_corners[i].y },
                                      { "valid", g_corners[i].valid } });
    return arr;
}

void calibFromJson(const nlohmann::json& j) {
    if (!j.is_array()) return;
    Corner tmp[kCorners];
    for (size_t i = 0; i < j.size() && i < (size_t)kCorners; ++i) {
        const nlohmann::json& e = j[i];
        if (!e.is_object()) continue;
        Corner c;
        try {
            if (e.contains("x")) c.x = e["x"].get<float>();
            if (e.contains("y")) c.y = e["y"].get<float>();
            if (e.contains("valid")) c.valid = e["valid"].get<bool>();
        } catch (...) { c = Corner{}; }
        if (c.valid && (!std::isfinite(c.x) || !std::isfinite(c.y))) c = Corner{};
        tmp[i] = c;
    }
    std::lock_guard<std::mutex> lk(g_tmux);
    for (int i = 0; i < kCorners; ++i) g_corners[i] = tmp[i];
}

}} // namespace gs::trail
