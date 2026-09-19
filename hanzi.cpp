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
        if (t == "dot")  return g_enableDunbi ? Z_DOWN_HEAVY : Z_DOWN_NORMAL;  // 关闭顿笔：点画与普通笔画等深，避免小字墨点过大
        if (t == "heng") return Z_DOWN_NORMAL;
        if (t == "shu")  return Z_DOWN_NORMAL;
        return Z_DOWN_LIGHT;
        };

    for (const auto& s : ch.strokes) {
        if (s.centerLine.empty()) continue;
        auto p0 = toLocal(s.centerLine.front());
        float zdown = g_writing_plane_valid ? g_writing_plane_z : zFor(s.type);
        if (g_enableDunbi) {
            // 现状：分层下刀 UP→MID→PRE→DOWN（真机上起笔逐层"往下戳"）
            traj.push_back(Point{ p0.first, p0.second, Z_UP,       false,(uint8_t)baseSpeed,"UP" });
            traj.push_back(Point{ p0.first, p0.second, Z_MID,      false,(uint8_t)baseSpeed,"MID" });
            traj.push_back(Point{ p0.first, p0.second, Z_PRE_DOWN, false,(uint8_t)baseSpeed,"PRE" });
            traj.push_back(Point{ p0.first, p0.second, zdown,      true, (uint8_t)baseSpeed,s.type });
        } else {
            // 关闭顿笔：单层直落，一笔一次冲到书写深度，消除起笔分层戳动
            traj.push_back(Point{ p0.first, p0.second, Z_UP,  false,(uint8_t)baseSpeed,"UP" });
            traj.push_back(Point{ p0.first, p0.second, zdown, true, (uint8_t)baseSpeed,s.type });
        }
        for (size_t k = 1; k < s.centerLine.size(); ++k) {
            auto pk = toLocal(s.centerLine[k]);
            traj.push_back(Point{ pk.first, pk.second, zdown, true,(uint8_t)baseSpeed,s.type });
        }
        auto pe = toLocal(s.centerLine.back());
        if (g_enableDunbi) {
            // 现状：分层收笔 DOWN→PRE→MID
            traj.push_back(Point{ pe.first, pe.second, Z_PRE_DOWN, false,(uint8_t)baseSpeed,"PRE" });
            traj.push_back(Point{ pe.first, pe.second, Z_MID,      false,(uint8_t)baseSpeed,"MID" });
        } else {
            // 关闭顿笔：单层直抬，一笔一次抬到 Z_UP，消除收笔分层戳动
            traj.push_back(Point{ pe.first, pe.second, Z_UP, false,(uint8_t)baseSpeed,"UP" });
        }
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

// —— 方向感知网格（横排 dir=0 与既有实现逐点一致；竖排 dir=1 宽高对调）—— //
// per=每书写线字数（横排=每行、竖排=每列），along=线内字距，cross=线间距。
// 竖排右起：书写线沿 Y（自上而下），线间（列间）沿 X（自右向左）。
static void grid_extents(int n, int per, float S, float along, float cross, int dir,
    float& gw, float& gh, int& nLines)
{
    if (per < 1) per = 1;
    nLines = (n + per - 1) / per;
    float alongSpan = per * S + (per - 1) * along;         // 一条书写线的长度（含 per 格）
    float crossSpan = nLines * S + (nLines - 1) * cross;   // 线间的总跨度
    if (dir == 0) { gw = alongSpan; gh = crossSpan; }      // 横排：线沿X、行间沿Y
    else          { gw = crossSpan; gh = alongSpan; }      // 竖排：列间沿X、列内沿Y
}

// 填充 offsets（相对文本区左上角，oy 为自顶向下的格底，最终世界坐标由调用方叠加）。
// textH 为文本区高，用于把“自顶向下”换算成代码里既有的“自底向上 y”约定（oy = textH - S - …）。
static void fill_offsets(std::vector<Offset>& out, int n, float S, int per,
    float along, float cross, int dir, float textH)
{
    if (per < 1) per = 1;
    int nLines = (n + per - 1) / per;
    out.clear(); out.reserve(n);
    for (int i = 0; i < n; ++i) {
        int line = i / per, pos = i % per;   // line=行/列序，pos=线内序
        float ox, oy;
        if (dir == 0) {                       // 横排左起：line 自上到下，pos 自左到右
            ox = pos * (S + along);
            oy = textH - S - line * (S + cross);
        } else {                              // 竖排右起：pos 列内自上到下，line 列自右到左
            ox = (nLines - 1 - line) * (S + cross);
            oy = textH - S - pos * (S + along);
        }
        out.push_back(Offset{ ox, oy });
    }
}

TextPlan plan_text_area_and_layout(int n, const WorkArea& safe, float init_top_ratio,
    float vgap_mm, float sp_init, int dir)
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

        // 列数候选依方向而变：横排 {5,4}（与旧实现逐点一致）；竖排偏好短列 {4,3,2,5} 以适应有限文本区高。
        const int* cands; int ncand;
        if (dir == 1) { static const int v[] = { 4, 3, 2, 5 }; cands = v; ncand = 4; }
        else          { static const int h[] = { 5, 4 };       cands = h; ncand = 2; }
        for (int ci = 0; ci < ncand; ++ci) {
            int colsTry = cands[ci];
            for (float S = SINGLE_CHAR_MAX; S >= SINGLE_CHAR_MIN - 1e-3f; S -= 1.0f) {
                for (float sp = sp_init; sp >= CHAR_SPACING_MIN - 1e-3f; sp -= 1.0f) {
                    float gw, gh; int nLines = 0;
                    grid_extents(n, colsTry, S, sp, sp, dir, gw, gh, nLines);
                    if (!(gw <= textW + 1e-3f && gh <= textH + 1e-3f)) continue;
                    tp.ok = true; tp.cols = colsTry; tp.rows = nLines; tp.used_S = S; tp.used_sp = sp;
                    tp.row_spacing = sp; tp.top_ratio = ratio; tp.dir = dir;
                    float text_ymax = safe.ymax - vgap_mm * 0.5f;
                    float text_ymin = text_ymax - textH;
                    tp.text_area = WorkArea{ safe.xmin + SAFE_MARGIN, safe.xmin + SAFE_MARGIN + textW, text_ymin, text_ymax };
                    fill_offsets(tp.offsets, n, S, colsTry, sp, sp, dir, textH);
                    return tp;
                }
            }
        }
    }
    tp.ok = false; return tp;
}

