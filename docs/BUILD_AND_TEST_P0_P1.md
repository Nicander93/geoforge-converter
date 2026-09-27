# 构建和测试说明 - P0+P1

## 快速验证

### 1. 语法检查（不需要完整构建环境）

```bash
# 检查Rust代码语法
cargo check --lib

# 检查C++头文件语法（需要C++编译器）
g++ -std=c++11 -fsyntax-only -I. src/osgb23dtile.cpp
```

### 2. 查看代码变更

```bash
# 查看所有更改
git diff master...cursor/p0-p1-converter-thread-safety-30f1

# 只查看C++线程安全修复
git diff master...cursor/p0-p1-converter-thread-safety-30f1 -- src/osgb23dtile.cpp

# 只查看Rust能力报告增强
git diff master...cursor/p0-p1-converter-thread-safety-30f1 -- src/main.rs
```

## 完整构建（需要Windows + vcpkg）

### 前置条件

1. **Windows 10/11 x64**
2. **Visual Studio 2019/2022** (MSVC)
3. **CMake** 3.15+
4. **vcpkg** 安装并设置 `VCPKG_ROOT`
5. **Rust** 1.70+

### 构建步骤

```powershell
# 1. 克隆仓库
git clone https://github.com/Nicander93/geoforge-converter.git
cd geoforge-converter

# 2. 切换到PR分支
git checkout cursor/p0-p1-converter-thread-safety-30f1

# 3. 更新子模块
git submodule update --init --recursive

# 4. 设置vcpkg
$env:VCPKG_ROOT = "C:\vcpkg"  # 你的vcpkg路径

# 5. 构建
cargo build --release

# 构建产物在 target/release/_3dtile.exe
```

## 功能测试

### 测试1: 能力报告

```bash
# 应该输出包含 converterVersion: "0.2.2" 的JSON
_3dtile --capabilities-json
```

预期输出片段：
```json
{
  "version": 1,
  "converterVersion": "0.2.2",
  "converterName": "geoforge-converter",
  "formats": ["osgb", "fbx", "obj"],
  "features": {
    "osgb": {
      "supported": true,
      "parallel": true,
      "threadControl": "GEOFORGE_CONVERT_THREADS",
      "defaultThreads": "available_parallelism/2, clamped 1-8"
    }
  }
}
```

### 测试2: 环境变量帮助

```bash
_3dtile --show-env-help
```

预期输出：
```
GeoForge Converter Environment Variables:

GEOFORGE_CONVERT_THREADS
  Controls OSGB parallel conversion worker threads.
  Value: positive integer
  Default: available_parallelism/2, clamped to 1-8
  Example: GEOFORGE_CONVERT_THREADS=4
...
```

### 测试3: 版本信息

```bash
_3dtile --version
```

预期输出：
```
Make 3dtile program 0.2.2
```

### 测试4: 实际转换（增强日志）

```bash
# Windows
set GEOFORGE_CONVERT_THREADS=4
_3dtile -f osgb -i D:\data\project -o D:\output

# Linux/Mac
GEOFORGE_CONVERT_THREADS=4 _3dtile -f osgb -i /data/project -o /output
```

预期日志包含：
```
INFO - GeoForge converter v0.2.2 starting OSGB conversion
INFO - Input: D:\data\project, Output: D:\output
INFO - OSGB conversion config: threads=4, max_lvl=100, texture_compress=false, ...
INFO - OSGB conversion completed successfully in 123.45s
```

## 线程安全验证

### 方法1: ThreadSanitizer (Linux/Mac)

```bash
# 使用ThreadSanitizer构建
RUSTFLAGS="-Z sanitizer=thread" cargo +nightly build --target x86_64-unknown-linux-gnu

# 运行并检查数据竞争
GEOFORGE_CONVERT_THREADS=8 ./target/x86_64-unknown-linux-gnu/debug/_3dtile -f osgb -i large_dataset -o output

# 应该没有 "WARNING: ThreadSanitizer: data race" 报告
```

### 方法2: 压力测试

```bash
# 使用最大线程数运行多次
for i in {1..10}; do
    GEOFORGE_CONVERT_THREADS=16 _3dtile -f osgb -i test_data -o output_$i
    echo "Run $i completed"
done

# 检查所有输出是否一致
# 日志中 "=== OpenSceneGraph Plugin Loading Information ===" 应该只出现一次
```

### 方法3: 手动代码审查

检查点：
1. ✅ `src/osgb23dtile.cpp:411` - `get_all_tree()` 使用 `std::call_once`
2. ✅ `src/osgb23dtile.cpp:1118` - `osgb2glb_buf()` 使用 `std::call_once`
3. ✅ 包含 `#include <mutex>` 头文件
4. ✅ 每处使用独立的 `std::once_flag`

## 回归测试

确保现有功能未被破坏：

### 单线程转换
```bash
GEOFORGE_CONVERT_THREADS=1 _3dtile -f osgb -i test_small -o output_single
```

### 各种压缩选项
```bash
# Draco压缩
_3dtile -f osgb -i test -o output1 --enable-draco

# 纹理压缩
_3dtile -f osgb -i test -o output2 --enable-texture-compress

# 组合
_3dtile -f osgb -i test -o output3 --enable-draco --enable-texture-compress --enable-unlit
```

### FBX/OBJ转换（确保路径完整）
```bash
# FBX
_3dtile -f fbx -i model.fbx -o output --model-config config.json

# OBJ
_3dtile -f obj -i model.obj -o output --model-config config.json
```

## CI测试

PR创建后，GitHub Actions应该自动运行：
- Windows构建
- 单元测试（如果有）
- 发布包验证

检查 https://github.com/Nicander93/geoforge-converter/pull/2/checks

## 常见问题

### Q: 构建失败 "VCPKG_ROOT not set"
**A:** 设置环境变量 `VCPKG_ROOT` 指向你的vcpkg安装路径

### Q: 链接错误找不到OSG库
**A:** 确保vcpkg已安装所有依赖：
```bash
vcpkg install osg gdal proj eigen3 nlohmann-json spdlog
```

### Q: Rust版本太旧
**A:** 更新Rust：
```bash
rustup update stable
```

### Q: 如何只测试Rust部分不构建C++？
**A:** 这个项目C++和Rust紧密耦合，必须一起构建。但可以：
```bash
# 只检查Rust语法
cargo check
```

## 最小验证方案

如果没有完整的构建环境，可以通过以下方式验证：

1. **代码审查**: 在GitHub PR界面查看diff
2. **静态分析**: 使用在线工具检查C++语法
3. **逻辑验证**: 阅读代码确认 `std::call_once` 正确使用
4. **文档检查**: 验证所有声称的功能都有对应代码

## 需要帮助？

- 查看 PR #2: https://github.com/Nicander93/geoforge-converter/pull/2
- 查看变更记录: `docs/CHANGELOG_P0_P1.md`
- 查看实施总结: `docs/P0_P1_IMPLEMENTATION_SUMMARY.md`
