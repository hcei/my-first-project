// motion.cpp — 运动轨迹处理与发送实现
// 由 Robot.cpp（单文件版）拆分而来；实现逐行保真
// （含首落笔加固、冷启动强节流、批量失败回退逐点等关键逻辑）。
// ★GUI 阶段：transmitTrajectoryWithSplit / do_dip_and_groom / write_text_streamed
//   增加 task::progress 钩子（阶段、字符、轨迹点计数——内存内更新，不打印）。
//   计数不改变任何发送逻辑与节拍。
#include "motion.h"
#include "serial_port.h"
#include "hanzi.h"
#include "gui_service.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <thread>

// —— 降采样/抽稀步长 —— //
// 方案B：书法段落笔点用 Ramer–Douglas–Peucker 按“最大弦高偏差”抽稀——
//   直段塌成两端点（一笔到底、最顺滑），弯曲/拐角自动保点 ⇒ 容差越大、点越少、停顿越少（治顿挫）。
//   现由 config 键 rdp_tol_mm 驱动（默认 0.35；实测 0.35≈减 10% 点、0.5≈17%、0.8≈30%，真减停在 0.5~0.8）。
static float RESAMPLE_STEP_MM_DRAW = 1.2f;   // 描边段沿用定步长抽稀（行为与修复前一致）

// --------------------------- 过滤与降采样 ---------------------------
// ★修复：书法区（原下标 < calli_end_src）去重间距由 1.5mm 收紧为 0.3mm——
//        0.6mm 重采样只能删点不能加点，若先用 1.5mm 去重，密集笔迹会永久丢失细节。
std::vector<Point> filterDuplicatePoints(const std::vector<Point>& in, std::vector<size_t>* srcIndex, size_t calli_end_src) {
    std::vector<Point> out; out.reserve(in.size());
    if (srcIndex) srcIndex->clear();
    auto nearEq = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
    for (size_t i = 0; i < in.size(); ++i) {
        if (out.empty()) { out.push_back(in[i]); if (srcIndex) srcIndex->push_back(i); continue; }
        const Point& prev = out.back();
        float xyEps = (i < calli_end_src) ? 0.3f : 1.5f;
        float zEps = in[i].isPenDown ? 0.6f : 1.5f;
        if (nearEq(prev.x, in[i].x, xyEps) && nearEq(prev.y, in[i].y, xyEps) && nearEq(prev.z, in[i].z, zEps)
            && prev.isPenDown == in[i].isPenDown && prev.speed == in[i].speed) continue;
        out.push_back(in[i]);
        if (srcIndex) srcIndex->push_back(i);
    }
    return out;
}

void resamplePolyline(std::vector<Point>& pts, float minStep) {
    if (pts.size() <= 2) return;
    std::vector<Point> out; out.reserve(pts.size());
    auto dist2d = [](const Point& a, const Point& b) {
        float dx = a.x - b.x, dy = a.y - b.y; return std::sqrt(dx * dx + dy * dy);
        };
    out.push_back(pts.front());
    Point lastKeep = pts.front();
    for (size_t i = 1; i < pts.size() - 1; ++i) {
        if (dist2d(lastKeep, pts[i]) >= minStep) { out.push_back(pts[i]); lastKeep = pts[i]; }
    }
    out.push_back(pts.back());
    pts.swap(out);
}

