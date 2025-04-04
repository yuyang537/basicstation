# KLEE符号执行测试

本目录包含针对项目中关键模块的KLEE符号执行测试，用于验证代码的关键安全属性。

## 概述

符号执行是一种软件分析技术，它使用符号化输入而不是具体值来执行程序，从而探索程序可能的执行路径，识别潜在的安全问题。在本项目中，我们使用KLEE工具进行符号执行测试。

## 测试模块

当前的测试关注以下关键模块：

1. **genkwcrcs.c** - 关键字CRC生成工具
   - 依赖最小的核心模块
   - 用于构建时生成kwcrc.h
   - 测试文件：`klee_genkwcrcs_test.c`

2. **crc32.c** - 标准CRC32计算工具
   - 独立的CRC32算法实现
   - 不依赖其他模块
   - 测试文件：`klee_crc32_test.c`

## 安全属性

### genkwcrcs.c 安全属性

1. **CRC非零属性**
   - 描述：哈希函数不会返回零值
   - 安全影响：确保每个关键字都有唯一有效的CRC值
   - 测试函数：`test_crc_never_zero()`

2. **标识符生成属性**
   - 描述：生成的标识符始终是合法的C语言标识符
   - 安全影响：防止生成不合法的C代码，避免编译错误和注入攻击
   - 测试函数：`test_identifier_generation()`

3. **哈希冲突属性**
   - 描述：不同的输入应该产生不同的哈希值
   - 安全影响：避免哈希碰撞导致的处理错误
   - 测试函数：`test_hash_collision()` (注：理论上冲突可能存在)

### crc32.c 安全属性

1. **基本正确性属性**
   - 描述：多次计算相同输入应得到相同结果
   - 安全影响：确保CRC计算的一致性和可靠性
   - 测试函数：`test_crc32_basic()`

2. **空输入处理属性**
   - 描述：对空数据的处理应该正确且一致
   - 安全影响：防止边界条件下的错误行为
   - 测试函数：`test_crc32_empty_input()`

3. **增量计算属性**
   - 描述：分段计算CRC与一次性计算结果一致
   - 安全影响：确保大数据处理和流式处理的正确性
   - 测试函数：`test_crc32_incremental()`

## 使用方法

### 编译测试

```bash
make
```

### 运行测试

运行所有测试：
```bash
make test
```

运行特定模块的测试：
```bash
make test-genkwcrcs  # 测试genkwcrcs模块
make test-crc32      # 测试crc32模块
```

运行特定安全属性测试：
```bash
# genkwcrcs测试
make test-genkwcrcs-crc       # CRC非零属性
make test-genkwcrcs-ident     # 标识符生成属性
make test-genkwcrcs-collision # 哈希冲突属性

# crc32测试
make test-crc32-basic       # 基本正确性属性
make test-crc32-empty       # 空输入处理属性
make test-crc32-incremental # 增量计算属性
```

### 查看测试结果

```bash
make show-results
```

### 清理生成的文件

```bash
make clean
```

## 注意事项

1. **KLEE依赖项**
   - KLEE需要预先安装
   - 可以使用Docker或按照官方文档安装
   - 运行`make help-deps`获取更多信息

2. **符号执行限制**
   - 某些复杂路径可能导致状态爆炸
   - 循环和递归可能需要限制执行深度
   - 哈希冲突测试可能找到理论上存在的反例

3. **结果解读**
   - 测试失败表示可能存在安全问题
   - 需检查测试用例的具体输入值
   - 非所有报错都表示实际漏洞，需结合代码上下文分析 