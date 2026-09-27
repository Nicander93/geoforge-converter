# P2 测试指南

## 前置条件

### 环境要求

1. **完整编译环境**
   ```bash
   # vcpkg 依赖
   vcpkg install osg gdal proj
   
   # Rust 工具链
   rustc --version  # >= 1.83
   cargo --version
   ```

2. **测试数据**
   - 小规模：4×4 fixture（已有）
   - 中等规模：16 个 OSGB Block
   - 大规模：100+ Block（内存测试）

### 基线对照

```bash
# 编译 P1 (基线)
git checkout 8303ff9
cargo build --release
cp target/release/_3dtile _3dtile_baseline

# 编译 P2
git checkout cursor/p2-streaming-block-results-40db
cargo build --release
cp target/release/_3dtile _3dtile_p2
```

## 测试清单

### 1. 功能正确性

#### 1.1 基本转换

**目标**：验证 P2 输出与 P1 结构等价

```bash
# 运行基线
_3dtile_baseline -f osgb -i tests/fixtures/4x4 -o output_baseline

# 运行 P2
_3dtile_p2 -f osgb -i tests/fixtures/4x4 -o output_p2

# 比较结构
diff -r output_baseline output_p2 --exclude=block_manifest.json

# 预期：除 block_manifest.json 外，所有文件内容一致
# （JSON 格式可能有微小差异，如空格，但语义相同）
```

**验证点**：
- [ ] `tileset.json` 存在且有效
- [ ] `Data/` 下所有 Block 目录存在
- [ ] 每个 Block 有 `tileset.json`
- [ ] GLB 文件存在且大小合理
- [ ] 边界框和几何误差值相近

#### 1.2 Block Manifest

```bash
# 检查新文件
cat output_p2/block_manifest.json | jq .

# 验证 JSON 格式
jq '.version == 1 and (.blocks | length) == 16' output_p2/block_manifest.json

# 验证 Block ID 排序
jq -r '.blocks[].id' output_p2/block_manifest.json | sort -c
```

**验证点**：
- [ ] `block_manifest.json` 存在
- [ ] version 字段为 1
- [ ] blocks 数组长度等于实际 Block 数
- [ ] Block ID 按字典序排序
- [ ] 每个 Block 有完整字段（id, path, boundingBox, geometricError, stats）

#### 1.3 稳定排序

**目标**：验证并发下的顺序一致性

```bash
# 多次运行
for i in {1..5}; do
  rm -rf output_run$i
  _3dtile_p2 -f osgb -i tests/fixtures/4x4 -o output_run$i
  jq -r '.blocks[].id' output_run$i/block_manifest.json > ids_$i.txt
done

# 比较所有运行的 ID 顺序
diff ids_1.txt ids_2.txt
diff ids_1.txt ids_3.txt
# ...
```

**预期**：所有运行的 Block ID 顺序完全相同

### 2. 内存占用

#### 2.1 小规模 vs 大规模

```bash
# 小规模（16 Block）
/usr/bin/time -v _3dtile_p2 -f osgb -i data/16blocks -o output_16 2>&1 | tee mem_16.log
grep "Maximum resident set size" mem_16.log

# 大规模（100 Block）
/usr/bin/time -v _3dtile_p2 -f osgb -i data/100blocks -o output_100 2>&1 | tee mem_100.log
grep "Maximum resident set size" mem_100.log

# 计算增长比例
python3 << EOF
import re
mem_16 = int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', open('mem_16.log').read()).group(1))
mem_100 = int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', open('mem_100.log').read()).group(1))
ratio = mem_100 / mem_16
blocks_ratio = 100 / 16
print(f"Memory ratio: {ratio:.2f}x")
print(f"Blocks ratio: {blocks_ratio:.2f}x")
print(f"Memory growth: {'PASS' if ratio < blocks_ratio * 0.5 else 'FAIL'}")
EOF
```

**预期**：
- 内存增长显著低于 Block 数量增长
- P2 应接近 O(N) 而非 O(N × JSON大小)

#### 2.2 基线对比

```bash
# P1 baseline
/usr/bin/time -v _3dtile_baseline -f osgb -i data/100blocks -o output_baseline 2>&1 | grep "Maximum resident"

# P2
/usr/bin/time -v _3dtile_p2 -f osgb -i data/100blocks -o output_p2 2>&1 | grep "Maximum resident"
```

**预期**：P2 内存占用显著低于 P1（理论上可降低 90%+）

### 3. 错误处理

#### 3.1 单 Block 失败

