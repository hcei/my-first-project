#pragma once
// shanshui_gen.hpp — Expanded Imagery Edition (header-only, C++17)
// 可离线、可配置、可拓展：在原有 山/水/云/松/竹/舟/鸟/日/月/雪 基础上，新增：
// 亭(亭台)、塔(宝塔)、桥(拱桥/平桥)、瀑布、芦苇/蒹葭、荷塘(莲叶/花)、礁石/怪石、村舍、
/* 小径(山路)、风帆(远帆)、海浪、薄雾带、细雨、星点 等。

使用：
  #include "shanshui_gen.hpp"
  shanshui::GenConfig cfg;
  cfg.enable_mist = true; cfg.enable_rain = false; // 等选项
  generate_shanshui_json_ex(L"渔舟唱晚 芦苇 荷塘 远帆 亭", "themes/more.json", &cfg);

保持兼容：generate_shanshui_json(...) 旧接口仍可用。
*/

#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <limits>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <Windows.h>
#endif

#include "nlohmann/json.hpp"
using json = nlohmann::json;

namespace shanshui {

static constexpr double PI = 3.141592653589793238462643383279502884;

struct Pt { double x, y; };
struct Poly { std::vector<Pt> pts; bool closed{false}; };

struct Canvas {
    double xmin=-150, xmax=150;
    double ymin=-120, ymax=120;
    double width()  const { return xmax - xmin; }
    double height() const { return ymax - ymin; }
};

struct Scene {
    Canvas cv;
    std::vector<Poly> polys;
};

// ---------------- 配置 ----------------
struct GenConfig {
    Canvas canvas;

    // 缺省补全
    bool default_mountain_if_none = true;
    bool default_cloud = true;
    bool auto_water_if_boat = true;

    // 经典元素
    int  mountain_layers_min = 2;
    int  mountain_layers_max = 3;
    double mountain_amp_min_ratio = 0.12;
    double mountain_amp_max_ratio = 0.22;

    int  cloud_count_min = 2;
    int  cloud_count_max = 4;
    bool enable_birds = true;
    int  birds_min = 5;
    int  birds_max = 10;
    bool enable_snow = true;
    int  snow_base = 100;
    int  snow_rand = 80;
    double sun_radius  = 12;
    double moon_radius = 10;

    // 新增元素开关与强度
    bool enable_pavilion = true;      // 亭
    bool enable_pagoda   = true;      // 塔
    bool enable_bridge   = true;      // 桥
    bool enable_waterfall= true;      // 瀑
    bool enable_reeds    = true;      // 芦苇
    bool enable_lotus    = true;      // 荷塘
    bool enable_rocks    = true;      // 怪石
    bool enable_village  = true;      // 村舍
    bool enable_path     = true;      // 山路/小径
    bool enable_sail     = true;      // 远帆
    bool enable_waves    = true;      // 海浪
    bool enable_mist     = true;      // 薄雾带
    bool enable_rain     = true;      // 细雨
    bool enable_stars    = true;      // 星

    // 数量参数（简化）
    int reeds_clumps = 5;
    int lotus_clumps = 4;
    int rock_groups  = 3;
    int village_houses = 3;
    int mist_bands   = 2;
    int star_count   = 80;

