# 源码结构

| 目录 | 职责 |
| --- | --- |
| [src/effect](../src/effect) | KWin 生命周期、特效状态、诊断及 6.6/6.7 接口适配 |
| [src/input](../src/input) | 鼠标按键、应用过滤、呈现帧时钟 |
| [src/particles](../src/particles) | 点击与拖动实例、轨迹、曲线及生成的参数表 |
| [src/render](../src/render) | GPU 合成、Bloom、重绘区域、网格、纹理及输出缩放 |
| [src/unity](../src/unity) | 与 FX_Touch 对照的数值内核 |
| [config](../config) · [po](../po) | 配置模块、界面与翻译 |
| [shader](../shader) · [assets](../assets) | 着色器、粒子贴图和网格 |
| [scripts](../scripts) · [tests](../tests) | 参数生成、测试、验证及源码打包 |

[插件入口](../src/main.cpp)、[插件元数据](../src/metadata.json)和[共享配置键与默认值](../src/baclickfxdefaults.h)位于 `src` 顶层。

每次光标所在输出呈现新帧时，特效采样光标、推进粒子并计算重绘区域。渲染器将受影响的桌面像素导入线性 RGBA16F 目标，先画拖尾、圆盘和 MeshTri，再构建 Bloom 金字塔。Ring3 和 Ring4 在 Bloom 采样之后叠加，合成结果按 KWin 输出色彩描述编码。HDR 缓冲和 Bloom 金字塔按输出复用。

KWin 6.8 是主要开发目标。[兼容层](../src/effect/kwincompat.cpp)处理 6.6 的呈现时间戳与指针回退，以及 6.6/6.7 的绘制返回类型。[FX_Touch 核对记录](unity-parity.md)说明渲染行为和原版的对应关系。
