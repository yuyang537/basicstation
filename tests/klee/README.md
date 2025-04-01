# KLEE 符号执行测试

本目录包含用于对 `genkwcrcs.c` 文件进行符号执行测试的代码和配置。这些测试旨在验证程序的关键安全属性。

## 目录结构

```
klee/
├── Makefile              # 构建和运行测试的Makefile
├── README.md             # 本说明文件
└── klee_genkwcrcs_test.c # KLEE测试源代码
```

## 测试内容

KLEE测试验证以下安全属性：

1. **哈希值不为零**：验证所有输入字符串生成的CRC哈希值不为零
2. **标识符合规性**：验证生成的C语言标识符符合命名规范
3. **哈希冲突检测**：探索可能的哈希冲突（注意：此测试可能会找到反例）

## 安装KLEE

要运行这些测试，您需要安装KLEE及其依赖项。

### 使用Docker（推荐）

```bash
docker pull klee/klee
docker run --rm -ti --ulimit='stack=-1:-1' -v $(pwd):/home/klee/src klee/klee
```

### 手动安装（Ubuntu/Debian）

```bash
# 安装依赖
apt-get update
apt-get install build-essential cmake llvm-dev llvm-tools clang python3 python3-pip

# 安装KLEE
git clone https://github.com/klee/klee.git
cd klee
mkdir build
cd build
cmake ..
make
make install
```

## 运行测试

在安装KLEE后，按照以下步骤运行测试：

1. 进入测试目录：
   ```bash
   cd tests/klee
   ```

2. 编译和运行所有测试：
   ```bash
   make
   ```

3. 运行特定测试：
   ```bash
   make test-hash         # 测试哈希值不为零
   make test-identifier   # 测试标识符生成
   make test-collision    # 测试哈希冲突（可能会失败）
   ```

4. 查看测试结果：
   ```bash
   make show-results
   ```

5. 清理生成的文件：
   ```bash
   make clean
   ```

## 测试结果解读

KLEE会生成测试用例和错误报告：

- **测试通过**：如果没有发现违反断言的情况
- **测试失败**：如果KLEE找到了违反断言的输入

错误报告位于 `klee-last/test*.err` 文件中，包含违反断言的具体输入。

## 符号执行的限制

请注意以下符号执行的限制：

1. 状态爆炸问题：对于复杂函数，KLEE可能无法探索所有可能路径
2. 环境模拟：KLEE无法模拟外部环境（如文件系统、网络）
3. 浮点运算：KLEE对浮点运算的支持有限

## 修改测试

要添加新的测试或修改现有测试：

1. 编辑 `klee_genkwcrcs_test.c` 文件
2. 添加新的测试函数
3. 在 `main()` 函数中调用新的测试函数
4. 更新 `Makefile` 添加新的测试目标 