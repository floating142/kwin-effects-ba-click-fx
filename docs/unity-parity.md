# FX_Touch 原版核对

本轮基准为 PC 原游戏中的 `FX_Touch`。
源参数、纹理像素哈希和网格数据保存在
[`tests/fixtures/unity-touch.json`](../tests/fixtures/unity-touch.json)。

## 来源与生成

| 来源 | SHA-256 |
| --- | --- |
| 游戏资源表 | `1134d2665b11be54a6a9d80749b2dae52dacc3d035118f81619d9d14bfccc0cf` |
| 游戏纹理流 | `7b54cd45ff30c472e592296e6f7393075df837c787cf96a0a6c9f03fd55cf2b7` |
| Unity 原生运行库 | `3f696bcf12b9bc6a4efe26193bdc7f209aee48d9a717e415c773cc5de155b5b3` |

`scripts/import-unity-touch.py` 沿实际 GameObject / Transform / Component 引用读取
Prefab。`src/particles/curvedata.h` 和 `src/particles/touchparams.h` 从源快照生成；有效关键帧数量、原始
uint16 渐变时间、非加权曲线切线、初始参数端点、材质颜色与队列均保留源值。
发现未支持的模式或新启用模块时生成器报错。根 ParticleSystem 已确认关闭发射，
不是遗漏的第五层粒子。沿 `TouchEffectCreater.Camera` 引用确认原始场景中的 Camera 143
为正交 UICamera、orthographicSize=1；场景哈希和相机字段也记录在快照中。
原版 `UIRenderPass.Execute`（`0x6c60fd6` 起）及 `GetProjectionMatrixByAspect`
（`0x6c62650`）给出普通投影的横向范围 ±宽高比和纵向范围 ±1，近远面为
0.1 和 20。启用游戏的上下黑边且宽高比低于门限时，纵向半高会扩大到
门限宽高比 / 实际宽高比。KWin 使用普通视口公式，光标跟随桌面输出。
相邻输出的边界按光标所在的逻辑像素归属，尺寸、显示器倍率、帧时钟和重绘使用
同一个坐标换算。含负坐标的输出接缝也以像素左上角为界。
原 FXTouch 的 Trail 引用和 FxTrailTimeScale 阈值从实际组件读取，不再手写阈值。

```bash
# 无需游戏文件或第三方 Python 库；检查已提交的数据表。
python3 scripts/import-unity-touch.py --check
```

仓库中四张 PNG 的尺寸和解码后 RGBA 像素与 PC 原资源完全一致。
`Cylinder002.obj` 的顶点、三角形和逐角 UV 也与原 Mesh 对照；投影时显式还原 OBJ
导出的 X 镜像，并把 Unity 的 Y 向上转换为屏幕的 Y 向下。
导入时从各 Renderer 的首个材质槽解析实际材质与贴图引用，
从 MeshTri 的 `m_Mesh` 引用读取单子网格 Cylinder002。
Ring 和 MeshTri 的第二个材质槽由原生 `get_trailMaterial`
（`0x1c9e22..0x1c9e37`）读取，两个系统的 ParticleSystem Trails 模块均关闭。

同时直接检查 PC Texture2D 的采样状态：四张贴图均为 FilterMode=1、MipCount=1、
Aniso=1、MipBias=0；Circle / Trail 为 Repeat，Grad_Ring3 / Triangle 为 Clamp。
材质的全部纹理变换均为 Scale=(1,1)、Offset=(0,0)，未启用 CustomData UV 偏移。
这些字段已加入原资源快照及导入检查，避免只验证 PNG 像素而遗漏采样状态。

原版 PC 的 `UIRenderBloomSettings` 数值已记录在参数快照中。
Intensity=1.7000000476837158、Threshold=1、SoftKnee=0、Clamp=65472、Diffusion=7、
AnamorphicRatio=0、Color=(1,1,1,1)，Shader 指向 `Hidden/MXFinalBloom`（62）。
原版反汇编中的 `UIRenderPass.RenderBloom`（0x6c629f0）确认：半尺寸
金字塔、log2 / Diffusion 级数、GammaToLinear 阈值与 Clamp、1e-5 knee，以及
`exp(Intensity / 10 * ln(2)) - 1` 的强度换算与当前路径相符。

