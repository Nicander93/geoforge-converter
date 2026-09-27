# P3 恢复功能测试指南

## 测试环境要求

### 必需工具
- Rust 1.83+ with Cargo
- CMake 3.10+
- vcpkg (或已安装 OSG、GDAL、PROJ 依赖)
- GCC/G++ (Linux) 或 MSVC (Windows)

### 测试数据
需要准备 OSGB 测试数据集:
- 小型: 3-5 个 Block (~100MB)
- 中型: 20-50 个 Block (~1-5GB)
- 大型: 100+ 个 Block (可选)

## 测试用例

### TC1: 基本恢复 - 正常中断

**目的:** 验证 Ctrl+C 中断后能恢复

**步骤:**
```bash
# 1. 首次运行,中途中断(等待 30-50% 完成)
geoforge-converter \
  --format osgb \
  --input tests/fixtures/osgb_medium \
  --output /tmp/test_resume \
  --lon 120.0 --lat 30.0 --alt 0.0
# 按 Ctrl+C 中断

# 2. 检查 manifest
cat /tmp/test_resume/block_manifest.json | jq '.blocks[] | {id, status}'

# 3. 恢复运行
geoforge-converter \
  --format osgb \
  --input tests/fixtures/osgb_medium \
  --output /tmp/test_resume \
  --lon 120.0 --lat 30.0 --alt 0.0 \
  --resume

# 4. 验证
# - 日志应显示 "Reused N succeeded blocks"
# - to_process 应小于总 blocks
# - 最终输出完整 tileset.json
```

**预期结果:**
- 第一次运行: 部分 Block succeeded, 部分 pending
- 恢复运行: 跳过 succeeded, 只处理 pending/failed
- 最终: 所有 Block succeeded, tileset.json 完整

### TC2: 崩溃恢复 - 认领孤儿 Block

**目的:** 验证 Block 已完成但 manifest 未更新的场景

**模拟方法:**
```bash
# 1. 正常运行到完成
geoforge-converter --format osgb ... --output /tmp/test_crash

# 2. 手动修改 manifest,将某些 succeeded 改为 running
jq '.blocks[0].status = "running"' \
  /tmp/test_crash/block_manifest.json > /tmp/manifest_tmp.json
mv /tmp/manifest_tmp.json /tmp/test_crash/block_manifest.json

# 3. 恢复运行
geoforge-converter --format osgb ... --output /tmp/test_crash --resume

# 4. 验证日志包含 "Reclaiming completed block after crash"
```

**预期结果:**
- 检测到 `Running` 状态但输出有效的 Block
- 直接认领为 `Succeeded`,不重新转换
- 总耗时接近零(只验证和构建根 tileset)

### TC3: 参数变更 - 拒绝恢复

**目的:** 验证参数改变时不复用旧结果

**步骤:**
```bash
# 1. 完成转换(无 Draco)
geoforge-converter --format osgb ... --output /tmp/test_params

# 2. 修改参数(启用 Draco)
geoforge-converter --format osgb ... --output /tmp/test_params \
  --enable-draco \
  --resume

# 3. 检查日志
# 应看到 "Manifest params hash mismatch ... Starting fresh."
```

**预期结果:**
- 检测到参数哈希不匹配
- 警告并从头开始
- 所有 Block 重新转换

### TC4: 输入变更 - 检测指纹变化

**目的:** 验证输入文件变化时重新处理

**步骤:**
```bash
# 1. 完成转换
geoforge-converter --format osgb ... --output /tmp/test_input

# 2. 修改某个输入文件(touch 改变 mtime)
touch tests/fixtures/osgb_medium/Data/Tile_001/Tile_001.osgb

# 3. 恢复运行
geoforge-converter --format osgb ... --output /tmp/test_input --resume

# 4. 检查日志
# Tile_001 应显示 "will reprocess"
# 其他 Block 应显示 "Reusing succeeded block"
```

**预期结果:**
- 指纹变化的 Block 重新处理
- 其他 Block 复用
- 最终结果反映新输入

### TC5: 部分失败 - 保留成功 Block

**目的:** 验证失败时不丢弃已完成工作

**模拟方法:**
```bash
# 1. 准备混合数据集(部分 Block 损坏)
cp -r tests/fixtures/osgb_medium /tmp/test_fail_input
echo "corrupted" > /tmp/test_fail_input/Data/Tile_003/Tile_003.osgb

# 2. 运行转换(会失败)
geoforge-converter --format osgb \
  --input /tmp/test_fail_input \
  --output /tmp/test_fail \
  --lon 120.0 --lat 30.0 || true

# 3. 检查 manifest
cat /tmp/test_fail/block_manifest.json | jq '.blocks[] | {id, status}'

# 4. 修复输入后恢复
echo "fixed" > /tmp/test_fail_input/Data/Tile_003/Tile_003.osgb
geoforge-converter --format osgb ... --resume
```

**预期结果:**
- 第一次运行: 部分 succeeded, Tile_003 failed
- manifest 保留所有状态
- 恢复后: 只重试 failed Block

### TC6: 版本升级 - 拒绝复用

**目的:** 验证转换器版本变化时从头开始