// 方案B（误差有界的抽稀）：对一段落笔点做 Ramer–Douglas–Peucker。
// 直段会被塌成两端点（一笔到底，最顺滑），弯曲处自动保留足够点，使“保留折线 vs 原始折线”的
// 最大垂直偏差 ≤ tolMm，因此拐角与笔形不会丢失。用显式栈迭代，避免长笔画大点数时的深递归爆栈。
void resamplePolylineRDP(std::vector<Point>& pts, float tolMm) {
    size_t n = pts.size();
    if (n <= 2) return;
    auto perp = [](const Point& p, const Point& a, const Point& b) -> float {
        float abx = b.x - a.x, aby = b.y - a.y;
        float len2 = abx * abx + aby * aby;
        if (len2 < 1e-12f) return std::hypot(p.x - a.x, p.y - a.y);
        float t = ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2;
        t = std::min(1.f, std::max(0.f, t));
        return std::hypot(p.x - (a.x + t * abx), p.y - (a.y + t * aby));
        };
    std::vector<char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(0, n - 1);
    while (!stack.empty()) {
        size_t beg = stack.back().first, end = stack.back().second; stack.pop_back();
        if (end <= beg + 1) continue;
        const Point& A = pts[beg]; const Point& B = pts[end];
        float maxd = -1.f; size_t idx = beg;
        for (size_t i = beg + 1; i < end; ++i) {
            float d = perp(pts[i], A, B);
            if (d > maxd) { maxd = d; idx = i; }
        }
        if (maxd > tolMm) { keep[idx] = 1; stack.emplace_back(beg, idx); stack.emplace_back(idx, end); }
    }
    std::vector<Point> out; out.reserve(n);
    for (size_t i = 0; i < n; ++i) if (keep[i]) out.push_back(pts[i]);
    pts.swap(out);
}

// —— 节奏估时 —— //
// 计时 v 只按【落笔/抬笔】选，不再查速度档：速度档现在【只】决定发给设备的字节(=设备快慢)，与软件节拍无关。
//   落笔 28mm/s：真机分桶实测"已知干净"的碎段计时常数（档1剖面 d/28+C 逐桶吻合；v_req∈[28,42)）。
//   抬笔 128mm/s：抬笔段无墨、截断不可见 ⇒ 走快，抬笔走行/锚点等待大幅缩短（整行 −40%）。
// ⚠ 28/128 是"每点该给多少时间"的计时常数，不是设备巡航速度，禁止填 320mm 预览线实测巡航值(415~1185)。
static const float PEN_DOWN_MM_PER_SEC = 28.0f;
static const float PEN_UP_MM_PER_SEC   = 128.0f;

// 兼容保留：书写计时已改走 isPenDown、不再调用本函数；仅留给"按 level 取字节语义"的旧引用。
float speedLevelToXYmmPerSec(int level) {
    level = std::max(SPEED_MIN, std::min(SPEED_MAX, level));
    return (level >= SPEED_MAX) ? 128.0f : 28.0f;
}
// Phase 1：把「到位时间」从「顿笔开关 / high_quality」解耦——三个 dwell 只读 config。
// 旧实现：!g_enableDunbi 先把 dwell 短路成 0（关顿笔→端点无时间→纯 Z 落笔/抬笔被串口地板卡住→笔画收尾/短笔截断）；
//         g_highQuality && isCalli 又硬编码 160/110/120 覆盖 config（→改 config 三键无效）。
// 现在：顿笔开关只管 hanzi.cpp 的分层下刀/深度，不再管时间；高质档不再自动改 dwell（要更慢更稳就把 config 三键调大）。
int get_Z_SETTLE_MS(bool /*isCalli*/)           { return g_z_settle_ms; }
int get_STROKE_BEGIN_DWELL_MS(bool /*isCalli*/) { return g_stroke_begin_ms; }
int get_STROKE_END_DWELL_MS(bool /*isCalli*/)   { return g_stroke_end_ms; }
int get_POINT_RATE_LIMIT_MS(bool isCalli) { return (g_highQuality && isCalli) ? 90 : POINT_RATE_LIMIT_MS_BASE; }