## 实现对照

| 路径 | 原差异 | 当前行为 |
| --- | --- | --- |
| 粒子颜色 | 浮点渐变直接相乘，两个输入分别解码 | uint16 时间、RGBA8 缓存插值、整数乘色，最后转换线性 RGB |
| 贴图上传 | RGB 在 sRGB 解码前额外预乘 Alpha | 保留原纹理直通 RGB，Alpha 由各材质公式应用 |
| 拖尾颜色 | 把 ParticleSystem 的颜色转换套到 Trail 上 | 原 LineRenderer 的 RGBA8 顶点色直接透传，与原 Forward DXBC 对照；不追加 sRGB 解码 |
| 拖尾网格 | 平均单位法线加额外转角扇面 | 原 TransformZ 法线、短段平滑、斜接裁限、内侧锚点圆弧、端帽及连续 strip 连接 |
| 帧内出生 | 首帧固定 age=0，距离阈值插点 | 距离余量、帧内出生分数、发射器位置回推、补齐 lane 及新生退役使用原规则 |
| 屏幕尺寸上限 | 仅按世界尺寸投影 | 原 billboard min/max 打包与半尺寸裁限，绘制和 damage 共用 |
| 初始属性 | 通用双精度随机数分布 | 各 ParticleSystem 独立 Initial / Shape 四 lane 随机状态 |
| 三角出生 / 速度 | 上一轮仅缩放出生点，误把 Shape 单函数验证当成完整出生管线验证 | Shape 使用单位上下文，出生矩阵缩放位置及方向，再乘 StartSpeed；World 速度包含一次叶节点缩放 |
| UV | 通用 RNG 分别抽样、双精度求和 | 用粒子种子派生两个模块随机流，float32 相加后一次取模 |
| Size / CustomData | 通用 double Hermite | 原生 float32 系数、优化缓存与求值顺序 |
| MeshTri 旋转 | 对角速度做解析积分，随机混合与粒子种子无关 | World 路径先求当前角速度，再逐帧推进角度和百分比年龄 |
| 绘制阶段 | 所有粒子进入同一个 Bloom 输入 | Trail / Ring / MeshTri 先参与 Bloom，Ring3 / Ring4 在 Bloom 后以加法叠加 |
| Bloom | 短边小于 2 提前终止，强度用手抄近似值 | 保留 Diffusion 指定层数至 1 像素，按 `exp2(1.7 / 10) - 1` 换算强度 |
| Bloom 重绘范围 | 仅估计最粗一级的传播距离，外侧光晕被局部重绘裁短 | 累加各级降采样、上采样及双线性采样足迹，按实际纹理尺寸计算 |
| 输出色域 | 只转换传递函数，把 BT.709 粒子数值直接当成 P3 / BT.2020 | 背景转换到线性 BT.709 混合和计算辉光，合成时转换回输出色域 |
| 非整数缩放 | 背景导入和最终合成未共用设备像素网格，非整数原点会被连续插值 | 离屏投影、UV 和辉光区域统一对齐实际缓冲像素，沿用 KWin 输出投影 |
| 混合刷新率换屏 | 更早的呈现时间仍触发采样，移动了发射器起点但 dt=0 没有发射 | 拒绝所有不晚于已采样时刻的帧，下一有效帧保留完整跨屏距离 |

### 小三角发射参数复核

原资源的 `minScalar` 和 `scalar` 保留原始顺序，TwoConstants=3 不会排序端点；
Constant=0 只使用 `scalar`，不会误用废弃端点。四个系统都是 World 模拟（1）、
Local 缩放（1）、无延迟、simulationSpeed=1、unscaledTime=true、重力为零。
生成器还会拒绝未支持的方向对齐、随机位置、Arc Spread 和旋转翻转。

