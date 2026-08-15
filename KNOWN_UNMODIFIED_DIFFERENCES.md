# 已知但暂不修改的 Unity 差异

本文只记录已经发现、但目前明确不修改或证据不足以修改的项目。判断依据优先级为：

1. Unity 实际运行结果；
2. 游戏反编译代码、原始 Prefab、材质和解包 shader；
3. Unity API 的确定语义；
4. 注释和项目文档仅作线索，不作为最终证据。

本文不是待办清单。标为“确认不改”的项目，除非获得新的 Unity 实测证据，否则不应再次据此修改代码。

## 确认不改

### 多输出环境不照搬单相机坐标 clamp

游戏的 `TouchEffectCreater.SetDragPosition()` 会把输入位置限制到单个 Camera 的
`[0, pixelWidth] × [0, pixelHeight]`。KWin 使用统一桌面坐标，鼠标可自然跨越多个输出。

若把位置限制到按下时所在输出，跨屏拖动会被卡在原屏边缘，Trail 和 Ring4 也会堆在边缘。这是平台模型差异，不是粒子效果公式差异，因此保留 KWin 的真实全局指针坐标。

### Ring3 不按 Prefab 的 `maxNumParticles: 3` 裁成三个

原始 Prefab 中 Ring3（PS18）同时保存了：

- burst 数量 4；
- `maxNumParticles: 3`。

但 Unity 实际运行中确认生成四个粒子。当前实现同样生成四个，因此不根据孤立的
`maxNumParticles` 字段裁剪。该字段在这个具体 burst 场景中的实际引擎行为不能机械地按“最终最多可见三个”解释。

## 待进一步验证

### Ring3/Ring4 的 Mesh Shape 出生位置

原始 Prefab 的 Ring3/Ring4 Shape 保存为：

- `type: 10`，引用 `FX_MESH_Triangle`；
- `placementMode: 1`；
- Shape scale 分别为 `0.3` 和 `0.15`。

Unity API 的枚举映射表明 Mesh Shape 的 placement 值 1 对应 Edge。原始三角网格顶点约为：

```text
( 0.09902798,  0.13489860)
(-0.14606594,  0.00000000)
( 0.09902798, -0.13489860)
```

当前实现没有沿三角网格边采样，而是为粒子随机一个角度，并在固定径向半径上出生。
不过目前尚未完整证明 Unity 的 Shape mesh 坐标、Shape scale、子对象 Transform scale、
粒子尺寸和屏幕世界单位之间的最终换算链；用户肉眼对比也未观察到差异。因此在获得
Unity 粒子出生位置采样或可复现实验之前不修改。

速度方向仍按已确认的规则处理：强制设为出生位置相对发射中心的径向方向。

### 异常长帧的时间步上限

当前 KWin 实现把单帧推进量限制为 `0.1s`，用于避免从锁屏、休眠或调试暂停恢复时
粒子一帧跳完。Unity 工程的 `TimeManager.asset` 保存了：

```text
Maximum Allowed Timestep: 0.33333334
```

但相关粒子系统同时启用了 `useUnscaledTime: 1`，现有证据不足以证明 Unity 粒子模拟在
这种异常帧下确实采用 `0.33333334s`，也不足以证明它与 KWin 的呈现时间戳恢复语义等价。
正常帧率下该差异完全不生效，因此暂不修改。

### OpenGL 纹理绑定状态未完整恢复

渲染结束时当前活动纹理单元会恢复到 `GL_TEXTURE0`，但各纹理单元进入效果前绑定的纹理
对象没有逐个保存和恢复。KWin 和后续效果通常会在绘制前自行绑定所需纹理，目前没有发现
实际故障或 Unity 视觉差异。为避免每帧增加额外 GL 状态查询，在出现可复现兼容问题前不处理。

### 粒子绘制依赖入口的 Cull/Depth 状态

Unity 的三个原始粒子 shader 都明确指定 `Cull Off`、`ZWrite Off`、`ZTest Always`。
当前实现没有在插件内部显式关闭 `GL_CULL_FACE` 和 `GL_DEPTH_TEST`，依赖 KWin 正常合成
路径留下的状态。私有 HDR FBO 没有深度附件，因此 depth test 通常不产生实际差异；若入口
意外开启背面剔除，则理论上可能丢失部分三角面。

可以在 `beginFrame()` 保存二者、插件绘制期间关闭、所有出口恢复，从而不影响插件外部。
当前正常环境中没有观察到缺面，按用户决定暂时保留现状。

### Blend equation、Scissor 与 Color Mask 依赖入口状态

Unity 材质的混合运算为 Add；当前实现设置了各层的 blend factors，但没有显式把 RGB/alpha
blend equation 设为 `GL_FUNC_ADD`。如果入口状态使用减法或 Min/Max，结果会不同。