// 方案A：返回相邻两条指令的“目标间隔”（运动时间 + 必要的物理停顿），
// 不再是“发完后再额外 sleep 的时长”。串口往返与运动本身占用的墙钟时间由调用方补偿扣除。
int estimateMoveMs(const Point& prev, const Point& cur, bool isCalli) {
    float vx = cur.isPenDown ? PEN_DOWN_MM_PER_SEC : PEN_UP_MM_PER_SEC;   // 计时只按落/抬笔，不查速度档
    float dx = cur.x - prev.x, dy = cur.y - prev.y;
    float dxy = std::sqrt(dx * dx + dy * dy);
    int t_xy = (vx > 1e-3f) ? int(std::ceil(dxy / vx * 1000.f)) : 0;

    bool zDownNow = cur.isPenDown;
    bool zDownPrev = prev.isPenDown;
    int extra = 0;
    if (!zDownPrev && zDownNow) extra += get_STROKE_BEGIN_DWELL_MS(isCalli);
    if (zDownPrev && !zDownNow) extra += get_STROKE_END_DWELL_MS(isCalli);
    // ★Z 过渡点(dxy≈0)的总预算 = base + z_settle = C + z_settle。要保住 Phase 1 验证过的 ~107ms
    //   落笔静压(不炸毛)，config 的 z_settle_ms 必须取 107 - C（C=80 → 27）。别留旧值 95（会变 175ms）。
    if (std::fabs(cur.z - prev.z) > 1.0f) extra += get_Z_SETTLE_MS(isCalli);

    // 目标间隔 = max(最小节拍, 运动时间 + 每点固定开销 C) + 物理停顿 extra。
    // C 按落笔段类型分两组：直线段(横/竖)=g_point_fixed_ms、曲线段(撇/捺/弯钩)=g_point_fixed_curve_ms。
    // （strokeKind 由 transmitTrajectoryWithSplit 的两段式 RDP 分类打标。）
    int Cfix = (cur.isPenDown && cur.strokeKind == 1) ? g_point_fixed_curve_ms : g_point_fixed_ms;
    int base = std::max(g_min_point_interval_ms, t_xy + Cfix);
    return base + extra;
}

// 角度
float angle_between(const Point& a, const Point& b, const Point& c) {
    float ux = a.x - b.x, uy = a.y - b.y;
    float vx = c.x - b.x, vy = c.y - b.y;
    float lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);
    if (lu < 1e-6f || lv < 1e-6f) return 0.f;
    float cosv = (ux * vx + uy * vy) / (lu * lv);
    cosv = std::max(-1.0f, std::min(1.0f, cosv));
    return std::acos(cosv);
}