// 手动排版：严格按用户给定参数排布，仅做 fit 检查，放不下直接失败（不缩放）。
// 文本区推导与自动模式一致：textH = totalH*top_ratio - vgap/2，textW = totalW - 2*margin，
// 文本区顶贴 safe.ymax - vgap/2，向下取 textH。offsets 采用与自动模式相同的横排左起顺序。
TextPlan plan_manual_layout(int n, const WorkArea& safe, float vgap_mm,
    float S, int cols, float col_spacing, float row_spacing, float top_ratio, int dir)
{
    TextPlan tp;
    if (n <= 0) { tp.err = LAY_NO_CHARS; return tp; }
    if (!std::isfinite(S) || S < SINGLE_CHAR_MIN - 1e-3f) { tp.err = LAY_BAD_PARAM; return tp; } // 60mm 比赛红线
    if (cols < 1 || cols > 200)                          { tp.err = LAY_BAD_PARAM; return tp; }
    if (!std::isfinite(col_spacing) || col_spacing < 0.f) { tp.err = LAY_BAD_PARAM; return tp; }
    if (!std::isfinite(row_spacing) || row_spacing < 0.f) { tp.err = LAY_BAD_PARAM; return tp; }
    if (dir != 0 && dir != 1) dir = 0;

    float totalH = safe.ymax - safe.ymin;
    float totalW = safe.xmax - safe.xmin;
    float textH = totalH * top_ratio - vgap_mm * 0.5f;
    float textW = totalW - SAFE_MARGIN * 2.0f;
    if (textH <= 0 || textW <= 0) { tp.err = LAY_FONT_H; return tp; }

    // 单字放不下（方向无关，按方向给更具体原因）；整体网格放不下 → LAY_GRID。均不缩放。
    if (S > textW + 1e-3f) { tp.err = LAY_FONT_W; return tp; }
    if (S > textH + 1e-3f) { tp.err = LAY_FONT_H; return tp; }
    float gridW, gridH; int nLines = 0;
    grid_extents(n, cols, S, col_spacing, row_spacing, dir, gridW, gridH, nLines);
    if (gridW > textW + 1e-3f || gridH > textH + 1e-3f) { tp.err = LAY_GRID; return tp; }

    tp.ok = true; tp.err = LAY_OK;
    tp.cols = cols; tp.rows = nLines; tp.used_S = S;
    tp.used_sp = col_spacing; tp.row_spacing = row_spacing; tp.top_ratio = top_ratio; tp.dir = dir;
    float text_ymax = safe.ymax - vgap_mm * 0.5f;
    float text_ymin = text_ymax - textH;
    tp.text_area = WorkArea{ safe.xmin + SAFE_MARGIN, safe.xmin + SAFE_MARGIN + textW, text_ymin, text_ymax };
    fill_offsets(tp.offsets, n, S, cols, col_spacing, row_spacing, dir, textH);
    return tp;
}

bool prepare_layout_only(const std::wstring& wtext_in, TextPlan& plan_out, std::wstring& chars_out) {
    chars_out.clear();
    for (wchar_t c : wtext_in) if (isCJKOrPunct(c)) chars_out.push_back(c);
    if (chars_out.empty()) { wprintln(L"[错误] 无有效汉字。"); return false; }
    if (g_layout_mode == 1) {
        // 手动：列间距沿用 CHAR_SPACING，行间距用独立值；字号上不封顶，放不下报因不缩放。
        plan_out = plan_manual_layout((int)chars_out.size(), g_safeArea, V_GAP_BETWEEN,
            g_lm_char_size, g_lm_cols, CHAR_SPACING, g_lm_row_spacing, g_lm_top_ratio, g_write_dir);
    } else {
        plan_out = plan_text_area_and_layout((int)chars_out.size(), g_safeArea, TEXT_TOP_RATIO, V_GAP_BETWEEN, CHAR_SPACING, g_write_dir);
    }
    if (!plan_out.ok) {
        if (g_layout_mode == 1) {
            const wchar_t* why = L"排版失败。";
            switch (plan_out.err) {
                case LAY_FONT_W: why = L"[错误] 手动排版失败：单字宽度超出文本区（减小字号或每行字数）。"; break;
                case LAY_FONT_H: why = L"[错误] 手动排版失败：单字高度超出文本区（减小字号或增大上区占比）。"; break;
                case LAY_GRID:   why = L"[错误] 手动排版失败：整体网格超出文本区（减少字数/增大上区占比/调小行距）。"; break;
                case LAY_BAD_PARAM: why = L"[错误] 手动排版失败：参数非法（字号需 ≥60mm）。"; break;
                default: break;
            }
            wprintln(why);
        } else {
            wprintln(L"[错误] 排版失败。尝试：减少字数/提高上区比例/减小字号/减小字距。");
        }
        return false;
    }
    ACTIVE_CHAR_SIZE = plan_out.used_S;
    TEXT_TOP_RATIO = plan_out.top_ratio;
    CHAR_SPACING = plan_out.used_sp;
    return true;
}
