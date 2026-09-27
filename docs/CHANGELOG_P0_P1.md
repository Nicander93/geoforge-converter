# P0 + P1 实施记录：转换器版本报告与线程安全

实施日期: 2026-09-27  
分支: cursor/p0-p1-converter-thread-safety-30f1  
目标: P0 脚手架（版本/能力报告钩子）和 P1 线程安全修复（仅转换器）

## P0: 版本和能力报告

### 增强的能力查询 (`--capabilities-json`)

扩展了转换器的能力报告，现在包含：

```json
{
  "version": 1,
  "converterVersion": "0.2.2",
  "converterName": "geoforge-converter",
  "formats": ["osgb", "fbx", "obj"],
  "modelConfigVersion": 1,
  "georeferenceModes": ["local", "anchor", "projected"],
  "projectedGeoreference": true,
  "features": {
    "osgb": {
      "supported": true,
      "parallel": true,
      "threadControl": "GEOFORGE_CONVERT_THREADS",
      "defaultThreads": "available_parallelism/2, clamped 1-8"
    },
    "compression": {
      "draco": true,
      "ktx2": true,
      "meshopt": true
    },
    "extensions": {
      "KHR_draco_mesh_compression": true,
      "KHR_texture_basisu": true,
      "KHR_materials_unlit": true
    }
  }
}
```

### 新命令行参数

#### `--show-env-help`

显示环境变量文档：

```bash
_3dtile --show-env-help
```

输出：
```
GeoForge Converter Environment Variables:

GEOFORGE_CONVERT_THREADS
  Controls OSGB parallel conversion worker threads.
  Value: positive integer
  Default: available_parallelism/2, clamped to 1-8
  Example: GEOFORGE_CONVERT_THREADS=4

RUST_LOG
  Controls logging verbosity.
  Values: error, warn, info, debug, trace
  Default: info

OSG_LIBRARY_PATH
  OpenSceneGraph plugin search path (auto-detected).

GDAL_DATA, PROJ_DATA
  GDAL/PROJ data paths for coordinate transformations (auto-detected).
```

### 增强的运行时日志

OSGB 转换现在输出详细配置：

```
INFO - GeoForge converter v0.2.2 starting OSGB conversion
INFO - Input: D:\data\project, Output: D:\output
INFO - OSGB conversion config: threads=4, max_lvl=100, texture_compress=true, meshopt=false, draco=true, unlit=true
INFO - OSGB conversion completed successfully in 123.45s
```

### 版本信息

- 主程序版本现在使用 `CARGO_PKG_VERSION` (0.2.2)
- 帮助文本更新为 "GeoForge converter: fast OSGB/model to 3D Tiles conversion"

## P1: 线程安全修复

### 问题

`src/osgb23dtile.cpp` 中存在两处使用普通 `static bool logged` 的地方：
- 第411行：`get_all_tree()` 函数
- 第1118行：`osgb2glb_buf()` 函数

在多线程并发调用时，这些静态变量存在数据竞争风险。

### 解决方案

使用 C++11 `std::call_once` 和 `std::once_flag` 替代，确保初始化函数（`log_osg_plugin_info()`）仅被调用一次，且线程安全。

#### 修改前：
```cpp
static bool logged = false;
if (!logged) {
    log_osg_plugin_info();
    logged = true;
}
```

#### 修改后：
```cpp
static std::once_flag log_flag;
std::call_once(log_flag, []() {
    log_osg_plugin_info();
});
```

### 技术细节

- 添加 `#include <mutex>` 头文件
- 两处修改都使用了相同的模式
- 不需要额外的同步开销（`std::call_once` 是标准库提供的高效实现）
- 保持了原有的"只记录一次"的语义

## 行为不变性

### 对默认行为的影响

**无影响**。所有修改都是：
- **P0**: 添加新的可选查询命令和更详细的日志（不改变转换逻辑）
- **P1**: 修复线程安全问题（保持单线程行为不变，提高多线程安全性）

### FBX/OBJ 路径

**保持完整**。所有 FBX 和 OBJ 相关代码未被修改，能力报告中正确包含了这些格式。

## 构建和测试

### 构建命令

```bash
git checkout cursor/p0-p1-converter-thread-safety-30f1
git submodule update --init --recursive
cargo build --release
```

### 测试新功能

```bash
# 查询能力
./_3dtile --capabilities-json

# 查看环境变量帮助
./_3dtile --show-env-help

# 版本信息
./_3dtile --version

# 正常OSGB转换（观察增强的日志）
GEOFORGE_CONVERT_THREADS=4 ./_3dtile -f osgb -i input -o output
```

## 不包含的内容（按计划）

本阶段**不包含**：
- P2: 流式结果收取（仍保留现有的完全收集模式）
- P3: 恢复机制
- P4-P8: 其他优化

这些将在后续 PR 中实施。

## 下一步

1. 创建 PR 到 master
2. 确保 CI 通过
3. 合并后继续 P2（流式结果收取与 Block 原子提交）

## 验证清单

- [x] P1 线程安全：两处 static bool 改为 std::call_once
- [x] P0 能力报告：增强 --capabilities-json 输出
- [x] P0 帮助信息：添加 --show-env-help
- [x] P0 日志增强：版本、配置、时间统计
- [x] 不改变默认行为
- [x] FBX/OBJ 路径保持完整
- [x] 文档记录
