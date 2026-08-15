// SPDX-License-Identifier: GPL-3.0-or-later

#include "meshprofiles.h"
#include "curveutils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace baclickfx {

namespace {

// JavaScript Math.round 采用 floor(x + 0.5)。
double jsRound(double v)
{
    return std::floor(v + 0.5);
}

// 去除字符串两端空白，包括 CRLF 中的回车字符。
std::string trimmed(const std::string &s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        e--;
    return s.substr(b, e - b);
}

// 按连续空白切分已去除首尾空白的字符串。
std::vector<std::string> splitWhitespace(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string token;
    while (ss >> token)
        out.push_back(token);
    return out;
}

// 按前缀解析浮点数；无有效数字时返回 NaN。
double parseFloatLoose(const std::string &s)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin)
        return std::numeric_limits<double>::quiet_NaN();
    return v;
}

// 解析 OBJ 面元素的指定索引段（0=v，1=vt，2=vn），并转换为 0 基索引。
// 支持负数相对索引；字段缺失、值为 0 或解析失败时返回 -1。
int parseObjIndex(const std::string &token, std::size_t count, int field = 0)
{
    // 按 '/' 提取指定字段；"3//3" 的空中间段表示缺少 vt。
    std::size_t begin_pos = 0;
    for (int i = 0; i < field; i++) {
        const std::size_t slash = token.find('/', begin_pos);
        if (slash == std::string::npos)
            return -1;  // 段数不够，没有这一维
        begin_pos = slash + 1;
    }
    const std::size_t next = token.find('/', begin_pos);
    const std::string head = token.substr(begin_pos, next == std::string::npos
                                                        ? std::string::npos
                                                        : next - begin_pos);
    if (head.empty())
        return -1;  // "3//3" 的中间段：无 vt

    const char *begin = head.c_str();
    char *end = nullptr;
    const long raw = std::strtol(begin, &end, 10);
    if (end == begin || raw == 0)
        return -1;
    if (raw > 0)
        return static_cast<int>(raw - 1);
    return static_cast<int>(static_cast<long>(count) + raw);
}

// OBJ 解析结果。
struct ObjData {
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    // 保留 OBJ 的逐角纹理坐标，避免 UV 接缝被顶点级近似破坏。
    std::vector<Vertex> uvs;
    // f 行的 vt 索引，与 faces 一一对应；OBJ 里 v 和 vt 的索引可以不同，
    // 不能拿 faces 直接当 uv 索引用。
    std::vector<Face> uvFaces;
    bool opened = false;
};

// 读取 OBJ 的 v、vt 和 f 记录，仅保留顶点 xy 分量并将多边形三角化。
ObjData parseObj(const std::string &path)
{
    ObjData data;
    std::ifstream in(path);
    if (!in)
        return data;
    data.opened = true;

    std::string line;
    while (std::getline(in, line)) {
        // 先处理 vt，使顶点与纹理坐标分支保持明确且互不依赖。
        if (line.rfind("vt ", 0) == 0) {
            const std::vector<std::string> parts = splitWhitespace(trimmed(line));
            if (parts.size() < 3)
                continue;

            const double u = parseFloatLoose(parts[1]);
            const double v = parseFloatLoose(parts[2]);
            if (std::isnan(u) || std::isnan(v))
                continue;

            data.uvs.push_back(Vertex{u, v});
            continue;
        }

        // 注意 "vt "/"vn " 不以 "v " 开头，天然被排除。
        if (line.rfind("v ", 0) == 0) {
            const std::vector<std::string> parts = splitWhitespace(trimmed(line));
            if (parts.size() < 3)
                continue;

            const double x = parseFloatLoose(parts[1]);
            const double y = parseFloatLoose(parts[2]);
            if (std::isnan(x) || std::isnan(y))
                continue;

            data.vertices.push_back(Vertex{x, y});
            continue;
        }

        if (line.rfind("f ", 0) != 0)
            continue;

        const std::vector<std::string> parts = splitWhitespace(trimmed(line));
        if (parts.size() < 4)
            continue;

        std::vector<int> idx;
        idx.reserve(parts.size() - 1);
        for (std::size_t i = 1; i < parts.size(); i++)
            idx.push_back(parseObjIndex(parts[i], data.vertices.size()));

        const bool bad = std::any_of(idx.begin(), idx.end(), [&](int v) {
            return v < 0 || static_cast<std::size_t>(v) >= data.vertices.size();
        });
        if (bad)
            continue;

        // 同一行再取一遍 vt 索引。整行的 vt 必须齐全才算有效，
        // 少一个就整面弃用 UV（宁可回退到线性 UV，也不要错位的贴图）。
        std::vector<int> uvIdx;
        uvIdx.reserve(parts.size() - 1);
        bool uvOk = !data.uvs.empty();
        for (std::size_t i = 1; i < parts.size() && uvOk; i++) {
            const int t = parseObjIndex(parts[i], data.uvs.size(), 1);
            if (t < 0 || static_cast<std::size_t>(t) >= data.uvs.size())
                uvOk = false;
            else
                uvIdx.push_back(t);
        }

        // 将 n 边形面拆成三角扇，便于统一绘制。
        for (std::size_t i = 1; i + 1 < idx.size(); i++) {
            data.faces.push_back(Face{idx[0], idx[i], idx[i + 1]});
            // uvFaces 与 faces 保持等长：这一面没有 vt 就填 -1，
            // 渲染侧据此决定回退。
            if (uvOk)
                data.uvFaces.push_back(Face{uvIdx[0], uvIdx[i], uvIdx[i + 1]});
            else
                data.uvFaces.push_back(Face{-1, -1, -1});
        }
    }

    return data;
}

} // namespace

