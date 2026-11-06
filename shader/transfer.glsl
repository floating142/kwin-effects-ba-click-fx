// SPDX-License-Identifier: GPL-3.0-or-later
//
// 背景导入与最终合成共用的传递函数。Unity UI HDR RenderTexture 使用线性颜色，
// KWin 输出目标保存传递函数编码值，因此导入时解码，写回时执行严格对应的编码。
// 线性结果以参考白为 1.0；绝对亮度仅在本文件内部以尼特为单位参与计算。

uniform int u_tfType;
uniform vec3 u_tfLum;    // 最小亮度、最大亮度和参考白亮度，单位为尼特。
uniform vec2 u_tfBt1886; // BT.1886 的 a 和 b。
uniform vec3 u_tfHlg;    // HLG 的 gamma、alpha 和黑电平 beta。
// 导入时为输出色域 → 线性 BT.709，合成时为逆向。矩阵不包含亮度归一化。
uniform mat4 u_colorMatrix;

float tfEotf(float e)
{
    // 类型值对应 KWin::TransferFunction::Type。
    if (u_tfType == 1) {
        return e * (u_tfLum.y - u_tfLum.x) + u_tfLum.x;
    } else if (u_tfType == 0) {
        float l = e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4);
        return l * (u_tfLum.y - u_tfLum.x) + u_tfLum.x;
    } else if (u_tfType == 3) {
        float l = pow(max(e, 0.0), 2.2);
        return l * (u_tfLum.y - u_tfLum.x) + u_tfLum.x;
    } else if (u_tfType == 4) {
        return u_tfBt1886.x * pow(max(e + u_tfBt1886.y, 0.0), 2.4);
    }

    // PQ（SMPTE ST 2084）。
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float ep = pow(max(e, 0.0), 1.0 / m2);
    return 10000.0 * pow(max(ep - c1, 0.0) / max(c2 - c3 * ep, 1e-6), 1.0 / m1);
}

float tfInverseEotf(float nits)
{
    float span = max(u_tfLum.y - u_tfLum.x, 1e-6);
    if (u_tfType == 1) {
        return clamp((nits - u_tfLum.x) / span, 0.0, 1.0);
    } else if (u_tfType == 0) {
        float l = clamp((nits - u_tfLum.x) / span, 0.0, 1.0);
        return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
    } else if (u_tfType == 3) {
        return pow(clamp((nits - u_tfLum.x) / span, 0.0, 1.0), 1.0 / 2.2);
    } else if (u_tfType == 4) {
        float e = pow(max(nits, 0.0) / max(u_tfBt1886.x, 1e-6), 1.0 / 2.4) - u_tfBt1886.y;
        return clamp(e, 0.0, 1.0);
    }

    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

vec3 tfDecode(vec3 e)
{
    if (u_tfType == 5) {
        // BT.2100 HLG：反 OETF 后，以 BT.2020 亮度对三个通道共同应用 OOTF。
        vec3 adjusted = max(vec3(0.0), (1.0 - u_tfHlg.z) * e + u_tfHlg.z);
        vec3 lo = adjusted * adjusted / 3.0;
        vec3 hi = (exp((adjusted - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
        vec3 nits = u_tfLum.y * mix(hi, lo, lessThanEqual(adjusted, vec3(0.5)));
        float y = dot(vec3(0.2627, 0.6780, 0.0593), nits);
        if (y <= 0.0) {
            return vec3(0.0);
        }
        return nits * (u_tfHlg.y * pow(y, u_tfHlg.x - 1.0) / max(u_tfLum.z, 1e-6));
    }

    // 将绝对亮度归一化到 Unity 使用的相对线性范围。
    return vec3(tfEotf(e.r), tfEotf(e.g), tfEotf(e.b)) / max(u_tfLum.z, 1e-6);
}

vec3 tfEncode(vec3 relative)
{
    // 恢复绝对亮度后按当前输出的传递函数编码。
    vec3 n = relative * u_tfLum.z;
    if (u_tfType == 5) {
        float y = dot(vec3(0.2627, 0.6780, 0.0593), n);
        float inverseOotf = pow(max(y / u_tfHlg.y, 0.0001), 1.0 / u_tfHlg.x - 1.0);
        vec3 scene = clamp(n * (inverseOotf / (u_tfHlg.y * u_tfLum.y)), 0.0, 1.0);
        vec3 lo = sqrt(3.0 * scene);
        vec3 hi = 0.17883277 * log(max(12.0 * scene - 0.28466892, vec3(1e-6))) + 0.55991073;
        vec3 encoded = mix(hi, lo, lessThanEqual(scene, vec3(1.0 / 12.0)));
        return (encoded - u_tfHlg.z) / (1.0 - u_tfHlg.z);
    }
    return vec3(tfInverseEotf(n.r), tfInverseEotf(n.g), tfInverseEotf(n.b));
}