// --------------------------- 串口预热（更新位姿） ---------------------------
void serial_pre_warm(SerialPort& sp) {
    wprintln(L"[预热] 开始...");
    Point center_up{ g_center_x,g_center_y,Z_UP,false,(uint8_t)SPEED_LEVEL,"UP-WARM" };
    int good = 0, tries = 0;
    while (tries < PREWARM_MAX_PROBES && good < PREWARM_MIN_GOOD_RESP) {
        if (sp.sendPointRetry(center_up)) ++good;
        else good = 0;
        ++tries;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    update_pose_from(center_up); // ★记录已到位
    if (good >= PREWARM_MIN_GOOD_RESP) wprintln(L"[预热] 成功，进入稳定态。");
    else wprintln(L"[预热] 未完全达到阈值，继续按正常流程发送。");
}

// --------------------------- 发送轨迹：书法逐点 + 描边批量 + 首落笔加固 + 冷启动强节流 ---------------------------
bool transmitTrajectoryWithSplit(SerialPort& sp, const std::vector<Point>& traj, size_t calli_end)
{
    if (traj.empty()) return true;

    // 去重（防止重复点）；srcIdx 记录保留点的原下标，供书法/描边分界映射
    std::vector<size_t> srcIdx;
    std::vector<Point> filtered = filterDuplicatePoints(traj, &srcIdx, calli_end);

    // 按段抽稀：书法段用 RDP(adaptive=true, param=容差mm)；描边段用定步长(adaptive=false, param=步长mm)
    auto resample_range = [&](size_t beg, size_t end, bool adaptive, float param) -> size_t {
        if (end <= beg) return 0;
        size_t i = beg;
        std::vector<Point> out; out.reserve(end - beg + 16);
        while (i < end) {
            bool pen = filtered[i].isPenDown;
            size_t j = i;
            std::vector<Point> seg;
            for (; j < end && filtered[j].isPenDown == pen; ++j) seg.push_back(filtered[j]);
            if (pen && adaptive) {
                // 两段式分类：先按【直线容差】RDP。塌到 ≤2 点 ⇒ 直线段(横/竖)，直接用；
                // 否则 ⇒ 曲线段(撇/捺/弯钩)，对【原始 seg】按【曲线容差】重算(保形)。
                std::vector<Point> probe = seg;
                resamplePolylineRDP(probe, g_rdp_tol_mm);
                if (probe.size() <= 2) {
                    seg = probe;
                    for (auto& p : seg) p.strokeKind = 0;
                } else {
                    resamplePolylineRDP(seg, g_rdp_tol_curve_mm);
                    for (auto& p : seg) p.strokeKind = 1;
                }
            } else if (pen) {
                resamplePolyline(seg, param);   // 描边段定步长
                for (auto& p : seg) p.strokeKind = 0;
            }
            out.insert(out.end(), seg.begin(), seg.end());
            i = j;
        }
        filtered.erase(filtered.begin() + beg, filtered.begin() + end);
        filtered.insert(filtered.begin() + beg, out.begin(), out.end());
        return out.size();
        };

    // ★修复：calli_end 是调用方按“原轨迹”给出的分界，去重和重采样都会改变点数——
    //        先用 srcIdx 映射到去重后的下标，再在书法段重采样后用返回值更新，
    //        否则混合轨迹（前段书法+后段描边）会发生分界错位。
    calli_end = (size_t)(std::lower_bound(srcIdx.begin(), srcIdx.end(), calli_end) - srcIdx.begin());
    calli_end = std::min(calli_end, filtered.size());
    if (calli_end > 0) calli_end = resample_range(0, calli_end, true, g_rdp_tol_mm);
    if (calli_end < filtered.size()) resample_range(calli_end, filtered.size(), false, RESAMPLE_STEP_MM_DRAW);

    const float corner_theta_rad = 75.0f * 3.1415926f / 180.0f;
    const int   corner_dwell_ms = 40;
    const int   hard_corner_ms = 80;

    // 冷启动：本次调用开始时间
    auto t_start = std::chrono::steady_clock::now();

    // 方案A（节拍重整）：由“发完指令后再固定空等”改为“补偿式计时”——
    // 串口往返与运动本身占用的墙钟时间计入指令间隔，只补睡到目标节拍，消除每点的死等。
    auto ms_since = [](std::chrono::steady_clock::time_point a) -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - a).count();
        };
    // 目标指令间隔（含物理停顿 extra）；prev 为空表示无参考点（本段/本次首点）。
    auto interval_for = [&](const Point* prev, const Point& cur, bool isCalli, int extra) -> int {
        int base = prev ? estimateMoveMs(*prev, cur, isCalli) : g_min_point_interval_ms;
        return base + extra;
        };
    // 从 t0（发送前时刻）补偿等待到目标间隔；冷启动首秒内给更保守的下限。
    auto wait_after_send = [&](std::chrono::steady_clock::time_point t0, int interval_ms) {
        if (ms_since(t_start) < 1000) interval_ms = std::max(interval_ms, g_cold_start_min_ms);
        long long remain = (long long)interval_ms - ms_since(t0);
        if (remain > 0) std::this_thread::sleep_for(std::chrono::milliseconds(remain));
        };

    // 命中停止（急停/停止任务都会置 g_estop）：按当前 XY 抬笔复位，避免笔尖停在纸面。
    auto estopLiftAt = [&](const Point& cur) {
        wprintln(L"[急停] 停止。抬笔复位。");
        Point up = cur; up.z = Z_UP; up.isPenDown = false; up.zType = "UP-ESTOP";
        sp.sendPointRetry(up);
        };

    const Point* lastSent = nullptr;
    bool firstDownDuplicated = false; // 仅“本次调用”的首个落笔点加固

    size_t i = 0;
    while (i < filtered.size()) {
        bool inCalli = (i < calli_end);

        if (g_estop) { estopLiftAt(filtered[i]); return false; }

        if (!filtered[i].isPenDown) {
            // 抬笔段：逐点发送（用于定位）
            const Point& p = filtered[i];
            auto t0 = std::chrono::steady_clock::now();
            if (!sp.sendPointRetry(p)) return false;
            gs::task::traj_add_done(1, false);   // ★GUI 钩子：轨迹点进度
            wait_after_send(t0, interval_for(lastSent, p, inCalli, 0));
            lastSent = &filtered[i];
            ++i;
            continue;
        }

        // —— 落笔段 —— //
        if (inCalli) {
            // 书法段：逐点发送；首落笔点重复一次并停顿
            size_t j = i; while (j < filtered.size() && filtered[j].isPenDown) ++j; // [i, j)

            for (size_t k = i; k < j; ++k) {
                const Point& p = filtered[k];
                if (g_estop) { estopLiftAt(p); return false; }   // ★逐点轮询：一笔之内也能停

                // 首落笔加固：检测“由抬到落”的第一个点，且尚未加固过（关闭顿笔时跳过此重压加固）
                bool isFirstDownThisCall = g_enableDunbi && (!firstDownDuplicated) && ((k == 0) || !filtered[k - 1].isPenDown);
                if (isFirstDownThisCall) {
                    // 第一次落笔：重复发送两次，中间停顿；再额外沉稳
                    auto t0 = std::chrono::steady_clock::now();
                    if (!sp.sendPointRetry(p)) return false;
                    wait_after_send(t0, interval_for(lastSent, p, true, 0));
                    std::this_thread::sleep_for(std::chrono::milliseconds(FIRST_DOWN_DUP_SEND_PAUSE));

                    if (!sp.sendPointRetry(p)) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(FIRST_DOWN_EXTRA_DWELL_MS));

                    // 再额外保持一小段，确保压力建立
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));

                    lastSent = &filtered[k];
                    firstDownDuplicated = true;
                    gs::task::traj_add_done(1, true);   // ★GUI 钩子（首落笔点）
                    continue;
                }

                auto t0 = std::chrono::steady_clock::now();
                if (!sp.sendPointRetry(p)) return false;
                gs::task::traj_add_done(1, true);       // ★GUI 钩子：书法逐点进度

                // 角点额外停顿：折进本点指令间隔（补偿式，不再叠加在串口耗时之上）
                int corner_extra = 0;
                if (k >= 1 && k + 1 < j) {
                    // angle_between 返回【内角】：直段续接=π(180°)、真拐角=π/2、发夹≈0。
                    // 旧判据 ang>θ 会把 +停顿打在【直线点】上(100% 命中)、反而漏掉真发夹——逻辑反了。
                    // 正确：用【转折角】turn = π - ang，只在方向真的改变处停顿。（此停顿与 estimateMoveMs 的 C 同批引入）
                    float ang = angle_between(filtered[k - 1], filtered[k], filtered[k + 1]);
                    float turn = 3.1415926f - ang;
                    if (turn > corner_theta_rad) corner_extra = hard_corner_ms;
                    else if (turn > corner_theta_rad * 0.6f) corner_extra = corner_dwell_ms;
                }

                wait_after_send(t0, interval_for(lastSent, p, true, corner_extra));
                lastSent = &filtered[k];
            }
            i = j; // 下一段
        }
        else {
            // 描边段：尽量批量每7点；失败回退到逐点
            size_t j = i; while (j < filtered.size() && filtered[j].isPenDown) ++j; // [i,j)
            size_t len = j - i, k = 0;
            while (k < len) {
                if (g_estop) { estopLiftAt(filtered[i + k]); return false; }   // ★逐块轮询：描边段也能停
                size_t chunk = std::min<size_t>(7, len - k);
                std::vector<Point> batch(filtered.begin() + i + k, filtered.begin() + i + k + chunk);

                auto t0batch = std::chrono::steady_clock::now();
                if (!sp.sendPointsBatch7(batch)) {
                    // 回退逐点 —— 用 filtered 的下标，保证 lastSent 指针稳定
                    for (size_t t = 0; t < batch.size(); ++t) {
                        const Point& p_ref = filtered[i + k + t];  // 指向 filtered 中的真实元素
                        if (g_estop) { estopLiftAt(p_ref); return false; }
                        auto t0 = std::chrono::steady_clock::now();
                        if (!sp.sendPointRetry(p_ref)) return false;
                        gs::task::traj_add_done(1, true);           // ★GUI 钩子：回退逐点进度
                        wait_after_send(t0, interval_for(lastSent, p_ref, false, 0));
                        lastSent = &p_ref;                          // 指向稳定存储
                    }
                }
                else {
                    const Point& plast = filtered[i + k + chunk - 1];
                    gs::task::traj_add_done(chunk, true);           // ★GUI 钩子：批量进度
                    wait_after_send(t0batch, interval_for(lastSent, plast, false, 0));
                    lastSent = &plast;
                }
                k += chunk;
            }
            i = j;
        }
    }
    return true;
}

