// hanzi.cpp — HanziWriter 汉字数据解析、单字轨迹生成与多字排版实现
// 由 Robot.cpp（单文件版）拆分而来；实现逐行保真。
// 注意：prepare_layout_only 会改写全局布局参数（ACTIVE_CHAR_SIZE / TEXT_TOP_RATIO / CHAR_SPACING），
//       该行为是“后绘对齐”的依赖，不可改为传值。
#include "hanzi.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

void bboxOfStrokes(const std::vector<MMAHStroke>& ss, float& minx, float& maxx, float& miny, float& maxy) {
    minx = miny = std::numeric_limits<float>::infinity();
    maxx = maxy = -std::numeric_limits<float>::infinity();
    for (const auto& s : ss) for (const auto& p : s.centerLine) {
        minx = std::min(minx, p.first); maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second); maxy = std::max(maxy, p.second);
    }
    if (!std::isfinite(minx)) { minx = maxx = miny = maxy = 0.f; }
}

MMAHCharData parseHanziWriterJson(const std::string& path, wchar_t wch) {
    MMAHCharData ret; ret.charStr = std::wstring(1, wch);
    std::ifstream ifs(path);
    if (!ifs) { std::wstringstream ws; ws << L"[错误] 打开汉字JSON失败：" << mb2w(path); wprintln(ws.str()); return ret; }
    json j; try { ifs >> j; }
    catch (...) { wprintln(L"[错误] 解析汉字JSON失败。"); return ret; }
    if (!j.contains("medians") || !j["medians"].is_array()) { wprintln(L"[错误] 汉字JSON缺少 medians。"); return ret; }

    int order = 0;
    for (auto& st : j["medians"]) {
        if (!st.is_array() || st.empty()) continue;
        MMAHStroke s; s.order = ++order;
        for (auto& seg : st) {
            if (!seg.is_array() || seg.size() < 2) continue;
            float x = float(seg[0].get<double>());
            float y = float(seg[1].get<double>());
            s.centerLine.emplace_back(x, y);
        }
        if (s.centerLine.size() >= 2) {
            auto a = s.centerLine.front(), b = s.centerLine.back();
            float dx = b.first - a.first, dy = b.second - a.second;
            if (std::fabs(dx) > std::fabs(dy) * 1.5f) s.type = "heng";
            else if (std::fabs(dy) > std::fabs(dx) * 1.5f) s.type = "shu";
            else if (s.centerLine.size() <= 3) s.type = "dot";
            else s.type = "other";
        }
        else s.type = "dot";
        if (!s.centerLine.empty()) ret.strokes.push_back(std::move(s));
    }
    ret.isValid = !ret.strokes.empty();
    return ret;
}

bool loadCharData(wchar_t wc, MMAHCharData& out) {
    std::string gbk1;
    {
        std::wstring one(1, wc);
        gbk1 = w2gbk(one);
        if (gbk1.empty()) {
            std::ostringstream oss; oss << std::hex << std::uppercase << int((unsigned)wc);
            gbk1 = "U" + oss.str();
        }
    }
    std::string path = HANZI_BASE_DIR + "/data/" + gbk1 + ".json";
    out = parseHanziWriterJson(path, wc);
    return out.isValid;
}

bool generateSingleCharTrajectory(const MMAHCharData& ch, int baseSpeed, std::vector<Point>& traj, float size_mm) {
    if (!ch.isValid) return false;
    float minx, maxx, miny, maxy; bboxOfStrokes(ch.strokes, minx, maxx, miny, maxy);
    float w = std::max(1.0f, maxx - minx);
    float h = std::max(1.0f, maxy - miny);
    float S = size_mm / std::max(w, h);
    auto toLocal = [&](std::pair<float, float> p) {
        return std::pair<float, float>{ (p.first - minx)* S, (p.second - miny)* S };
        };
    auto zFor = [&](const std::string& t)->float {
        if (t == "dot")  return Z_DOWN_HEAVY;
        if (t == "heng") return Z_DOWN_NORMAL;
        if (t == "shu")  return Z_DOWN_NORMAL;
        return Z_DOWN_LIGHT;
        };

    for (const auto& s : ch.strokes) {
        if (s.centerLine.empty()) continue;
        auto p0 = toLocal(s.centerLine.front());
        float zdown = zFor(s.type);
        traj.push_back(Point{ p0.first, p0.second, Z_UP,       false,(uint8_t)baseSpeed,"UP" });
        traj.push_back(Point{ p0.first, p0.second, Z_MID,      false,(uint8_t)baseSpeed,"MID" });
        traj.push_back(Point{ p0.first, p0.second, Z_PRE_DOWN, false,(uint8_t)baseSpeed,"PRE" });
        traj.push_back(Point{ p0.first, p0.second, zdown,      true, (uint8_t)baseSpeed,s.type });
        for (size_t k = 1; k < s.centerLine.size(); ++k) {
            auto pk = toLocal(s.centerLine[k]);
            traj.push_back(Point{ pk.first, pk.second, zdown, true,(uint8_t)baseSpeed,s.type });
        }
        auto pe = toLocal(s.centerLine.back());
        traj.push_back(Point{ pe.first, pe.second, Z_PRE_DOWN, false,(uint8_t)baseSpeed,"PRE" });
        traj.push_back(Point{ pe.first, pe.second, Z_MID,      false,(uint8_t)baseSpeed,"MID" });
    }
    // 末尾抬笔，方便分字
    if (!traj.empty()) {
        Point last = traj.back(); last.z = Z_UP; last.isPenDown = false; last.zType = "UP-ENDCHAR";
        traj.push_back(last);
    }
    return true;
}

