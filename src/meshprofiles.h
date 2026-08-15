// SPDX-License-Identifier: GPL-3.0-or-later
// Cylinder002 OBJ 网格及逐角 UV 数据。

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace baclickfx {

using Vertex = std::array<double, 2>;
using Face = std::array<int, 3>;

/// 带逐角纹理坐标索引的三角网格。
struct MeshProfile {
    std::vector<Vertex> vertices;
    std::vector<Face> faces;

    // `uvFaces` 与 `faces` 等长，保存 `uvs` 的索引；缺少 vt 的面使用 {-1,-1,-1}。
    // MeshTri 必须使用 OBJ 的逐角 UV，以保留接缝和面片插值。
    std::vector<Vertex> uvs;
    std::vector<Face> uvFaces;

    bool valid() const { return !vertices.empty() && !faces.empty(); }

    /// 返回指定面角的纹理坐标；索引无效或缺少 vt 时返回空值。
    std::optional<Vertex> faceUv(std::size_t face, int corner) const
    {
        if (face >= uvFaces.size() || corner < 0 || corner > 2)
            return std::nullopt;
        const int i = uvFaces[face][static_cast<std::size_t>(corner)];
        if (i < 0 || static_cast<std::size_t>(i) >= uvs.size())
            return std::nullopt;
        return uvs[static_cast<std::size_t>(i)];
    }
};

/// Cylinder002 网格及其归一化圆环参数。
struct CylinderProfile {
    double innerRadiusNorm = 1.0;
    double outerRadiusNorm = 1.0636685;
    int segmentCount = 64;

    // OBJ 中的 vt 落在 [0.0005, 0.99950004]。保留该范围可避免 CLAMP_TO_EDGE
    // 在渐变贴图两端产生额外采样，确保溶解边缘与原网格一致。
    double uvMin = 0.0005;
    double uvMax = 0.999500036239624;

    MeshProfile mesh;
};

/// 渲染器使用的网格资源集合。
struct MeshProfiles {
    CylinderProfile cylinder002;
};

/// 解析 Cylinder002 OBJ；文件无效或分段数不足时返回空值。
std::optional<CylinderProfile> loadCylinderObjProfile(const std::string &path);

/// 加载渲染器所需的全部网格资源。
MeshProfiles loadMeshProfiles(const std::string &cylinderPath);

} // namespace baclickfx
