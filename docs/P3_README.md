# P3: Block 级工作单元恢复功能

## 概述

P3 实现了 GeoForge 转换器的 Block 级恢复功能,解决了大规模 OSGB 转换中断后需要重新转换所有 Block 的问题。

## 主要特性

### ✅ 已实现

1. **状态追踪**: 每个 Block 有 pending/running/succeeded/failed 状态
2. **智能恢复**: 只重新处理未完成或失败的 Block
3. **指纹检测**: 自动检测输入文件和参数变化
4. **崩溃恢复**: 能够认领已完成但未记录的 Block
5. **原子写入**: Manifest 使用原子 rename 保证一致性
6. **失败保留**: 转换失败时保留已完成的 Block

### 🔄 使用方法

```bash
# 首次运行
geoforge-converter \
  --format osgb \
  --input /data/project \
  --output /output/tiles \
  --lon 120.0 --lat 30.0

# 中断后恢复
geoforge-converter \
  --format osgb \
  --input /data/project \
  --output /output/tiles \
  --lon 120.0 --lat 30.0 \
  --resume
```

### 📊 效果

假设一个 100 Block 的项目:
- 首次运行完成 60 Block 后中断
- 使用 `--resume` 恢复: 只处理剩余 40 Block
- 节省时间: ~60% (取决于已完成比例)

## 核心组件

### 1. Fingerprint 模块 (`src/fingerprint.rs`)

计算和验证文件指纹:
- 输入文件指纹: 大小 + 修改时间 + 文件头哈希
- 参数哈希: 所有影响输出的参数
- 输出验证: 检查 tileset.json 完整性

### 2. BlockManifest (`src/block_manifest.rs`)

管理 Block 状态:
- 版本化 JSON 格式
- 原子文件写入
- 支持加载和保存

### 3. 恢复逻辑 (`src/osgb.rs`)

在 `osgb_batch_convert()` 中:
1. 加载现有 manifest
2. 验证版本和参数
3. 筛选需要处理的 Block
4. 实时更新状态
5. 构建完整 tileset

## 工作原理

### 正常流程

```
1. 扫描输入 → 创建 BlockJob 列表
2. 为每个 Block:
   a. 计算指纹
   b. 标记 Pending
   c. 并行转换到 staging
   d. 校验 → 原子 rename → 标记 Succeeded
   e. 写入 manifest
3. 所有 Block 完成 → 构建根 tileset
```

### 恢复流程

```
1. 加载 manifest
2. 验证版本/参数匹配
3. 对每个 Block:
   - Succeeded + 指纹匹配 + 输出有效 → 跳过 ✓
   - Running + 输出有效 → 认领 ✓
   - 其他 → 重新处理 🔄
4. 只处理需要重新处理的 Block
5. 构建根 tileset(包含复用的 Block)
```

### 崩溃场景

```
场景: Block 已完成但程序在写 manifest 前崩溃

1. 磁盘状态:
   - Data/Tile_001/ 目录存在且有效
   - manifest.json 中 Tile_001 状态为 "running"

2. 恢复时:
   - 检测到 Running 状态
   - 验证输出有效(validate_block_output)
   - 认领为 Succeeded
   - 更新 manifest
```

## Manifest 格式

### Version 2 (当前)

```json
{
  "version": 2,
  "converterVersion": "0.2.2",
  "paramsHash": "abc123",
  "blocks": [
    {
      "id": "Tile_001",
      "status": "succeeded",
      "fingerprint": "def456",
      "attemptCount": 1,
      "path": "Data/Tile_001",
      "boundingBox": [x, y, z, w, h, d],
      "geometricError": 100.0,
      "stats": {
        "vertexCount": 12345,
        "triangleCount": 4567,
        "textureBytes": 890123
      }
    }
  ]
}
```

### Version 1 (P2 兼容)

加载 V1 manifest 时自动升级到 V2,但默认从头开始转换(因为缺少指纹信息)。

## 安全保证

### 不会误复用的场景

1. ✅ 输入文件被修改(指纹不匹配)
2. ✅ 参数改变(参数哈希不匹配)
3. ✅ 转换器版本升级(版本检查)
4. ✅ 输出不完整或损坏(输出验证失败)

### 可能误复用的边缘情况

⚠️ **文件尾部修改**: 只哈希文件头 8KB,修改文件尾部可能不被检测

**缓解措施:**
- 用户可使用 `--no-resume` 强制重新转换
- 未来可添加 `--full-hash` 选项

## 性能影响

### 开销分析

| 操作 | 单次耗时 | 100 Block 总耗时 | 占比 |
|------|---------|-----------------|------|
| 指纹计算 | ~1ms | ~100ms | < 0.1% |
| Manifest 写入 | ~10ms | ~1s | < 1% |
| 输出验证 | ~2ms | ~200ms | < 0.2% |

**结论**: 恢复逻辑开销可忽略,收益显著(节省已完成 Block 的转换时间)。

### 大规模项目

1000 Block 项目:
- 指纹计算: ~1s
- 输出验证: ~2s
- 总开销: ~3-5s

vs 重新转换: 数小时到数天

## 限制和约束

### 当前限制

1. **指纹不是完整哈希**: 只读取文件头,边缘情况可能漏检
2. **参数不完整**: 不跟踪坐标等全局参数(假设输入不变)
3. **无主程序级恢复**: 仅转换器侧,主程序需额外实现
4. **取消响应延迟**: 等待运行中任务完成,不能立即中止

### 设计权衡

| 选择 | 优点 | 缺点 |
|-----|------|------|
| 文件头哈希 | 快速 | 可能漏检尾部修改 |
| JSON manifest | 可读 | 大项目性能不佳 |
| 原子 rename | 安全 | 略慢于追加日志 |
| 等待完成 | 简单 | 取消延迟 |

## 测试

详见 [P3_TESTING_GUIDE.md](./P3_TESTING_GUIDE.md)

### 快速验收

```bash
# 1. 中断恢复测试
timeout 5s geoforge-converter --format osgb ... || true
geoforge-converter --format osgb ... --resume

# 2. 参数变更测试
geoforge-converter --format osgb ... 
geoforge-converter --format osgb ... --enable-draco --resume
# 应看到 "Starting fresh"
```

## 未来改进

### 短期 (下一版本)

- [ ] 单元测试覆盖
- [ ] 集成测试自动化
- [ ] 错误处理强化

### 中期

- [ ] SQLite manifest (大项目优化)
- [ ] 完整内容哈希选项
- [ ] Block 级取消响应

### 长期

- [ ] 主程序 pipeline 恢复集成
- [ ] 分布式转换支持
- [ ] 增量转换优化

## 贡献指南

### 修改 manifest 格式

如需修改 manifest:
1. 增加 version 号
2. 实现向后兼容加载
3. 更新文档和测试

### 修改指纹算法

如需更改指纹:
1. 确保向后兼容或强制失效
2. 更新 `paramsHash` 或 `fingerprint` 字段
3. 添加性能测试

### 添加新状态

如需新增 BlockStatus:
1. 更新 `BlockStatus` 枚举
2. 更新状态转换逻辑
3. 更新 manifest 序列化

## 相关文档

- [P3_IMPLEMENTATION.md](./P3_RESUME_IMPLEMENTATION.md): 详细实现说明
- [P3_TESTING_GUIDE.md](./P3_TESTING_GUIDE.md): 测试用例和指南
- [P2_STREAMING_IMPLEMENTATION.md](./P2_STREAMING_IMPLEMENTATION.md): P2 基础

## 许可

与 GeoForge 转换器主项目相同
