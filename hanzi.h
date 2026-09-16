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
TextPlan plan_text_area_and_layout(int n, const WorkArea& safe, float init_top_ratio,
    float vgap_mm, float sp_init);
bool prepare_layout_only(const std::wstring& wtext_in, TextPlan& plan_out, std::wstring& chars_out);