```bash
# 损坏一个输入文件
cp -r data/16blocks data/16blocks_corrupted
echo "corrupted" > data/16blocks_corrupted/Data/Tile_005_005/Tile_005_005.osgb

# 运行转换
_3dtile_p2 -f osgb -i data/16blocks_corrupted -o output_corrupted 2>&1 | tee error.log

# 验证
echo "Exit code: $?"
```

**验证点**：
- [ ] 退出码非零
- [ ] 错误日志包含 "Block Tile_005_005 failed"
- [ ] `output_corrupted/Data/` 下有成功的 Block 目录
- [ ] 失败 Block 只有 `Tile_005_005_staging` 或不存在
- [ ] `block_manifest.json` 不包含失败 Block

#### 3.2 磁盘写满模拟

```bash
# 创建小文件系统
dd if=/dev/zero of=small_fs.img bs=1M count=10
mkfs.ext4 small_fs.img
mkdir /tmp/small_mount
mount -o loop small_fs.img /tmp/small_mount

# 尝试转换到满盘
_3dtile_p2 -f osgb -i tests/fixtures/4x4 -o /tmp/small_mount/output 2>&1 | tee disk_full.log

# 验证
umount /tmp/small_mount
```

**预期**：
- 写入失败不产生半成品 Block（只有 staging 或不存在）
- 错误信息清晰

### 4. 并发与取消

#### 4.1 线程配置

```bash
# 不同线程数
for threads in 1 2 4 8; do
  GEOFORGE_CONVERT_THREADS=$threads _3dtile_p2 -f osgb \
    -i tests/fixtures/4x4 -o output_t$threads 2>&1 | tee log_t$threads.txt
  
  grep "queue_capacity" log_t$threads.txt
done
```

**验证点**：
- [ ] threads=1 → queue_capacity=4 (最小值)
- [ ] threads=2 → queue_capacity=4
- [ ] threads=4 → queue_capacity=8
- [ ] threads=8 → queue_capacity=16

#### 4.2 取消信号

```bash
# 后台启动
_3dtile_p2 -f osgb -i data/100blocks -o output_cancel 2>&1 | tee cancel.log &
PID=$!

# 等待部分完成
sleep 10

# 发送 SIGTERM
kill $PID
wait $PID
echo "Exit code: $?"

# 验证
ls output_cancel/Data/ | grep -E "_staging|^Tile"
```

**预期**：
- 进程能正常退出
- 已完成的 Block 目录正常
- 未完成的可能有 staging 或不存在
- 无僵尸进程或泄漏资源

### 5. 性能与日志

#### 5.1 转换速度

```bash
# 运行并计时
time _3dtile_p2 -f osgb -i data/16blocks -o output_perf

# 查看详细日志
grep "completed successfully" output_perf.log | wc -l
```

**预期**：
- 总时间与 P1 相近（±10%）
- Block 完成日志数量等于实际 Block 数

#### 5.2 日志完整性

```bash
_3dtile_p2 -f osgb -i tests/fixtures/4x4 -o output 2>&1 | tee full.log

# 检查关键日志
grep "OSGB conversion config" full.log
grep "Block .* completed successfully" full.log
grep "OSGB conversion completed successfully" full.log
```

**验证点**：
- [ ] 配置日志包含 blocks, threads, queue_capacity
- [ ] 每个成功 Block 有完成日志
- [ ] 最终总结日志存在

### 6. 兼容性

#### 6.1 FBX/OBJ 路径

```bash
# 确保 P2 不影响其他格式
_3dtile_p2 -f fbx -i tests/fixtures/model.fbx -o output_fbx \
  --model-config tests/fixtures/model_config.json

# 验证
test -f output_fbx/tileset.json && echo "PASS" || echo "FAIL"
```

**预期**：FBX/OBJ 转换完全正常，不受影响

#### 6.2 环境变量

```bash
# 测试所有相关环境变量
_3dtile_p2 --show-env-help
_3dtile_p2 --capabilities-json | jq .
```

**预期**：
- 帮助信息完整
- 能力 JSON 包含 OSGB 相关字段
- 无新的破坏性变更

### 7. 边界情况

#### 7.1 空输入

```bash
mkdir -p empty_data/Data
_3dtile_p2 -f osgb -i empty_data -o output_empty
echo "Exit code: $?"
```

**预期**：退出码非零，错误信息明确

#### 7.2 单 Block

```bash
# 只有一个 Block
_3dtile_p2 -f osgb -i data/single_block -o output_single

# 验证
jq '.blocks | length' output_single/block_manifest.json
```

**预期**：正常工作，manifest 包含 1 个 Block

#### 7.3 大量小 Block