// --------------------------- 安全区/布局 ---------------------------
bool layout_fit(int n, float W, float H, float S, float sp, int cols, int& out_rows) {
    if (cols <= 0) return false;
    out_rows = (n + cols - 1) / cols;
    float full_w = cols * S + (cols - 1) * sp;
    float full_h = out_rows * S + (out_rows - 1) * sp;
    return (full_w <= W + 1e-3f) && (full_h <= H + 1e-3f);
}

TextPlan plan_text_area_and_layout(
    int n, const WorkArea& safe, float init_top_ratio,
    float vgap_mm, float sp_init)
{
    TextPlan tp;
    if (n <= 0) {
        tp.ok = true; tp.cols = 5; tp.rows = 0; tp.used_S = SINGLE_CHAR_MAX; tp.used_sp = sp_init; tp.top_ratio = init_top_ratio; tp.text_area = safe;
        return tp;
    }

    // 在上区划出文本区
    for (float ratio = init_top_ratio; ratio <= 0.90f + 1e-6f; ratio += 0.02f) {
        float totalH = safe.ymax - safe.ymin;
        float totalW = safe.xmax - safe.xmin;
        float textH = totalH * ratio - vgap_mm * 0.5f;
        float textW = totalW - SAFE_MARGIN * 2.0f;
        if (textH <= 0 || textW <= 0) continue;

        for (int colsTry : {5, 4}) {
            for (float S = SINGLE_CHAR_MAX; S >= SINGLE_CHAR_MIN - 1e-3f; S -= 1.0f) {
                for (float sp = sp_init; sp >= CHAR_SPACING_MIN - 1e-3f; sp -= 1.0f) {
                    int rows = 0;
                    if (!layout_fit(n, textW, textH, S, sp, colsTry, rows)) continue;
                    tp.ok = true; tp.cols = colsTry; tp.rows = rows; tp.used_S = S; tp.used_sp = sp; tp.top_ratio = ratio;
                    float text_ymax = safe.ymax - vgap_mm * 0.5f;
                    float text_ymin = text_ymax - textH;
                    tp.text_area = WorkArea{ safe.xmin + SAFE_MARGIN, safe.xmin + SAFE_MARGIN + textW, text_ymin, text_ymax };

                    tp.offsets.clear(); tp.offsets.reserve(n);
                    for (int i = 0; i < n; ++i) {
                        int r = i / tp.cols, c = i % tp.cols;
                        float ox = c * (S + sp);
                        float oy = textH - S - r * (S + sp);
                        tp.offsets.push_back(Offset{ ox,oy });
                    }
                    return tp;
                }
            }
        }
    }
    tp.ok = false; return tp;
}

bool prepare_layout_only(const std::wstring& wtext_in, TextPlan& plan_out, std::wstring& chars_out) {
    chars_out.clear();
    for (wchar_t c : wtext_in) if (isCJKOrPunct(c)) chars_out.push_back(c);
    if (chars_out.empty()) { wprintln(L"[错误] 无有效汉字。"); return false; }
    plan_out = plan_text_area_and_layout((int)chars_out.size(), g_safeArea, TEXT_TOP_RATIO, V_GAP_BETWEEN, CHAR_SPACING);
    if (!plan_out.ok) {
        wprintln(L"[错误] 排版失败。尝试：减少字数/提高上区比例/减小字号/减小字距。");
        return false;
    }
    ACTIVE_CHAR_SIZE = plan_out.used_S;
    TEXT_TOP_RATIO = plan_out.top_ratio;
    CHAR_SPACING = plan_out.used_sp;
    return true;
}