| 参数 | 点击 Ring3 | 拖动 Ring4 |
| --- | --- | --- |
| StartSpeed 两端（世界单位/秒，出生变换前） | 0.3000000119 → 0.4000000060 | 0.3000000119 → 0.2000000030 |
| 发射器叶节点缩放 | 0.3078823984 | 0.3078823984 |
| 寿命两端（秒） | 0.6999999881 → 0.6000000238 | 0.4000000060 → 0.2000000030 |
| 起始尺寸两端 | 0.1000000015 → 0.2000000030 | 0.1000000015 → 0.2000000030 |
| 数量 | 点击 Burst 4，系统上限 10 | 每世界单位距离 5，系统上限 50 |

此前未提交实现遗漏了 World 出生矩阵对方向的缩放，使小三角约快 3.248 倍。
本次对照完整出生分发函数 `0xadebc0` / `0xadf2a0` 与矩阵构造 `0xae2cb0`，
修复后的顺序是：Shape 归一化方向 → 出生矩阵 → StartSpeed → World 积分。
原矩阵构造器的变换来源参数和 simulationSpace 是两个独立字段；Circle 的前者为 0，
不是把 World 的枚举 1 直接传进去。父节点缩放不影响 Local 缩放下的叶节点尺度。
核对使用实际插件 `emitTriBurst`，包含反向端点和非四整数批次；数值快照由原函数产生。

RGB/Alpha 混合和 Dissolve 丢弃公式由原编译材质确认。GPU 测试直接运行生产片元
着色器，对比半透明颜色在已有背景上的 Ring、Additive、Dissolve 可见和丢弃结果。

局部重绘另以固定 HDR 亮源执行整屏图像对照：奇偶尺寸、竖屏、极短输出各覆盖
连续移动、大幅跳跃、屏幕边缘及消失。旧范围估计在 640×360 的首帧即出现约
0.0193 的编码 RGB 差异，位于重绘矩形外侧；累加各级采样足迹后通过同一测试。
这验证 KWin 局部重绘没有明显裁短该组测试的辉光，不等同于原游戏逐帧截图验收。

输出色彩使用 KWin 当前 RenderTarget 的实际 containerColorimetry，不以显示器名称
猜测色域。原 PC PlayerSettings 确认为 ActiveColorSpace=1（Linear）、
useHDRDisplay=false、ColorGamuts=[0]（sRGB），字段已加入源快照检查。
相对比色矩阵包含白点适配；亮度仍按参考白归一化。普通 BT.709 输出
使用严格单位矩阵。导入后的线性 HDR 背景保留负分量，避免广色域饱和颜色在进入
BT.709 工作空间后因提前钳位而改变桌面原色。
离屏 GPU 对照覆盖 BT.709 / P3 / BT.2020、gamma22 / sRGB / linear / BT.1886 / PQ，
分别核对低于辉光阈值的固定游戏颜色和饱和背景往返。旧实现的 P3 / BT.2020 / PQ
三组测试失败，补齐矩阵转换后通过。该测试验证数学和着色器链路，不代替实机
ICC、HDR 元数据、驱动与面板的最终观感验收。

背景往返进一步使用非均匀渐变及棋盘图案验证，覆盖八种旋转 / 镜像、
100% / 125% / 150% / 160% 缩放、负坐标输出原点、奇数纹理尺寸及复制回退。
旧实现在 160% 下产生约 0.0378（直读）/ 0.0553（复制）的最大编码颜色误差；
原因是逻辑矩形原点与 KWin 缩放取整后的设备像素网格之间存在亚像素相位。
此外 150% 下有些横竖互换的输出在取整后与实际缓冲相差一像素。现在以实际缓冲
尺寸建立网格，背景导入和合成使用同一组投影坐标，区域边界也对齐到像素边缘，
避免旋转前后因半像素边界选取不同像素而出现暗边。上述图案对照全部通过。

