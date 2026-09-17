// polyline.cpp — 折线 JSON（作画）加载、适配与轨迹生成实现
// 由 Robot.cpp（单文件版）拆分而来；实现逐行保真。
#include "polyline.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

#include <cmath>
#include <fstream>
#include <sstream>

bool loadPolylinesFromJson(const std::string& path, std::vector<PolyPath2D>& polys, float& mm_per_unit, float& user_scale) {
    std::ifstream ifs(path);
    if (!ifs) { std::wstringstream ws; ws << L"[错误] 打开线稿JSON失败：" << mb2w(path); wprintln(ws.str()); return false; }
    json j; try { ifs >> j; }
    catch (...) { wprintln(L"[错误] 解析线稿JSON失败。"); return false; }
    std::string units = j.value("units", "mm");
    user_scale = float(j.value("scale", 1.0));
    if (units == "mm") mm_per_unit = 1.0f;
    else if (units == "px") mm_per_unit = float(j.value("mm_per_unit", 0.264583));
    else { wprintln(L"[警告] 未知units，按mm处理。"); mm_per_unit = 1.0f; }

    if (!j.contains("polylines") || !j["polylines"].is_array()) { wprintln(L"[错误] 线稿JSON缺少 polylines。"); return false; }
    for (auto& pl : j["polylines"]) {
        if (!pl.contains("points") || !pl["points"].is_array()) continue;
        PolyPath2D p; p.closed = pl.value("closed", false);
        for (auto& pt : pl["points"]) {
            if (!pt.is_array() || pt.size() < 2) continue;
            float x = float(pt[0].get<double>()) * mm_per_unit * user_scale;
            float y = float(pt[1].get<double>()) * mm_per_unit * user_scale;
            p.pts.emplace_back(x, y);
        }
        if (p.pts.size() >= 2) polys.push_back(std::move(p));
    }
    return !polys.empty();
}

void bboxOfPolys(const std::vector<PolyPath2D>& polys, float& minx, float& maxx, float& miny, float& maxy) {
    minx = miny = std::numeric_limits<float>::infinity();
    maxx = maxy = -std::numeric_limits<float>::infinity();
    for (const auto& p : polys) for (const auto& q : p.pts) {
        minx = std::min(minx, q.first); maxx = std::max(maxx, q.first);
        miny = std::min(miny, q.second); maxy = std::max(maxy, q.second);
    }
    if (!std::isfinite(minx)) { minx = maxx = miny = maxy = 0.f; }
}

void fitPolylinesToArea(std::vector<PolyPath2D>& polys, const WorkArea& area) {
    if (polys.empty()) return;
    float minx, maxx, miny, maxy; bboxOfPolys(polys, minx, maxx, miny, maxy);
    float w = std::max(1.0f, maxx - minx), h = std::max(1.0f, maxy - miny);
    float targetW = (area.xmax - area.xmin) - SAFE_MARGIN * 2;
    float targetH = (area.ymax - area.ymin) - SAFE_MARGIN * 2;
    float s = std::min(targetW / w, targetH / h);
    float ox = area.xmin + SAFE_MARGIN - minx * s + (targetW - w * s) * 0.5f;
    float oy = area.ymin + SAFE_MARGIN - miny * s + (targetH - h * s) * 0.5f;
    for (auto& p : polys) for (auto& q : p.pts) { q.first = q.first * s + ox; q.second = q.second * s + oy; }
}

void generatePolylineTrajectory(const std::vector<PolyPath2D>& polys, std::vector<Point>& traj, int speed, float drawZ) {
    for (const auto& p : polys) {
        if (p.pts.size() < 2) continue;
        auto s = p.pts.front();
        traj.push_back(Point{ s.first, s.second, Z_UP,       false,(uint8_t)speed,"UP" });
        traj.push_back(Point{ s.first, s.second, Z_MID,      false,(uint8_t)speed,"MID" });
        traj.push_back(Point{ s.first, s.second, Z_PRE_DOWN, false,(uint8_t)speed,"PRE" });
        traj.push_back(Point{ s.first, s.second, drawZ,      true, (uint8_t)speed,"DRAW" });
        for (size_t i = 1; i < p.pts.size(); ++i) {
            traj.push_back(Point{ p.pts[i].first, p.pts[i].second, drawZ, true,(uint8_t)speed,"DRAW" });
        }
        if (p.closed) traj.push_back(Point{ s.first, s.second, drawZ, true,(uint8_t)speed,"DRAW" });
        auto e = p.pts.back();
        traj.push_back(Point{ e.first, e.second, Z_PRE_DOWN, false,(uint8_t)speed,"PRE" });
        traj.push_back(Point{ e.first, e.second, Z_MID,      false,(uint8_t)speed,"MID" });
        traj.push_back(Point{ e.first, e.second, Z_UP,       false,(uint8_t)speed,"UP" });
    }
}

bool drawPolylinesFromFile(const std::string& path, std::vector<Point>& traj) {
    std::vector<PolyPath2D> polys; float mm_per_unit = 1.0f, user_scale = 1.0f;
    if (!loadPolylinesFromJson(path, polys, mm_per_unit, user_scale)) return false;

    // 放到底部区域
    WorkArea area = g_safeArea;
    float totalH = area.ymax - area.ymin;
    float bottom_ymin = area.ymin + SAFE_MARGIN;
    float bottom_ymax = area.ymin + totalH * (1.0f - TEXT_TOP_RATIO) - V_GAP_BETWEEN * 0.5f - SAFE_MARGIN;
    if (bottom_ymax < bottom_ymin + 10.0f) bottom_ymax = bottom_ymin + std::max(10.0f, totalH * 0.2f);
    WorkArea bottom{ area.xmin + SAFE_MARGIN, area.xmax - SAFE_MARGIN, bottom_ymin, bottom_ymax };

    fitPolylinesToArea(polys, bottom);
    generatePolylineTrajectory(polys, traj, /*speed*/1, /*drawZ*/Z_DOWN_LIGHT);
    return true;
}
