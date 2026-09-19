// hanzi.h — HanziWriter 汉字数据解析、单字轨迹生成与多字排版
// 由 Robot.cpp（单文件版）拆分而来；行为保持不变。
#pragma once
#include "robot_common.h"

void bboxOfStrokes(const std::vector<MMAHStroke>& ss, float& minx, float& maxx, float& miny, float& maxy);
MMAHCharData parseHanziWriterJson(const std::string& path, wchar_t wch);
bool loadCharData(wchar_t wc, MMAHCharData& out);
bool generateSingleCharTrajectory(const MMAHCharData& ch, int baseSpeed, std::vector<Point>& traj, float size_mm);

// —— 安全区/布局 —— //
bool layout_fit(int n, float W, float H, float S, float sp, int cols, int& out_rows);
// dir: 0=横排左起（线沿X、行间沿Y）；1=竖排右起（列内从上到下、列从右往左）。
TextPlan plan_text_area_and_layout(int n, const WorkArea& safe, float init_top_ratio,
    float vgap_mm, float sp_init, int dir = 0);
// 手动排版：按 S/每行字数/行间距/上区占比/方向 严格排布，做纯 fit 检查（放不下置 ok=false 与 err，不缩放）。
TextPlan plan_manual_layout(int n, const WorkArea& safe, float vgap_mm,
    float S, int cols, float col_spacing, float row_spacing, float top_ratio, int dir = 0);
bool prepare_layout_only(const std::wstring& wtext_in, TextPlan& plan_out, std::wstring& chars_out);