在 640×360、641×359、360×640 和 1024×64 上读取实际 GPU 的 FP16 场景和
各级 Bloom 纹理。用匹配原版的 D3D11 指令解释器逐级核对预筛选、降采样、
上采样和最终合成，所有 RGB 像素都在双线性采样与 FP16 误差容限内。
本组 gamma22 最终输出相对参考的最大编码通道差为 0.000489。
移动亮源的局部重绘测试使用整张金字塔重算作参考，并覆盖亮源消失。

Unity 的 `applyActiveColorSpace` 在最终粒子颜色传到 GPU 前进行线性颜色转换。
RGBA8 缓存和整数乘法细节以本地匹配 UnityPlayer 原函数为依据。

## 原函数验证

[数值内核](../src/unity)保留单精度操作顺序。原生验证直接执行哈希匹配的函数，
测试快照记录运行结果。

| 项目 | 对照结果 |
| --- | --- |
| InitialModule | 84,000 颗粒子的种子、尺寸、旋转、RGBA8、倒数寿命和 RNG 状态逐位一致 |
| Gradient | 当前四个系统的 4,216 次浮点/打包/缓存求值及寿命颜色更新逐位一致；65,536 组整数乘色一致 |
| UV | 当前两个系统及边界范围，共 468,864 次归一化帧求值逐位一致 |
| Circle Shape | 当前两个系统及缩放上下文，共 103,128 次出生位置/方向与 1,014 组连续 RNG 状态逐位一致 |
| Curve | 当前七条曲线的 7,267 次普通求值、16,640 次优化求值及系数/极值对照通过 |
| StartSpeed | 当前四个系统及边界范围，共 806,016 次速度采样逐位一致 |
| Birth context / dispatcher | World/Local 原矩阵构造、19,200 组真实 Ring3/Ring4；163,942 个位置/速度/年龄及数组槽位一致，包含帧内时间、移动回推、容量裁限、SIMD 补齐、新生退役及接续积分 |
| Emission | 4 个源系统、13,464 个连续/边界窗口：计数、浮点余量、间隔与 RNG 一致 |
| Distance emission | 17,280 个连续步：源距离计数、余量、间隔与 RNG 一致 |
| Trail points / retirement | 1,536 组、14,784 个头尾点/标记逐位一致；896 组退役边界和保留前驱规则一致 |
| Trail mesh | 源 TransformZ、Corner=4、Cap=1；72 条路径、5,072 个顶点，坐标、顶点数、UV 与 RGBA8 一致，含零长度、短段、折返及随机路径 |
| Billboard limits | 600 组源上限、视口、缩放和尺寸探针，原打包/裁限半尺寸一致 |
| World Rotation | 526,336 次角速度求值逐位一致，包含粒子种子派生的混合和翻转流 |
| Motion / Age | 858,624 次位置、速度和百分比年龄采样逐位一致，覆盖 41,472 个步进 |
| Timestep | 786,432 次步长选择、2,808 种追赶调度逐位一致；源 Maximum Particle Timestep 为 `0.029999999329447746` 秒 |
| Retirement | 27,648 种零时间/跨寿命边界案例通过，169,728 个存活者身份与状态一致 |

原函数入口包括 `InitialModule 0xb1a8b0`、`ColorOverLifetime 0xaf92d0`、
`AnimationCurve 0x485d10`、旋转常规路径 `0xb05fd0`、TwoCurves 求值 `0xad5b60`。
管线 `0xae4300` 在积分 `0xadd7a0` 之前求旋转角速度。旋转调用保留原算术，不补丁
浮点计算；部分既有颜色 oracle 为原函数提供独立 Windows TLS 和静态缓存。

[原函数验证工具](../scripts/verify-unity-native.py)提供可选的专项验证入口。
需要 Linux x86-64、C++ 编译器、Qt6Core、Qt6Gui、Python `pefile` 和 `capstone`，
以及匹配的原版运行库和参考夹具。