std::optional<CylinderProfile> loadCylinderObjProfile(const std::string &path)
{
    // 圆环需要按角度聚合内外半径，因此在通用解析之外多走一遍顶点。
    const ObjData data = parseObj(path);
    if (!data.opened)
        return std::nullopt;

    struct Ring {
        double inner = 0.0;
        double outer = 0.0;
    };

    // 键是量化后的角度。std::map 按键升序遍历，与 JS 侧 sort((a,b)=>a.angle-b.angle)
    // 的遍历顺序一致——求均值时的累加顺序会影响浮点结果，必须对齐。
    std::map<double, Ring> angleMap;
    for (const Vertex &v : data.vertices) {
        const double r = std::hypot(v[0], v[1]);
        const double a = normalizeAngle(std::atan2(v[1], v[0]));
        // 将角度量化后聚合同一扇区，统计该角度上的内外半径。
        const double key = jsRound(a * 100000) / 100000;

        auto it = angleMap.find(key);
        if (it != angleMap.end()) {
            it->second.inner = std::min(it->second.inner, r);
            it->second.outer = std::max(it->second.outer, r);
        } else {
            angleMap.emplace(key, Ring{r, r});
        }
    }

    if (angleMap.size() < 16)
        return std::nullopt;

    double innerSum = 0.0;
    double outerSum = 0.0;
    for (const auto &[angle, ring] : angleMap) {
        (void)angle;
        innerSum += ring.inner;
        outerSum += ring.outer;
    }

    CylinderProfile profile;
    profile.innerRadiusNorm = innerSum / angleMap.size();
    profile.outerRadiusNorm = outerSum / angleMap.size();
    profile.segmentCount = static_cast<int>(angleMap.size());
    profile.mesh.vertices = data.vertices;
    profile.mesh.faces = data.faces;
    profile.mesh.uvs = data.uvs;
    profile.mesh.uvFaces = data.uvFaces;

    // 圆环的几何是按角度程序化重建的，不直接画 OBJ 面，所以 UV 只能取范围来用。
    // Cylinder002 的 vt 在两轴上都落在 [0.0005, 0.99950004]（导出时的固定内缩），
    // 这正是 ba-click-fx 里 textureUvMin / textureUvMax 那两个数的来源。
    if (!data.uvs.empty()) {
        double lo = data.uvs.front()[0];
        double hi = lo;
        for (const Vertex &uv : data.uvs) {
            lo = std::min({lo, uv[0], uv[1]});
            hi = std::max({hi, uv[0], uv[1]});
        }
        // 退化的 UV（全 0 或反了）不覆盖默认值，免得把贴图压成一个点。
        if (hi - lo > 1e-6) {
            profile.uvMin = lo;
            profile.uvMax = hi;
        }
    }
    return profile;
}

MeshProfiles loadMeshProfiles(const std::string &cylinderPath)
{
    MeshProfiles profiles;

    if (const auto cylinder = loadCylinderObjProfile(cylinderPath))
        profiles.cylinder002 = *cylinder;

    return profiles;
}

} // namespace baclickfx