// --------------------------- 蘸墨流程（两次蘸→十字抖→离液面“上慢下快”5次） ---------------------------
bool do_dip_and_groom(SerialPort& sp, const InkStation& ink) {
    if (!g_enableDip) return true;          // 总开关：默认关闭
    if (!ink.valid) {
        wprintln(L"[错误] 已开启蘸墨但未设置蘸墨位（菜单14），中止任务。");
        return false;
    }
    const uint8_t SLOW = 1;
    const uint8_t FAST = (uint8_t)std::min(6, std::max(2, SPEED_LEVEL));
    const float dx = 8.0f, dy = 8.0f; // 十字抖动幅度

    std::vector<Point> traj; traj.reserve(128);

    // 到位（抬笔）
    traj.push_back(Point{ ink.x, ink.y, Z_UP,  false, SLOW, "UP-INK" });
    traj.push_back(Point{ ink.x, ink.y, Z_MID, false, SLOW, "MID-INK" });

    // —— 两次蘸 —— //
    for (int t = 0; t < 2; ++t) {
        traj.push_back(Point{ ink.x, ink.y, ink.z, false, SLOW, "DIP" });
        traj.push_back(Point{ ink.x, ink.y, Z_MID, false, SLOW, "RISE" });
    }

    // —— 十字抖动（在 Z_MID 附近）—— //
    traj.push_back(Point{ ink.x - dx, ink.y,     Z_MID, false, SLOW, "GROOM-X" });
    traj.push_back(Point{ ink.x + dx, ink.y,     Z_MID, false, SLOW, "GROOM-X" });
    traj.push_back(Point{ ink.x,      ink.y,     Z_MID, false, SLOW, "GROOM-C" });
    traj.push_back(Point{ ink.x,      ink.y - dy,Z_MID, false, SLOW, "GROOM-Y" });
    traj.push_back(Point{ ink.x,      ink.y + dy,Z_MID, false, SLOW, "GROOM-Y" });
    traj.push_back(Point{ ink.x,      ink.y,     Z_MID, false, SLOW, "GROOM-C" });

    // —— 离液面后上下抖 5 次：上慢下快 —— //
    for (int k = 0; k < 5; ++k) {
        traj.push_back(Point{ ink.x, ink.y, Z_MID, false, SLOW, "SHAKE-UP-SLOW" });   // 上拉慢
        traj.push_back(Point{ ink.x, ink.y, ink.z, false, FAST, "SHAKE-DOWN-FAST" }); // 迅速下
    }
    // 结束抬笔
    traj.push_back(Point{ ink.x, ink.y, Z_UP, false, SLOW, "UP-END" });

    // 实际发送（作为“描边”逻辑：不需要角点停顿，calli_end=0）
    bool ok = transmitTrajectoryWithSplit(sp, traj, 0);
    if (ok) gs::task::dip_done();   // ★GUI 钩子：一次完整蘸墨流程完成（合规提示用）
    return ok;
}

