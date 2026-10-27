# GFX 高级能力

[返回基建总览](../infrastructure_cn.md)

主要头文件：`<gfx/bake/baked_model.h>`、`<gfx/bake/baked_serializer.h>`、`<gfx/math/pose_stack.h>`、`<gfx/renderer/model_state.h>`、`<gfx/renderer/render.h>`、`<gfx/renderer/schedule.h>`、`<gfx/renderer/color.h>`、`<gfx/renderer/bone_attribute.h>`、`<gfx/renderer/vertex/kind.h>`。

```text
model payload + RGBA texture
        -> BakeModel
        -> BakedModel
        -> SerializeBakedModel / ReadBakedModel
        -> ModelState::Extract
        -> Render
```

- `BakeModel` 是 renderer 的烘焙入口；`TryBakeModel` 仅检查可烘焙性。
- `SerializeBakedModel` / `ReadBakedModel` 读写 baked cache。读取门面负责结构、索引、层级、有限浮点值和 SIMD 布局校验。
- `BakedModel::CapacityBytes` 统计固定结构、全部 vector capacity 与 SIMD 分区；`BakedModelSerializationBytes` 复用 serializer 的尺寸推导，输出预分配精确容量。两者只读 backing，可与运行提取并行；Java 存储域另计 metadata、codec 与 staging 验证。
- Java 运行消费者与派生存储共享不可变 geometry。消费者只撤强引用，最后引用消失后由 Cleaner 释放 wrapper；共享后的显式销毁被拒绝。native 函数的同步借用仍由调用方保活，私有构造失败保留唯一 owner 回滚。
- `PoseStack` 维护 pose/normal、uniform scale、tangent orientation 和 normal scale。`PushPose` 复制当前状态；`PopPose(depth)` 回到保存的深度。
- `EulerZYX`、`Mat4Mul` 和 `AffineMul` 提供 generic/ISA-tagged 路径，`SignBit` 提供位级符号判断。ISA overload 仅由 runtime 分派路径调用。
- `ModelState::Extract` 生成 renderer 状态并更新 schedule；成功后才能交给 `Render`。
- `Render` 按 `VertexKind`、runtime SIMD、render context 和 schedule 写入调用方 vertex buffer；容量不足或状态非法时返回 status。需要区分恢复责任的调用者同时接收 `RenderResult`：完好 publication 的外部容量/宿主矩阵拒绝及纯组合溢出为 `kOutputRejected`，坏状态或无法证明生成完整性的失败为 `kStateInvalid`，完整生成才为 `kSuccess`；混合失败中坏状态优先。调用者负责宿主提交与对应 publication 撤销；原生函数返回前所有 worker 已结束。

`Pixel`、`Color`、`BoneAttribute` 和 `BonePose` 具有固定大小、对齐或字段布局，不能作为普通 struct 重排或 reinterpret cast。

`RenderSchedule`、`RenderState`、`VertexKindTraits`、cube buffer 和 worker-ready 协议只供 renderer 内部跨 translation unit 协作。新增 vertex layout 或调度模式仍通过 `ModelState` / `Render` 门面闭环。
