// SPDX-License-Identifier: GPL-3.0-or-later
//
// 背景导入与最终合成共用的传递函数。Unity UI HDR RenderTexture 使用线性颜色，
// KWin 输出目标保存传递函数编码值，因此导入时解码，写回时执行严格对应的编码。
// 线性结果以参考白为 1.0；绝对亮度仅在本文件内部以尼特为单位参与计算。

uniform int u_tfType;
uniform vec3 u_tfLum;    // 最小亮度、最大亮度和参考白亮度，单位为尼特。
uniform vec2 u_tfBt1886; // BT.1886 的 a 和 b。

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
    // 将绝对亮度归一化到 Unity 使用的相对线性范围。
    return vec3(tfEotf(e.r), tfEotf(e.g), tfEotf(e.b)) / max(u_tfLum.z, 1e-6);
}

vec3 tfEncode(vec3 relative)
{
    // 恢复绝对亮度后按当前输出的传递函数编码。
    vec3 n = relative * u_tfLum.z;
    return vec3(tfInverseEotf(n.r), tfInverseEotf(n.g), tfInverseEotf(n.b));
}