```bash
# 1000 个小 Block（如果有此测试数据）
_3dtile_p2 -f osgb -i data/1000_tiny_blocks -o output_many
```

**预期**：
- 不崩溃
- 内存占用仍为 O(N)
- 队列限流生效

## 回归测试矩阵

| 场景 | P1 基线 | P2 | 状态 |
|------|---------|----|----|
| 4×4 fixture 结构等价 | ✓ | ? | 待测 |
| 16 Block 输出正确 | ✓ | ? | 待测 |
| 100 Block 内存占用 | 高 | 低 | 待测 |
| Block 失败不影响其他 | ✗ | ✓ | 待测 |
| 取消信号响应 | ? | ✓ | 待测 |
| 稳定排序 | N/A | ✓ | 待测 |

## 自动化测试脚本

```bash
#!/bin/bash
# test_p2.sh - P2 自动化测试套件

set -e

echo "=== P2 测试套件 ==="

# 1. 功能测试
echo "[1/6] 功能正确性..."
_3dtile_p2 -f osgb -i tests/fixtures/4x4 -o output_p2
test -f output_p2/tileset.json && echo "✓ Root tileset 存在"
test -f output_p2/block_manifest.json && echo "✓ Block manifest 存在"
jq -e '.version == 1' output_p2/block_manifest.json > /dev/null && echo "✓ Manifest 格式正确"

# 2. 稳定排序
echo "[2/6] 稳定排序..."
for i in {1..3}; do
  rm -rf output_run$i
  _3dtile_p2 -f osgb -i tests/fixtures/4x4 -o output_run$i
done
diff <(jq -r '.blocks[].id' output_run1/block_manifest.json) \
     <(jq -r '.blocks[].id' output_run2/block_manifest.json) && echo "✓ 排序稳定"

# 3. 内存测试（需要大数据集）
if [ -d "data/100blocks" ]; then
  echo "[3/6] 内存占用..."
  /usr/bin/time -v _3dtile_p2 -f osgb -i data/100blocks -o output_100 2>&1 | \
    grep "Maximum resident" && echo "✓ 内存测试完成"
else
  echo "[3/6] 跳过内存测试（无大数据集）"
fi

# 4. 错误处理
echo "[4/6] 错误处理..."
# （需要准备损坏的测试数据）
echo "✓ 跳过（需手动验证）"

# 5. 并发配置
echo "[5/6] 并发配置..."
GEOFORGE_CONVERT_THREADS=2 _3dtile_p2 -f osgb -i tests/fixtures/4x4 -o output_t2 2>&1 | \
  grep "queue_capacity=4" && echo "✓ 队列容量正确"

# 6. 兼容性
echo "[6/6] 向后兼容..."
_3dtile_p2 --capabilities-json | jq -e '.formats[] | select(. == "osgb")' > /dev/null && \
  echo "✓ 能力 JSON 兼容"

echo ""
echo "=== 所有测试完成 ==="
```

## 性能基准

### 预期指标（参考）

| 数据集 | Block 数 | P1 内存 | P2 内存 | P1 时间 | P2 时间 |
|-------|---------|---------|---------|---------|---------|
| 4×4 | 16 | ~50 MB | ~5 MB | 10s | 10s |
| Medium | 64 | ~200 MB | ~15 MB | 40s | 40s |
| Large | 256 | ~800 MB | ~50 MB | 160s | 160s |

（实际数值依赖硬件和数据特征）

## 故障排查

### 常见问题

1. **编译失败**
   ```
   error: failed to download `clap_lex v1.1.1`
   ```
   → 清理 Cargo 缓存：`rm -rf ~/.cargo/registry`

2. **运行时崩溃**
   ```
   Segmentation fault
   ```
   → 检查 C++ 依赖（OSG, GDAL）版本
   → 查看 P1 线程安全修复是否生效

3. **Block 完成但无输出**
   → 检查 staging 目录权限
   → 查看 rename 是否成功（跨文件系统不支持）

4. **内存占用仍然很高**
   → 确认使用的是 P2 版本二进制
   → 检查 C++ 层是否有内存泄漏

## 提交前检查

- [ ] 所有功能测试通过
- [ ] 内存占用显著降低（如有大数据集）
- [ ] 稳定排序验证通过
- [ ] 错误场景不留半成品
- [ ] FBX/OBJ 路径不受影响
- [ ] 日志输出完整
- [ ] 文档已更新

## 反馈

测试结果请记录到：
- `docs/P2_TEST_RESULTS.md`（新建）
- 或 PR 评论

包含：
- 测试环境（OS, CPU, RAM）
- 数据集规模
- 测试结果截图
- 性能对比数据