// --------------------------- ★抬笔预定位→等待（首字/每行首字用） ---------------------------
bool move_up_to_and_wait(SerialPort& sp, const Point& targetUp, bool isCalli) {
    // 发送会立即更新软件目标位姿，因此必须在发送前保存旧位置用于估算预定位距离。
    Point prev = g_pose_init ? g_last_pose : Point{ 0.f,0.f,Z_UP,false,(uint8_t)SPEED_LEVEL,"POSE0" };

    // 1) 先抬笔到目标XY（保证安全）
    Point up = targetUp;
    up.z = Z_UP;
    up.isPenDown = false;
    up.zType = "UP-PREPOS";
    if (!sp.sendPointRetry(up)) return false;

    // 2) 估算到位时间：抬笔预定位 → 用抬笔计时常数（更快），不再查 level
    float vx = PEN_UP_MM_PER_SEC;
    float dx = up.x - prev.x, dy = up.y - prev.y;
    float dxy = std::sqrt(dx * dx + dy * dy);
    int t_xy = (vx > 1e-3f) ? int(std::ceil(dxy / vx * 1000.f)) : 0;

    // 3) 加冗余：远距离给更大guard；再加 Z settle
    int guard = (dxy > 20.f ? 600 : 250) + get_Z_SETTLE_MS(isCalli);
    {
        std::wstringstream ws;
        ws << L"[预定位] dXY=" << dxy << L"mm 估计t=" << t_xy << L"ms + guard=" << guard << L"ms";
        wprintln(ws.str());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(t_xy, get_POINT_RATE_LIMIT_MS(isCalli)) + guard));

    update_pose_from(up);
    return true;
}