此脚本只在临时目录提取和重定位经过哈希校验的函数，构造时写入，运行前改为 RX，
不使用同时可写可执行的页面。仓库不保存游戏 DLL 或机器码片段。维护者可显式加 `--write-snapshot`
更新数值快照；日常测试只读取仓库中的数值快照，不运行原游戏函数。

## 调用链与宿主适配

距离发射 `0xafb120` 与时间发射 `0xafb2d0` 共用余量和随机状态。距离出生先占用
系统容量，再推进本帧已有及新生粒子；新生粒子的初始时间会扣除待追赶时间。
每次拖动维护同一组粒子存储，不能按每颗粒子或每个输入事件建立独立系统。
对齐后的组合验证直接执行原出生分发器和积分器，检查存活数量及最终槽位顺序。

Trail 网格使用 `0x394c90` 及其法线、斜接、转角与端帽函数作为参考；临时内存分配
由测试夹具提供，数值指令保持原样。Forward 顶点/片元 DXBC 的哈希及透传颜色证据
记录在参数快照中。共享加法材质不意味着 ParticleSystem 和 Trail 有相同的 CPU
颜色处理：前者 ApplyActiveColorSpace=true，后者直接使用 LineRenderer 产生的字节色。

原资源的 GameObject 层与 SortingLayer 是两个独立字段。资源快照从 TagManager
解析层名：Trail / Ring / MeshTri 属于 UIAlwaysVisible（13，队列 4499），
Ring3 / Ring4 属于 LetterBox（8，队列 4550），UICamera 的 culling mask 包含两层。
原版 `UIRenderPass` 在构造时分别为这两组建立透明过滤器，`Execute` 用
`0x17`（CommonTransparent）建立透明 DrawingSettings。它在 `0x6c62100` 提交
UI 透明列表，`0x6c62156` 调用 RenderBloom，再在 `0x6c62287` 提交
LetterBox 列表。插件按这一顺序冻结辉光金字塔，再加上小三角的线性颜色。
离屏回归用同一 HDR 亮源分别验证两个绘制阶段的核心亮度和几何外侧辉光。

同一列表内，SortingLayer/Order、RendererPriority、SortingFudge 和粒子
SortMode 均为零，正交相机下同平面主距离相同。后续排序键含运行时渲染状态及
node ID。插件采用稳定提交顺序。Ring 的目标衰减会使其与其它 4499 元素的
重叠随顺序改变，这部分仍待原游戏运行时记录验证。4550 中的加法在理想线性
算术下可交换，半精度累加可能有舍入差异。

`CreateDrawingSettings`（`0x6c3b7b0`）将当前 `RenderingData.cameraData.camera`
传给 `SortingSettings`。原 UnityPlayer 的初始化函数 `0xfd6f0` 从该相机读取
视图矩阵、相机位置和透明排序模式。默认正交模式下主距离按视深比较。
运行时采用的相机及其后续状态仍需随 DrawRenderers 记录一起采集。
原生记录构造函数在 `0x457a6f..0x457a8f` 读入 Renderer 边界中心和 SortingFudge，
在 `0x457b28..0x457b46` 计算视深，再由 `0x457c0a..0x457c13` 产生正交主排序键。
同深度时，`0x17` 比较器仍会比较组、节点字段、批处理和材质状态，最后才比较节点下标。
已重新执行参考项目的原生比较器测试，5,407,488 对构造记录与移植比较器一致。
测试输入提供这些状态字段，游戏实时填入的值仍需实机记录。

另一版日服客户端的 Unity 原生运行库哈希与上表相同。其资源表
（SHA-256 `ba94eaecdfd1ec92610e38fa55d9a969af58f3f4cd8e056adace240ab58ad282`）
重新提取后生成相同的 C++ 参数表。改号的四张贴图仍指向相同名称与 RGBA 像素，
Shape 里的网格引用仍指向 FX_MESH_Triangle；四个材质对应的三套着色器压缩程序哈希相同。
新版客户端的 IL2CPP 注册表也指向相同的调用顺序。
`0x6e4764b` 写入 CommonTransparent，`0x6e48820` 提交 UIAlwaysVisible，
`0x6e48876` 执行 RenderBloom，`0x6e489a7` 提交 LetterBox。

