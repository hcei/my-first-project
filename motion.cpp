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

// —— 降采样步长（transmitTrajectoryWithSplit 当前使用等值字面量，变量保留以免行为差异） —— //
static float RESAMPLE_STEP_MM_CALLI = 0.6f;
static float RESAMPLE_STEP_MM_DRAW = 1.2f;

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

// —— 节奏估时 —— //
float speedLevelToXYmmPerSec(int level) {
    static const float table_normal[7] = { 0,40,60,80,110,140,180 };
    static const float table_quality[7] = { 0,28,42,56,80,100,128 };
    level = std::max(SPEED_MIN, std::min(SPEED_MAX, level));
    return g_highQuality ? table_quality[level] : table_normal[level];
}
int get_Z_SETTLE_MS(bool isCalli) { return (g_highQuality && isCalli) ? 160 : Z_SETTLE_MS_BASE; }
int get_STROKE_BEGIN_DWELL_MS(bool isCalli) { return (g_highQuality && isCalli) ? 110 : STROKE_BEGIN_DWELL_MS_BASE; }
int get_STROKE_END_DWELL_MS(bool isCalli) { return (g_highQuality && isCalli) ? 120 : STROKE_END_DWELL_MS_BASE; }
int get_POINT_RATE_LIMIT_MS(bool isCalli) { return (g_highQuality && isCalli) ? 90 : POINT_RATE_LIMIT_MS_BASE; }

int estimateMoveMs(const Point& prev, const Point& cur, bool isCalli) {
    float vx = speedLevelToXYmmPerSec(cur.speed);
    float dx = cur.x - prev.x, dy = cur.y - prev.y;
    float dxy = std::sqrt(dx * dx + dy * dy);
    int t_xy = (vx > 1e-3f) ? int(std::ceil(dxy / vx * 1000.f)) : 0;

    bool zDownNow = cur.isPenDown;
    bool zDownPrev = prev.isPenDown;
    int extra = 0;
    if (!zDownPrev && zDownNow) extra += get_STROKE_BEGIN_DWELL_MS(isCalli);
    if (zDownPrev && !zDownNow) extra += get_STROKE_END_DWELL_MS(isCalli);
    if (std::fabs(cur.z - prev.z) > 1.0f) extra += get_Z_SETTLE_MS(isCalli);

    int base = std::max(get_POINT_RATE_LIMIT_MS(isCalli), t_xy);
    base = std::max(base, DELAY_MS);
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

    // 按段降采样：书法更密、描边稍稀（返回替换后该段的新点数，供修正分界）
    auto resample_range = [&](size_t beg, size_t end, float step) -> size_t {
        if (end <= beg) return 0;
        size_t i = beg;
        std::vector<Point> out; out.reserve(end - beg + 16);
        while (i < end) {
            bool pen = filtered[i].isPenDown;
            size_t j = i;
            std::vector<Point> seg;
            for (; j < end && filtered[j].isPenDown == pen; ++j) seg.push_back(filtered[j]);
            if (pen) resamplePolyline(seg, step);
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
    if (calli_end > 0) calli_end = resample_range(0, calli_end, 0.6f);   // RESAMPLE_STEP_MM_CALLI（已定义为 0.6）
    if (calli_end < filtered.size()) resample_range(calli_end, filtered.size(), 1.2f); // RESAMPLE_STEP_MM_DRAW（1.2）

    const float corner_theta_rad = 75.0f * 3.1415926f / 180.0f;
    const int   corner_dwell_ms = 40;
    const int   hard_corner_ms = 80;

    // 冷启动：本次调用开始时间
    auto t_start = std::chrono::steady_clock::now();

    auto sleep_move = [&](const Point* prev, const Point& cur, bool isCalli) {
        int ms = prev ? estimateMoveMs(*prev, cur, isCalli)
            : std::max(get_POINT_RATE_LIMIT_MS(isCalli), DELAY_MS);
        // 冷启动首秒强限速（设备刚“醒”时更保守）
        auto ms_from_start = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        if (ms_from_start < 1000) ms = std::max(ms, 150);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };

    const Point* lastSent = nullptr;
    bool firstDownDuplicated = false; // 仅“本次调用”的首个落笔点加固

    size_t i = 0;
    while (i < filtered.size()) {
        bool inCalli = (i < calli_end);

        if (g_estop) {
            wprintln(L"[急停] 停止。抬笔复位。");
            Point up = filtered[i]; up.z = Z_UP; up.isPenDown = false; up.zType = "UP-ESTOP";
            sp.sendPointRetry(up);
            return false;
        }

        if (!filtered[i].isPenDown) {
            // 抬笔段：逐点发送（用于定位）
            const Point& p = filtered[i];
            if (!sp.sendPointRetry(p)) return false;
            gs::task::traj_add_done(1, false);   // ★GUI 钩子：轨迹点进度
            sleep_move(lastSent, p, inCalli);
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

                // 首落笔加固：检测“由抬到落”的第一个点，且尚未加固过
                bool isFirstDownThisCall = (!firstDownDuplicated) && ((k == 0) || !filtered[k - 1].isPenDown);
                if (isFirstDownThisCall) {
                    // 第一次落笔：重复发送两次，中间停顿；再额外沉稳
                    if (!sp.sendPointRetry(p)) return false;
                    sleep_move(lastSent, p, true);
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

                if (!sp.sendPointRetry(p)) return false;
                gs::task::traj_add_done(1, true);       // ★GUI 钩子：书法逐点进度

                // 角点轻微停顿
                if (k >= 1 && k + 1 < j) {
                    float ang = angle_between(filtered[k - 1], filtered[k], filtered[k + 1]);
                    if (ang > corner_theta_rad) std::this_thread::sleep_for(std::chrono::milliseconds(hard_corner_ms));
                    else if (ang > corner_theta_rad * 0.6f) std::this_thread::sleep_for(std::chrono::milliseconds(corner_dwell_ms));
                }

                sleep_move(lastSent, p, true);
                lastSent = &filtered[k];
            }
            i = j; // 下一段
        }
        else {
            // 描边段：尽量批量每7点；失败回退到逐点
            size_t j = i; while (j < filtered.size() && filtered[j].isPenDown) ++j; // [i,j)
            size_t len = j - i, k = 0;
            while (k < len) {
                size_t chunk = std::min<size_t>(7, len - k);
                std::vector<Point> batch(filtered.begin() + i + k, filtered.begin() + i + k + chunk);

                if (!sp.sendPointsBatch7(batch)) {
                    // 回退逐点 —— 用 filtered 的下标，保证 lastSent 指针稳定
                    for (size_t t = 0; t < batch.size(); ++t) {
                        const Point& p_ref = filtered[i + k + t];  // 指向 filtered 中的真实元素
                        if (!sp.sendPointRetry(p_ref)) return false;
                        gs::task::traj_add_done(1, true);           // ★GUI 钩子：回退逐点进度
                        sleep_move(lastSent, p_ref, false);
                        lastSent = &p_ref;                          // 指向稳定存储
                    }
                }
                else {
                    const Point& plast = filtered[i + k + chunk - 1];
                    gs::task::traj_add_done(chunk, true);           // ★GUI 钩子：批量进度
                    sleep_move(lastSent, plast, false);
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

    // 2) 估算到位时间：若没有历史pose，就按 0,0,UP 估
    float vx = speedLevelToXYmmPerSec(up.speed);
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