// --------------------------- 逐字流式写字（预热；可选蘸墨；首字/每行首字预定位） ---------------------------
bool write_text_streamed(SerialPort& sp, const std::wstring& chars, const TextPlan& plan, int baseSpeed, int per_char_pause_ms) {
    // 串口预热
    serial_pre_warm(sp);

    // 开写前可选蘸墨（默认关闭，用菜单15开启）
    if (g_enableDip) {
        if (!do_dip_and_groom(sp, g_ink)) {
            wprintln(L"[错误] 蘸墨流程失败，禁止开始书写。");
            return false;
        }
    }

    for (size_t i = 0; i < chars.size(); ++i) {
        if (g_estop) return false;

        MMAHCharData ch;
        if (!loadCharData(chars[i], ch)) {
            std::wstringstream ws; ws << L"[警告] 找不到字形数据，跳过：" << std::wstring(1, chars[i]);
            wprintln(ws.str());
            continue;
        }

        std::vector<Point> local;
        if (!generateSingleCharTrajectory(ch, baseSpeed, local, ACTIVE_CHAR_SIZE)) continue;

        // 加上版面偏移
        std::vector<Point> one; one.reserve(local.size() + 2);
        const Offset of = plan.offsets[(int)i];
        for (const auto& p : local) {
            one.push_back(Point{
                plan.text_area.xmin + of.x + p.x,
                plan.text_area.ymin + of.y + p.y,
                p.z, p.isPenDown, p.speed, p.zType
                });
        }

        // ★ 首字 或 每行首字：抬笔预定位→等待，再写整字
        if (i == 0 || (plan.cols > 0 && (int)i % plan.cols == 0)) {
            if (!one.empty()) {
                Point firstUp = one.front();
                firstUp.z = Z_UP;
                firstUp.isPenDown = false;
                firstUp.zType = (i == 0) ? "UP-FIRST-ANCHOR" : "UP-LINE-ANCHOR";
                if (!move_up_to_and_wait(sp, firstUp, /*isCalli*/true)) return false;
                // 额外沉稳一点（开启蘸墨时更久）
                std::this_thread::sleep_for(std::chrono::milliseconds(g_enableDip ? 400 : 200));
            }
        }

        // 发送（书法全部算 calli 段）
        if (!transmitTrajectoryWithSplit(sp, one, one.size())) return false;
        gs::task::char_done((int)i);          // ★GUI 钩子：第 i 个字符书写完成

        // 每写5个字再蘸一次（开启时）
        if (g_enableDip && (((int)(i + 1)) % 5 == 0)) {
            if (!do_dip_and_groom(sp, g_ink)) {
                wprintln(L"[错误] 中途蘸墨失败，书写任务终止。");
                return false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(per_char_pause_ms));
    }

    // 收尾：若总字数不是5的倍数且开启蘸墨，再补一次
    if (g_enableDip && (((int)chars.size() % 5) != 0)) {
        if (!do_dip_and_groom(sp, g_ink)) {
            wprintln(L"[错误] 收尾蘸墨失败，任务未完成。");
            return false;
        }
    }
    return true;
}