材质快照记录四个材质的 Forward 混合因子、RGB 和 Alpha 运算及颜色写入掩码。
四个材质均为 BlendOp.Add。插件开始绘制时显式设置该运算，结束后恢复宿主状态。
实际几何离屏回归使用两个不同年龄的点击、MeshTri 和一条重叠拖尾。
将拖尾从圆盘前移到圆盘后，最大单通道差异在 HDR 场景为 14.7754，
在辉光合成并编码后为 0.3394。这个固定场景验证顺序具有可见影响。

FXTouch.Play/Stop 与 TouchEffectCreater 的创建、位置更新及归还调用链也已检查：
根系统负责带子节点播放；相机坐标先裁到视口再 ScreenToWorld；归还时清理 Trail 并
Stop 粒子。游戏协程以 `Duration × 60` 次 yield 等待后归还对象池。插件按实际寿命
回收，不移植游戏对象池的帧数等待，以保留已经选择的随屏幕刷新率运行行为。

原版鼠标路径位于 `TouchEffectCreater.Update` 的 `0x219ca03..0x219cb6e`。
GetMouseButtonDown 读取当帧鼠标位置并创建实例，CreateEffect 先定位再调用
FXTouch.Play。GetMouseButton 持续按 Update 帧更新位置，GetMouseButtonUp
启动归还协程并移除跟随记录，松开分支没有额外的末端位置更新。
插件将新点击放入首个有效呈现帧。新按住的手势在同一帧用光标位置初始化
圆盘和拖尾起点，后续帧才按两个采样点之间的距离发射。两个呈现帧之间完成的快点
保留按下位置，这是 KWin 事件模式对这类输入的处理。
空闲后第一帧即完成出生初始化，第二帧使用完整帧间隔推进粒子。
60 / 144 / 240 Hz 的逻辑检查覆盖这一路径，独立虚拟 KWin 的输入检查覆盖
首帧前移动和首帧前松开。原游戏实际输入事件与其 PlayerLoop 的帧归属仍待运行时对照。

原版 UnityPlayer 的 TimeManager 在 `0x45f51f..0x45f552` 计算实际帧间隔，
在 `0x45f5d7..0x45f63c` 对游戏时间应用 Maximum Allowed Timestep，
随后在 `0x45f6c4..0x45f6d9` 发布两套时钟。粒子调度器
`0xad5620..0xad563b` 按 useUnscaledTime 选取帧间隔，FX_Touch 的圆环和三角粒子
使用实际时间，再按原生 Maximum Particle Timestep 分步推进。Ring4 距离发射用
同一个实际间隔计算发射器速度和新生粒子的年龄偏移。
TrailRenderer 在 `0xde5058..0xde5072` 读取游戏时钟，先采样新点
（`0xde52cc`），随后以同一时刻回收旧点（`0xde52db`）。
插件以源 TimeManager 的 100 ms 上限推进拖尾，并保留这一帧内顺序。

这些是已明确的宿主边界：

- 自动种子的初始熵和按键事件到呈现帧的归属来自 KWin；60/144/240 Hz 与混合刷新率
  逻辑有覆盖，不声明 KWin 与 Unity PlayerLoop 的外层调度完全相同。
- 插件的动画速度选项属于宿主功能；游戏 FxTrailTimeScale 的低全局时间倍率停车序列
  不作为该选项的逐帧复刻目标。默认倍率 1 不进入原游戏的停车分支。
- 原游戏运行时分配的透明排序平局键没有跨引擎固定对应关系，插件保持稳定顺序。
- 显示器缩放、桌面色彩管理和输出传递函数由 KWin 提供。

## 逐帧画面对照

实机对照需要固定原游戏种子、输入历史与帧时刻，分别比较几何、线性 HDR 粒子层、
Bloom 和最终输出，并记录同队列绘制顺序和输入归帧。
