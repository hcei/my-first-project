// polyline.h — 折线 JSON（作画）加载、适配与轨迹生成
// 由 Robot.cpp（单文件版）拆分而来；行为保持不变。
#pragma once
#include "robot_common.h"

bool loadPolylinesFromJson(const std::string& path, std::vector<PolyPath2D>& polys, float& mm_per_unit, float& user_scale);
void bboxOfPolys(const std::vector<PolyPath2D>& polys, float& minx, float& maxx, float& miny, float& maxy);
void fitPolylinesToArea(std::vector<PolyPath2D>& polys, const WorkArea& area);
void generatePolylineTrajectory(const std::vector<PolyPath2D>& polys, std::vector<Point>& traj, int speed, float drawZ);
bool drawPolylinesFromFile(const std::string& path, std::vector<Point>& traj);