**步骤:**
```bash
# 1. 使用旧版本完成转换
geoforge-converter-0.2.1 --format osgb ... --output /tmp/test_version

# 2. 手动修改 manifest 的 converterVersion
jq '.converterVersion = "0.2.1"' \
  /tmp/test_version/block_manifest.json > /tmp/manifest_tmp.json
mv /tmp/manifest_tmp.json /tmp/test_version/block_manifest.json

# 3. 使用新版本恢复
geoforge-converter-0.2.2 --format osgb ... --output /tmp/test_version --resume

# 4. 检查日志
# 应看到 "Manifest converter version mismatch ... Starting fresh."
```

**预期结果:**
- 检测到版本不匹配
- 警告并从头开始
- 所有 Block 重新转换

### TC7: 无恢复标志 - 覆盖模式

**目的:** 验证不加 `--resume` 时的行为

**步骤:**
```bash
# 1. 完成转换
geoforge-converter --format osgb ... --output /tmp/test_no_resume

# 2. 不加 --resume 再次运行
geoforge-converter --format osgb ... --output /tmp/test_no_resume

# 3. 观察行为
```

**预期结果:**
- 不加 `--resume` 时创建新 manifest
- 所有 Block 重新转换
- 旧 manifest 被覆盖

### TC8: 空输出目录 - 首次运行

**目的:** 验证没有 manifest 时正常工作

**步骤:**
```bash
rm -rf /tmp/test_fresh
geoforge-converter --format osgb ... --output /tmp/test_fresh --resume
```

**预期结果:**
- `--resume` 不影响首次运行
- 创建新 manifest
- 所有 Block 正常转换

## 自动化测试框架

### 单元测试 (未实现)

在 `tests/resume_tests.rs` 中:

```rust
#[test]
fn test_fingerprint_computation() {
    // 测试指纹计算稳定性
}

#[test]
fn test_manifest_version_check() {
    // 测试版本检查逻辑
}

#[test]
fn test_params_hash() {
    // 测试参数哈希变化检测
}

#[test]
fn test_output_validation() {
    // 测试输出验证逻辑
}
```

### 集成测试脚本

```bash
#!/bin/bash
# tests/integration/test_resume.sh

set -e

TEST_DATA="tests/fixtures/osgb_small"
OUTPUT_DIR="/tmp/geoforge_resume_test"

echo "=== Test 1: Basic resume ==="
rm -rf $OUTPUT_DIR
timeout 5s geoforge-converter --format osgb \
  --input $TEST_DATA --output $OUTPUT_DIR --lon 0 --lat 0 || true

# 验证部分完成
test -f $OUTPUT_DIR/block_manifest.json
succeeded=$(jq '.blocks[] | select(.status=="succeeded") | .id' \
  $OUTPUT_DIR/block_manifest.json | wc -l)
echo "Succeeded blocks: $succeeded"

# 恢复
geoforge-converter --format osgb \
  --input $TEST_DATA --output $OUTPUT_DIR --lon 0 --lat 0 --resume

# 验证完成
test -f $OUTPUT_DIR/tileset.json
echo "Test 1: PASS"

# ... 更多测试用例
```

## 性能基准

### 指标收集

```bash
# 记录首次运行时间
time geoforge-converter --format osgb ... --output /tmp/bench_full

# 记录恢复时间(50% 完成后)
time geoforge-converter --format osgb ... --output /tmp/bench_resume --resume

# 记录指纹计算开销
# (从日志中提取时间戳)
```

### 预期基准
- 指纹计算: < 1% 总耗时
- Manifest 写入: < 0.5% 总耗时
- 输出验证: < 2% 总耗时
- 恢复启动开销: < 5s (100 Block 项目)

## CI 集成

### GitHub Actions 工作流

```yaml
name: Test P3 Resume

on: [push, pull_request]

jobs:
  test-resume:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Setup dependencies
        run: |
          # 安装 vcpkg, OSG, GDAL 等
      
      - name: Build converter
        run: cargo build --release
      
      - name: Download test data
        run: |
          # 下载或生成小型测试数据
      
      - name: Run resume tests
        run: |
          bash tests/integration/test_resume.sh
```

## 故障排查

### 常见问题

1. **"Manifest params hash mismatch" 但参数未变**
   - 检查隐式参数(如环境变量)
   - 验证参数哈希算法是否稳定

2. **"Block changed or invalid output" 但文件未修改**
   - 检查文件系统时间戳精度
   - 考虑使用完整内容哈希

3. **恢复后 tileset.json 不完整**
   - 验证所有 Block 状态为 succeeded
   - 检查 build_root_tileset 逻辑

4. **崩溃后无法认领 Block**
   - 确认输出目录存在且有效
   - 检查 validate_block_output 逻辑

### 调试技巧

```bash
# 启用详细日志
RUST_LOG=debug geoforge-converter ... --resume

# 检查 manifest 状态
jq '.blocks[] | {id, status, attemptCount}' \
  /tmp/output/block_manifest.json

# 验证输出完整性
for dir in /tmp/output/Data/*/; do
  block=$(basename "$dir")
  if [ ! -f "$dir/tileset.json" ]; then
    echo "Missing tileset: $block"
  fi
done

# 计算指纹(手动验证)
stat tests/fixtures/osgb_medium/Data/Tile_001/Tile_001.osgb
head -c 8192 tests/fixtures/osgb_medium/Data/Tile_001/Tile_001.osgb | sha256sum
```

## 下一步

完成测试后需要:
1. 修复发现的 bug
2. 优化性能瓶颈
3. 补充单元测试
4. 集成到主程序调用链
5. 更新用户文档