    // 随机种子（0=按文本哈希）
    uint64_t seed = 0;
};

// ---------------- 随机/噪声 ----------------
struct RNG {
    std::mt19937_64 gen;
    explicit RNG(uint64_t seed):gen(seed){}
    double uni(double a=0.0,double b=1.0){ std::uniform_real_distribution<double> d(a,b); return d(gen); }
    int    randint(int a,int b){ std::uniform_int_distribution<int> d(a,b); return d(gen); }
};
static inline double lerp(double a,double b,double t){return a+(b-a)*t;}
static inline double fade(double t){return t*t*(3 - 2*t);}

struct ValueNoise1D {
    std::vector<double> table; double step; double x0;
    ValueNoise1D(RNG& rng, int gridN=4096, double _x0=0.0,double _step=1.0): table(gridN), step(_step), x0(_x0){
        for(double& v : table) v = rng.uni(-1.0,1.0);
    }
    double at(double x) const {
        double u = (x - x0)/step;
        int i0 = (int)std::floor(u), i1 = i0 + 1;
        auto g = [&](int i){ int m=(int)table.size(); i=(i%m+m)%m; return table[i]; };
        double t = u - i0;
        return lerp(g(i0), g(i1), fade(t));
    }
};
static double fbm(ValueNoise1D& vn, double x, int oct=4, double lac=2.0, double gain=0.5){
    double a=1.0, f=1.0, sum=0.0, amp=0.0;
    for(int i=0;i<oct;i++){ sum += a * vn.at(x*f); amp += a; a*=gain; f*=lac; }
    return (amp>0? sum/amp : 0.0);
}

// ---------------- 基础元素 ----------------
static void add_poly(Scene& S, const Poly& p){ S.polys.push_back(p); }

static void add_mountain_range(Scene& S, RNG& rng, double y_mid, double amp, int layers=2){
    ValueNoise1D vn(rng, 4096, 0.0, 20.0);
    for(int L=0; L<layers; ++L){
        Poly ridge;
        int N = 600;
        double a  = amp * (1.0 - 0.35*L);
        double ym = y_mid - L * (amp*0.5);
        for(int i=0;i<=N;i++){
            double t = (double)i / N;
            double x = S.cv.xmin + t * S.cv.width();
            double n = fbm(vn, x*0.7 + L*131.7, 5, 2.1, 0.55);
            double y = ym + n * a;
            ridge.pts.push_back({x,y});
        }
        add_poly(S, ridge);
    }
}

static Pt bezier2(Pt p0, Pt p1, Pt p2, double t){
    double u = 1-t;
    return { u*u*p0.x + 2*u*t*p1.x + t*t*p2.x,
             u*u*p0.y + 2*u*t*p1.y + t*t*p2.y };
}
static void add_river(Scene& S, RNG& rng, Pt p0, Pt p1, Pt p2, double half_width){
    Poly center; int N=300;
    for(int i=0;i<=N;i++){ double t=(double)i/N; center.pts.push_back(bezier2(p0,p1,p2,t)); }
    Poly left,right;
    for(int i=0;i<(int)center.pts.size();++i){
        Pt a = (i? center.pts[i-1] : center.pts[i]);
        Pt b = (i+1<(int)center.pts.size()? center.pts[i+1] : center.pts[i]);
        double dx=b.x-a.x, dy=b.y-a.y, len=std::hypot(dx,dy); if(len<1e-6) len=1.0;
        double nx=-dy/len, ny=dx/len;
        left.pts.push_back({ center.pts[i].x + nx*half_width, center.pts[i].y + ny*half_width });
        right.pts.push_back({ center.pts[i].x - nx*half_width, center.pts[i].y - ny*half_width });
    }
    add_poly(S, left); add_poly(S, right);
}
static void add_cloud(Scene& S, RNG& rng, Pt c, double rx, double ry, int lobes=3){
    Poly edge; edge.closed=true;
    int N = 100*lobes; double phase = rng.uni(0, 2*PI);
    for(int i=0;i<=N;i++){
        double t = (double)i/N * 2*PI;
        double rmod = 1.0 + 0.15*std::sin(lobes*t + phase);
        edge.pts.push_back({ c.x + rx*rmod*std::cos(t), c.y + ry*rmod*std::sin(t) });
    }
    add_poly(S, edge);
}
static void add_pine(Scene& S, RNG&, Pt base, double h, int branch_layers=5){
    Poly trunk; trunk.pts.push_back(base); trunk.pts.push_back({base.x, base.y+h}); add_poly(S, trunk);
    for(int i=1;i<=branch_layers;i++){
        double y = base.y + h*(double)i/(branch_layers+1);
        double span=(h*0.6)*(1.0 - (double)i/(branch_layers+1));
        Poly b1{{{base.x,y},{base.x-span,y+span*0.2}}}, b2{{{base.x,y},{base.x+span,y+span*0.2}}};
        add_poly(S, b1); add_poly(S, b2);
    }
}
static void add_bamboo(Scene& S, RNG& rng, Pt base, double h, int joints=6){
    Poly stalk;
    for(int i=0;i<=joints;i++){
        double y = base.y + h*(double)i/joints;
        if(i==0) stalk.pts.push_back({base.x, y}); else stalk.pts.push_back({base.x + rng.uni(-1.5,1.5), y});
        Poly node{{{base.x-4,y},{base.x+4,y}}}; add_poly(S, node);
    }
    add_poly(S, stalk);
    for(int i=0;i<12;i++){
        double y = base.y + rng.uni(0.2,0.9)*h, len=rng.uni(12,24), ang=rng.uni(-PI/3,PI/3);
        Poly leaf{{{base.x,y},{base.x+len*std::cos(ang), y+len*std::sin(ang)}}}; add_poly(S, leaf);
    }
}
static void add_boat(Scene& S, RNG&, Pt c, double w){
    double h = w*0.25;
    Poly hull{{{c.x-w*0.6,c.y},{c.x-w*0.3,c.y-h},{c.x+w*0.3,c.y-h},{c.x+w*0.6,c.y}}}; add_poly(S, hull);
    Poly hood{{{c.x-w*0.15,c.y-h},{c.x-w*0.05,c.y-h*1.6},{c.x+w*0.15,c.y-h}}}; add_poly(S, hood);
    Poly man {{ {c.x-w*0.22,c.y-h*0.9},{c.x-w*0.15,c.y-h*1.35} }}; add_poly(S, man);
    Poly rod {{ {c.x-w*0.15,c.y-h*1.35},{c.x-w*0.55,c.y-h*1.55} }}; add_poly(S, rod);
}
static void add_birds(Scene& S, RNG& rng, Pt a, Pt b, int n=7){
    for(int i=0;i<n;i++){
        double x=rng.uni(a.x,b.x), y=rng.uni(a.y,b.y), s=rng.uni(4.0,8.0);
        Poly v{{{x-s,y},{x,y+s*0.5},{x+s,y}}}; add_poly(S, v);
    }
}
static void add_disc(Scene& S, Pt c, double r){
    Poly cir; cir.closed=true; int N=120;
    for(int i=0;i<=N;i++){ double t=(double)i/N*2*PI; cir.pts.push_back({c.x+r*std::cos(t), c.y+r*std::sin(t)}); }
    add_poly(S, cir);
}
static void add_snow(Scene& S, RNG& rng, int n=120){
    for(int i=0;i<n;i++){
        double x=rng.uni(S.cv.xmin,S.cv.xmax), y=rng.uni(S.cv.ymin,S.cv.ymax), d=rng.uni(1.5,3.5), ang=rng.uni(0,2*PI);
        Poly f{{{x-d*0.5*std::cos(ang), y-d*0.5*std::sin(ang)},
                {x+d*0.5*std::cos(ang), y+d*0.5*std::sin(ang)}}}; add_poly(S,f);
    }
}

// ---------------- 新增元素实现 ----------------
static void add_pavilion(Scene& S, Pt c, double w){
    double h = w*0.6;
    // 柱
    Poly left{{{c.x-w*0.45,c.y},{c.x-w*0.45,c.y+h*0.6}}};
    Poly right{{{c.x+w*0.45,c.y},{c.x+w*0.45,c.y+h*0.6}}};
    add_poly(S,left); add_poly(S,right);
    // 檐
    Poly roof;
    roof.pts = {{c.x-w*0.6,c.y+h*0.6},{c.x-w*0.2,c.y+h*0.9},{c.x,c.y+h},{c.x+w*0.2,c.y+h*0.9},{c.x+w*0.6,c.y+h*0.6}};
    add_poly(S,roof);
    // 台阶
    Poly base{{{c.x-w*0.5,c.y},{c.x+w*0.5,c.y}}}; add_poly(S,base);
}
static void add_pagoda(Scene& S, Pt c, double w){
    double tierH = w*0.35;
    for(int i=0;i<4;i++){
        double ww = w * (1.0 - 0.18*i);
        double y0 = c.y + tierH*i;
        Poly roof{{{c.x-ww,y0+tierH*0.6},{c.x-ww*0.2,y0+tierH*0.95},{c.x,y0+tierH},{c.x+ww*0.2,y0+tierH*0.95},{c.x+ww,y0+tierH*0.6}}};
        add_poly(S,roof);
        Poly colL{{{c.x-ww*0.4,y0},{c.x-ww*0.4,y0+tierH*0.6}}};
        Poly colR{{{c.x+ww*0.4,y0},{c.x+ww*0.4,y0+tierH*0.6}}};
        add_poly(S,colL); add_poly(S,colR);
    }
}
static void add_bridge(Scene& S, Pt a, Pt b, double arch=0.15){
    // 拱桥：简弧 + 桥身线
    int N=80; Poly arc;
    Pt mid{ (a.x+b.x)/2.0, (a.y+b.y)/2.0 + std::abs(b.x-a.x)*arch };
    for(int i=0;i<=N;i++){
        double t=(double)i/N;
        Pt p0=a, p1=mid, p2=b;
        double u=1-t;
        Pt p{ u*u*p0.x + 2*u*t*p1.x + t*t*p2.x,
              u*u*p0.y + 2*u*t*p1.y + t*t*p2.y };
        arc.pts.push_back(p);
    }
    add_poly(S,arc);
    Poly rail;
    rail.pts = { {a.x, a.y + 2}, {b.x, b.y + 2} };
    add_poly(S, rail);

}
static void add_waterfall(Scene& S, RNG& rng, Pt top, double h, double w){
    // 上缘
    Poly edge{{{top.x-w*0.5,top.y},{top.x+w*0.5,top.y}}}; add_poly(S,edge);
    // 多条竖向水丝
    int n = std::max(6,(int)std::round(w/4));
    for(int i=0;i<n;i++){
        double x = top.x - w*0.45 + (w*0.9) * (double)i/(n-1);
        Poly drop; int seg=40;
        for(int k=0;k<=seg;k++){
            double t=(double)k/seg;
            double y = top.y - h*t + std::sin(t*PI*2 + i)*1.2;
            drop.pts.push_back({x + std::sin(t*PI*1.5 + i)*0.8, y});
        }
        add_poly(S,drop);
    }
}
static void add_reed_clump(Scene& S, RNG& rng, Pt c, double r, int blades=12){
    for(int i=0;i<blades;i++){
        double ang = rng.uni(-PI/2, -PI/6);
        double len = rng.uni(r*0.8, r*1.4);
        Poly blade{{{c.x, c.y},{c.x+len*std::cos(ang), c.y+len*std::sin(ang)}}};
        add_poly(S,blade);
    }
}
static void add_lotus_clump(Scene& S, RNG& rng, Pt c, double r){
    // 叶片（圆/椭圆）
    int N=48; Poly leaf; leaf.closed=true;
    double rx=rng.uni(r*0.8,r*1.3), ry=rng.uni(r*0.6,r*1.0), ph=rng.uni(0,2*PI);
    for(int i=0;i<=N;i++){ double t=(double)i/N*2*PI;
        double rr = 1.0 + 0.08*std::sin(3*t+ph);
        leaf.pts.push_back({c.x+rr*rx*std::cos(t), c.y+rr*ry*std::sin(t)});
    }
    add_poly(S,leaf);
    // 花（五瓣简笔）
    Poly flower;
    for(int k=0;k<5;k++){
        double ang = k*2*PI/5.0;
        flower.pts.push_back({c.x, c.y});
        flower.pts.push_back({c.x + r*0.6*std::cos(ang), c.y + r*0.6*std::sin(ang)});
    }
    add_poly(S,flower);
}
static void add_rock_group(Scene& S, RNG& rng, Pt c, double r, int count=3){
    for(int i=0;i<count;i++){
        double ang=rng.uni(0,2*PI), d=rng.uni(0,r*0.8), rx=rng.uni(6,12), ry=rng.uni(8,16);
        Pt p{c.x+d*std::cos(ang), c.y+d*std::sin(ang)};
        Poly rock; rock.closed=true; int N=30;
        for(int k=0;k<=N;k++){ double t=(double)k/N*2*PI;
            double rr=1.0+0.2*std::sin(5*t+ang);
            rock.pts.push_back({p.x+rr*rx*std::cos(t), p.y+rr*ry*std::sin(t)});
        }
        add_poly(S,rock);
    }
}
static void add_house(Scene& S, Pt c, double w){
    double h=w*0.6;
    Poly base{{{c.x-w*0.6,c.y},{c.x+w*0.6,c.y}}}; add_poly(S,base);
    Poly wallL{{{c.x-w*0.5,c.y},{c.x-w*0.5,c.y+h}}}; add_poly(S,wallL);
    Poly wallR{{{c.x+w*0.5,c.y},{c.x+w*0.5,c.y+h}}}; add_poly(S,wallR);
    Poly roof{{{c.x-w*0.65,c.y+h},{c.x,c.y+h*1.25},{c.x+w*0.65,c.y+h}}}; add_poly(S,roof);
    Poly door{{{c.x- w*0.12,c.y},{c.x- w*0.12,c.y+h*0.5}}}; add_poly(S,door);
    Poly door2{{{c.x+ w*0.12,c.y},{c.x+ w*0.12,c.y+h*0.5}}}; add_poly(S,door2);
}
static void add_path(Scene& S, Pt a, Pt b, double wav=6.0){
    int N=120; Poly way;
    for(int i=0;i<=N;i++){
        double t=(double)i/N; double x=lerp(a.x,b.x,t); double y=lerp(a.y,b.y,t);
        y += std::sin(t*PI*3.0)*wav*0.2;
        way.pts.push_back({x,y});
    }
    add_poly(S,way);
}
static void add_sail(Scene& S, Pt c, double w){
    double h=w*1.2;
    Poly mast{{{c.x,c.y},{c.x,c.y+h}}}; add_poly(S,mast);
    Poly sailL{{{c.x,c.y+h},{c.x,c.y+h*0.2},{c.x-w*0.6,c.y+h*0.6},{c.x,c.y+h}}}; add_poly(S,sailL);
    Poly hull{{{c.x-w*0.6,c.y},{c.x+w*0.6,c.y}}}; add_poly(S,hull);
}
static void add_waves(Scene& S, RNG& rng, double y, double amp, int rows=3){
    for(int r=0;r<rows;r++){
        Poly wl; int N=120; double a=amp*(1.0-r*0.3); double yy = y + r*5;
        for(int i=0;i<=N;i++){
            double t=(double)i/N * 4*PI;
            wl.pts.push_back({ S.cv.xmin + (S.cv.width())*(double)i/N, yy + std::sin(t+r)*a });
        }
        add_poly(S,wl);
    }
}
static void add_mist_band(Scene& S, RNG& rng, double y, double h, int und=3){
    Poly band; int N=160;
    for(int i=0;i<=N;i++){
        double t=(double)i/N;
        double x = S.cv.xmin + S.cv.width()*t;
        double yy = y + std::sin(t*PI*und)*h*0.2;
        band.pts.push_back({x,yy});
    }
    add_poly(S,band);
}
static void add_rain(Scene& S, RNG& rng, int n, Pt a, Pt b){
    for(int i=0;i<n;i++){
        double x=rng.uni(a.x,b.x), y=rng.uni(a.y,b.y), len=rng.uni(6,16), ang=-PI/2 + rng.uni(-PI/16,PI/16);
        Poly drop{{{x,y},{x+len*std::cos(ang), y+len*std::sin(ang)}}}; add_poly(S,drop);
    }
}
static void add_stars(Scene& S, RNG& rng, int n, Pt a, Pt b){
    for(int i=0;i<n;i++){
        double x=rng.uni(a.x,b.x), y=rng.uni(a.y,b.y);
        Poly p{{{x-0.8,y},{x+0.8,y}}}; add_poly(S,p);
        Poly q{{{x,y-0.8},{x,y+0.8}}}; add_poly(S,q);
    }
}

// ---------------- 文本 → 意象标签 ----------------
struct Flags {
    bool mountain=false, river=false, cloud=false, pine=false;
    bool bamboo=false, boat=false, birds=false, moon=false, sun=false, snow=false;
    // 新增
    bool pavilion=false, pagoda=false, bridge=false, waterfall=false;
    bool reeds=false, lotus=false, rocks=false, village=false, path=false;
    bool sail=false, waves=false, mist=false, rain=false, stars=false;
};
static Flags parse_flags(const std::wstring& ws){
    auto has_any = [&](const wchar_t* chars)->bool{
        for(size_t i=0; chars[i]; ++i) if (ws.find(chars[i]) != std::wstring::npos) return true; return false;
    };
    auto has_phrase = [&](const wchar_t* p)->bool{ return ws.find(p) != std::wstring::npos; };

    Flags f;
    if (has_any(L"山峰岳岭巅岭峰峦山")) f.mountain = true;
    if (has_any(L"水江河溪湖海湾潮浪渚汀渡泉瀑")) f.river = true;
    if (has_any(L"云雾霭霞岚")) f.cloud = true;
    if (has_any(L"松杉柏")) f.pine = true;
    if (has_any(L"竹篁笋")) f.bamboo = true;
    if (has_any(L"舟船艇渡")) f.boat = true;
    if (has_any(L"鸟雁鸿鹤鸢鹭")) f.birds = true;
    if (has_any(L"月明月亮")) f.moon = true;
    if (has_any(L"日阳太阳晨夕白日")) f.sun = true;
    if (has_any(L"雪霜寒冰")) f.snow = true;

    // 新增
    if (has_any(L"亭亭台榭亭子")) f.pavilion = true;
    if (has_any(L"塔宝塔")) f.pagoda = true;
    if (has_any(L"桥梁桥")) f.bridge = true;
    if (has_any(L"瀑布飞瀑泉")) f.waterfall = true;
    if (has_any(L"芦苇蒹葭苇")) f.reeds = true;
    if (has_any(L"荷莲塘荷塘莲花")) f.lotus = true;
    if (has_any(L"石岩礁怪石")) f.rocks = true;
    if (has_any(L"村舍屋庄")) f.village = true;
    if (has_any(L"径路小径山路")) f.path = true;
    if (has_any(L"帆风帆船篷")) f.sail = true;
    if (has_any(L"浪涛海")) f.waves = true;
    if (has_any(L"雾霭薄雾岚")) f.mist = true;
    if (has_any(L"雨霖落雨细雨")) f.rain = true;
    if (has_any(L"星辰星斗繁星")) f.stars = true;

    if (has_phrase(L"独钓寒江雪")) { f.river=true; f.boat=true; f.snow=true; f.mountain=true; f.cloud=true; f.birds=false; f.moon=false; }
    if (has_phrase(L"千山鸟飞绝")) { f.mountain=true; f.birds=false; f.snow=true; }
    if (has_phrase(L"万径人踪灭")) { f.mountain=true; f.snow=true; f.boat=false; }
    return f;
}

// ---------------- 序列化 ----------------
static json to_json(const Scene& S){
    json j;
    j["units"]="mm"; j["scale"]=1.0; j["polylines"]=json::array();
    for (auto& pl : S.polys) {
        json arr=json::array();
        for (auto& p : pl.pts) arr.push_back({p.x,p.y});
        j["polylines"].push_back({ {"closed", pl.closed}, {"points", arr} });
    }
    return j;
}

// FNV-1a 64
static uint64_t hash64(const std::wstring& ws){
    const uint64_t FNV_OFFSET=1469598103934665603ull, FNV_PRIME=1099511628211ull;
    uint64_t h=FNV_OFFSET; for(wchar_t wc: ws){ uint32_t c=(uint32_t)wc;
        h^=(unsigned char)(c&0xFF); h*=FNV_PRIME;
        h^=(unsigned char)((c>>8)&0xFF); h*=FNV_PRIME;
    } return h;
}

// UTF-8 -> wstring
static bool utf8_to_wstring(const std::string& utf8, std::wstring& out){
#ifdef _WIN32
    if (utf8.empty()) { out.clear(); return true; }
    int wlen=MultiByteToWideChar(CP_UTF8,0,utf8.c_str(),(int)utf8.size(),nullptr,0);
    if (wlen<=0) return false; out.resize(wlen);
    int ok=MultiByteToWideChar(CP_UTF8,0,utf8.c_str(),(int)utf8.size(),&out[0],wlen); return ok>0;
#else
    out.clear(); for(unsigned char c: utf8) out.push_back((wchar_t)c); return true;
#endif
}

// ---------------- 主生成 ----------------
static Scene build_scene_from_text(const std::wstring& text_w, const GenConfig& cfg){
    Scene S; S.cv = cfg.canvas;
    RNG rng(cfg.seed==0? hash64(text_w) : cfg.seed);
    Flags F = parse_flags(text_w);

    if (cfg.default_mountain_if_none && !F.mountain && !F.river) F.mountain = true;
    if (cfg.default_cloud && !F.cloud) F.cloud = true;

    // 山
    if (F.mountain){
        double ym = rng.uni(S.cv.ymin+S.cv.height()*0.15, S.cv.ymax- S.cv.height()*0.25);
        double amp = S.cv.height()*rng.uni(cfg.mountain_amp_min_ratio, cfg.mountain_amp_max_ratio);
        int layers = cfg.mountain_layers_min + rng.randint(0, std::max(0, cfg.mountain_layers_max - cfg.mountain_layers_min));
        add_mountain_range(S, rng, ym, amp, layers);
    }

    // 云
    if (F.cloud && cfg.default_cloud){
        int n = cfg.cloud_count_min + rng.randint(0, std::max(0, cfg.cloud_count_max - cfg.cloud_count_min));
        for(int i=0;i<n;i++){
            Pt c{ rng.uni(S.cv.xmin+20, S.cv.xmax-20), rng.uni(S.cv.ymax-45, S.cv.ymax-15) };
            add_cloud(S, rng, c, rng.uni(18,32), rng.uni(8,14), 2 + rng.randint(0,1));
        }
    }

    // 水体（若有舟且允许“舟=>水”）
    if (F.river || (F.boat && cfg.auto_water_if_boat)){
        Pt p0{ S.cv.xmin+rng.uni(10,40), rng.uni(S.cv.ymin+20, S.cv.ymin+60) };
        Pt p2{ S.cv.xmax-rng.uni(10,40), rng.uni(S.cv.ymin+5,  S.cv.ymin+70) };
        Pt p1{ (p0.x+p2.x)/2 + rng.uni(-30,30), (p0.y+p2.y)/2 + rng.uni(15,45) };
        add_river(S, rng, p0,p1,p2, rng.uni(4,8));
    }

    // 树木/竹
    if (F.pine) { for(int i=0, k=rng.randint(3,6); i<k; ++i) add_pine(S, rng, {rng.uni(S.cv.xmin+10,S.cv.xmax-10), rng.uni(S.cv.ymin+5,S.cv.ymin+40)}, rng.uni(25,40), rng.randint(4,6)); }
    if (F.bamboo){ for(int i=0, k=rng.randint(3,6); i<k; ++i) add_bamboo(S, rng, {rng.uni(S.cv.xmin+15,S.cv.xmax-15), rng.uni(S.cv.ymin+5,S.cv.ymin+35)}, rng.uni(28,42), rng.randint(5,7)); }

    // 亭、塔、桥、瀑
    if (F.pavilion && cfg.enable_pavilion) add_pavilion(S, {rng.uni(S.cv.xmin+30,S.cv.xmax-30), rng.uni(S.cv.ymin+35,S.cv.ymin+75)}, 40);
    if (F.pagoda   && cfg.enable_pagoda)   add_pagoda  (S, {rng.uni(S.cv.xmin+40,S.cv.xmax-40), rng.uni(S.cv.ymin+25,S.cv.ymin+65)}, 40);
    if (F.bridge   && cfg.enable_bridge)   add_bridge  (S, {rng.uni(S.cv.xmin+20,S.cv.xmax-60), rng.uni(S.cv.ymin+30,S.cv.ymin+60)},
                                                           {rng.uni(S.cv.xmin+60,S.cv.xmax-20), rng.uni(S.cv.ymin+30,S.cv.ymin+60)}, 0.18);
    if (F.waterfall&& cfg.enable_waterfall)add_waterfall(S, rng, {rng.uni(S.cv.xmin+30,S.cv.xmax-30), rng.uni(S.cv.ymin+70,S.cv.ymax-10)}, rng.uni(30,60), rng.uni(20,40));

    // 芦苇、荷塘、怪石、村舍、小径
    if (F.reeds && cfg.enable_reeds){
        for(int i=0;i<cfg.reeds_clumps;i++){
            add_reed_clump(S, rng, {rng.uni(S.cv.xmin+10,S.cv.xmax-10), rng.uni(S.cv.ymin+5,S.cv.ymin+55)}, rng.uni(10,18), rng.randint(10,16));
        }
    }
    if (F.lotus && cfg.enable_lotus){
        for(int i=0;i<cfg.lotus_clumps;i++){
            add_lotus_clump(S, rng, {rng.uni(S.cv.xmin+15,S.cv.xmax-15), rng.uni(S.cv.ymin+10,S.cv.ymin+60)}, rng.uni(10,16));
        }
    }
    if (F.rocks && cfg.enable_rocks){
        for(int i=0;i<cfg.rock_groups;i++){
            add_rock_group(S, rng, {rng.uni(S.cv.xmin+20,S.cv.xmax-20), rng.uni(S.cv.ymin+20,S.cv.ymin+70)}, rng.uni(10,18), rng.randint(2,4));
        }
    }
    if (F.village && cfg.enable_village){
        double y = rng.uni(S.cv.ymin+35,S.cv.ymin+75);
        for(int i=0;i<cfg.village_houses;i++){
            add_house(S, {rng.uni(S.cv.xmin+20,S.cv.xmax-20), y + rng.uni(-4,4)}, rng.uni(18,26));
        }
    }
    if (F.path && cfg.enable_path){
        add_path(S, {rng.uni(S.cv.xmin+10,S.cv.xmax-60), rng.uni(S.cv.ymin+20,S.cv.ymin+55)},
                    {rng.uni(S.cv.xmin+60,S.cv.xmax-10), rng.uni(S.cv.ymin+20,S.cv.ymin+55)}, 6.0);
    }

    // 舟/帆、海浪
    if (F.boat) add_boat(S, rng, {rng.uni(S.cv.xmin+35,S.cv.xmax-35), rng.uni(S.cv.ymin+10,S.cv.ymin+55)}, rng.uni(30,40));
    if (F.sail && cfg.enable_sail) add_sail(S, {rng.uni(S.cv.xmin+30,S.cv.xmax-30), rng.uni(S.cv.ymin+10,S.cv.ymin+55)}, rng.uni(18,26));
    if (F.waves && cfg.enable_waves) add_waves(S, rng, rng.uni(S.cv.ymin+10,S.cv.ymin+45), rng.uni(2,6), rng.randint(2,4));

    // 鸟、日/月、雪、雾、雨、星
    if (F.birds && cfg.enable_birds) add_birds(S, rng, {S.cv.xmin+20,S.cv.ymax-60}, {S.cv.xmax-20,S.cv.ymax-15}, cfg.birds_min + rng.randint(0,std::max(0,cfg.birds_max-cfg.birds_min)));
    if (F.moon && !F.sun) add_disc(S, {S.cv.xmax-35,S.cv.ymax-35}, cfg.moon_radius);
    else if (F.sun && !F.moon) add_disc(S, {S.cv.xmax-35,S.cv.ymax-35}, cfg.sun_radius);
    else { if (rng.uni()<0.3) add_disc(S, {S.cv.xmax-35,S.cv.ymax-35}, cfg.moon_radius);
           else add_disc(S, {S.cv.xmax-35,S.cv.ymax-35}, cfg.sun_radius); }
    if (F.snow && cfg.enable_snow) add_snow(S, rng, cfg.snow_base + rng.randint(0,std::max(0,cfg.snow_rand)));
    if (F.mist && cfg.enable_mist){ for(int i=0;i<cfg.mist_bands;i++) add_mist_band(S, rng, rng.uni(S.cv.ymin+50,S.cv.ymax-10), rng.uni(8,16), rng.randint(2,4)); }
    if (F.rain && cfg.enable_rain) add_rain(S, rng, 140, {S.cv.xmin+10,S.cv.ymin+60},{S.cv.xmax-10,S.cv.ymax-10});
    if (F.stars && cfg.enable_stars) add_stars(S, rng, cfg.star_count, {S.cv.xmin+10,S.cv.ymax-50},{S.cv.xmax-10,S.cv.ymax-10});

    return S;
}

// ---------------- 对外接口 ----------------
inline bool generate_shanshui_json_ex(const std::wstring& text, const std::string& outpath, const GenConfig* cfgOpt=nullptr){
    if (text.empty()) return false;
    GenConfig cfg; if (cfgOpt) cfg = *cfgOpt;
    Scene S = build_scene_from_text(text, cfg);
    json j = to_json(S);
    std::ofstream ofs(outpath);
    if(!ofs) return false; ofs<<j.dump(2); return true;
}
inline bool generate_shanshui_json_ex_utf8(const std::string& text_utf8, const std::string& outpath, const GenConfig* cfgOpt=nullptr){
    std::wstring ws; if (!utf8_to_wstring(text_utf8, ws)) return false;
    return generate_shanshui_json_ex(ws, outpath, cfgOpt);
}
// 旧接口兼容
inline bool generate_shanshui_json(const std::wstring& text, const std::string& outpath){
    return generate_shanshui_json_ex(text, outpath, nullptr);
}
inline bool generate_shanshui_json_utf8(const std::string& text_utf8, const std::string& outpath){
    return generate_shanshui_json_ex_utf8(text_utf8, outpath, nullptr);
}

} // namespace shanshui

// 便捷别名
using shanshui::GenConfig;
inline bool generate_shanshui_json_ex(const std::wstring& text, const std::string& outpath, const shanshui::GenConfig* cfgOpt){ return shanshui::generate_shanshui_json_ex(text, outpath, cfgOpt); }
inline bool generate_shanshui_json_ex_utf8(const std::string& text_utf8, const std::string& outpath, const shanshui::GenConfig* cfgOpt){ return shanshui::generate_shanshui_json_ex_utf8(text_utf8, outpath, cfgOpt); }
inline bool generate_shanshui_json(const std::wstring& text, const std::string& outpath){ return shanshui::generate_shanshui_json(text, outpath); }
inline bool generate_shanshui_json_utf8(const std::string& text_utf8, const std::string& outpath){ return shanshui::generate_shanshui_json_utf8(text_utf8, outpath); }