插件切换到 output-local HDR FBO 和多级 bloom FBO 后，也没有显式关闭继承的
`GL_SCISSOR_TEST`。若 KWin 入口留下屏幕设备坐标的 scissor，非零 render offset 或低分辨率
bloom FBO 理论上可能被错误裁剪。`GL_COLOR_WRITEMASK` 同样没有强制恢复为全 RGBA。

可行方案是保存 blend equation、scissor 开关与矩形、color mask，插件内部设为 Add、关闭
scissor、启用全部颜色通道，并在所有出口恢复。当前环境未观察到相应故障，按用户决定暂时保留。

### HDR/广色域输出缺少 BT.709 到目标原色的转换

Unity 粒子颜色与四张 sRGB 贴图属于 BT.709/sRGB 原色。当前 KWin 路径会按 RenderTarget
传递函数正确解码、编码亮度，但没有转换 RGB 原色空间。普通 SDR RenderTarget 同样使用
BT.709，矩阵为恒等，因此当前可见结果正确；HDR 输出通常使用 BT.2020 容器，此时桌面背景
处于 BT.2020 线性 RGB，而粒子仍按 BT.709 数值直接加入，同一组 RGB 数值代表的实际色度
不同，蓝色 Trail、青蓝 Ring 和 MeshTri 理论上会出现色相或饱和度偏差。

可行方案是使用 `ColorDescription::containerColorimetry()` 计算 BT.709 到目标色域的相对色度
矩阵，把粒子和 bloom 转到目标线性空间后再与桌面混合。该修改涉及 HDR 色彩管理，且当前
没有 HDR/BT.2020 输出上的实测对照，按用户决定暂不修改。

### 极窄输出的 Bloom 金字塔会少建层级

Unity 的 bloom 降采样尺寸使用 `Mathf.Max(1, size / 2)`，允许金字塔出现 `1×N` 或
`N×1` 的层级。当前实现要求每一级宽高都至少为 2，认为单像素短边上的 box4 已退化，
因此提前停止。正常横屏、竖屏和常见超宽输出不会触发；短边约小于 128 设备像素时才会
出现，例如 `100×1920` 上 Unity 建 6 级而当前建 5 级。该尺寸不属于实际桌面输出范围，
按用户决定暂不修改。

### Ring3/Ring4 Mesh Shape 的精确采样分布

Prefab 与 Unity API 能确认 Shape 使用三角网格边缘采样，但无法仅靠序列化数据还原
Unity 内部对边长的加权方式、随机数消耗顺序以及粒子出生点的最终变换链。当前实现使用
固定半径上的均匀随机角，并将速度强制设为出生位置的径向方向。后者已经由实际效果确认；
前者需要 Unity 运行时导出一批粒子出生坐标后才能进行统计比对。

### TrailRenderer 急转弯与端帽的精确拓扑

当前实现已经对齐 `minVertexDistance`、宽度、渐变、纹理方向、4 个 corner vertices 和
1 个 cap vertex，但 Unity TrailRenderer 生成急转弯连接面和端帽三角形的内部算法没有
公开的稳定合同。普通轨迹肉眼一致；若要逐像素确认，需要在 Unity 中捕获包含固定锐角路径
的网格或参考帧。

### 同一透明 Render Queue 内的稳定排序

Trail、Ring、Ring3 和 Ring4 的 Render Queue 均为 3000，且 Prefab 中相关 Renderer 的
`sortingOrder`、`sortingFudge` 和深度没有提供可区分的排序键。Unity 对完全相同排序键的
透明 Renderer 不提供可由 Prefab RootOrder 推导的稳定顺序。当前实现使用固定提交顺序；
在获得 Unity 帧调试器或运行时抓帧证据前不调整。

### 自动随机种子与跨子系统随机序列

Unity 的每个 ParticleSystem 都启用 `autoRandomSeed`，具体种子及各模块消耗随机数的顺序
不属于 Prefab 数据。当前实现使用一个进程内 RNG 为各层采样，因此可以对齐随机分布，不能
对齐某一次运行中每颗粒子的具体随机结果。除非 Unity 侧固定并导出所有 ParticleSystem 的
random seed，否则不存在可复现的逐粒子目标序列。

### 输入事件的帧内出生时间

Unity 在 `Update()` 中读取按键并创建效果，KWin 插件在鼠标按下事件中创建效果。当前实现
保证新实例首次绘制时 `age=0`，与 Unity 新 ParticleSystem 的首帧语义一致，但两套事件循环
没有共同的亚帧时间戳，无法证明高刷新率下出生时刻完全相同。没有观测到首帧位置或寿命差异，
暂不人为加入事件到呈现之间的时间补偿。

### HDR/BT.2020 输出上的最终视觉对照

传递函数的解码与编码已按 KWin RenderTarget 描述实现，普通 SDR 输出可验证。HDR 输出还
缺少与 Unity 在相同显示器、相同参考白和相同系统色彩管理条件下的截图或测量，因此 Bloom
强度、峰值裁剪和广色域转换只能由公式审计，尚未完成真实输出的端到端对照。
