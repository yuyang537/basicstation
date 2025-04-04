# KLEE 测试 Makefile

# 编译工具和标志
CC = clang
CFLAGS = -I../../src -g -O0 -emit-llvm -c

# KLEE 命令和标志
KLEE = klee
KLEE_FLAGS = --emit-all-errors

# 测试文件
GENKWCRCS_TEST = klee_genkwcrcs_test
CRC32_TEST = klee_crc32_test

# 输出目录
OUT_DIR = ./out

# 默认目标
all: prepare $(OUT_DIR)/$(GENKWCRCS_TEST).bc $(OUT_DIR)/$(CRC32_TEST).bc

# 创建输出目录
prepare:
	mkdir -p $(OUT_DIR)

# 编译 genkwcrcs 测试
$(OUT_DIR)/$(GENKWCRCS_TEST).bc: $(GENKWCRCS_TEST).c
	$(CC) $(CFLAGS) $< -o $@

# 编译 crc32 测试
$(OUT_DIR)/$(CRC32_TEST).bc: $(CRC32_TEST).c
	$(CC) $(CFLAGS) $< -o $@

# 运行所有测试
test: test-genkwcrcs test-crc32

# 运行 genkwcrcs 测试
test-genkwcrcs: $(OUT_DIR)/$(GENKWCRCS_TEST).bc
	$(KLEE) $(KLEE_FLAGS) $<

# 运行 crc32 测试
test-crc32: $(OUT_DIR)/$(CRC32_TEST).bc
	$(KLEE) $(KLEE_FLAGS) $<

# genkwcrcs 特定测试
test-genkwcrcs-crc: $(OUT_DIR)/$(GENKWCRCS_TEST).bc
	$(KLEE) $(KLEE_FLAGS) --entry-point=test_crc_never_zero $<

test-genkwcrcs-ident: $(OUT_DIR)/$(GENKWCRCS_TEST).bc
	$(KLEE) $(KLEE_FLAGS) --entry-point=test_identifier_generation $<

test-genkwcrcs-collision: $(OUT_DIR)/$(GENKWCRCS_TEST).bc
	$(KLEE) $(KLEE_FLAGS) --entry-point=test_hash_collision $<

# crc32 特定测试
test-crc32-basic: $(OUT_DIR)/$(CRC32_TEST).bc
	$(KLEE) $(KLEE_FLAGS) --entry-point=test_crc32_basic $<

test-crc32-empty: $(OUT_DIR)/$(CRC32_TEST).bc
	$(KLEE) $(KLEE_FLAGS) --entry-point=test_crc32_empty_input $<

test-crc32-incremental: $(OUT_DIR)/$(CRC32_TEST).bc
	$(KLEE) $(KLEE_FLAGS) --entry-point=test_crc32_incremental $<

# 查看测试结果
show-results:
	ktest-tool $(shell find klee-last -name "*.ktest")

# 清理生成的文件
clean:
	rm -rf $(OUT_DIR) klee-*

# 安全属性说明文档
.PHONY: doc
doc:
	@echo "===== 安全属性说明 ====="
	@echo ""
	@echo "1. genkwcrcs.c 安全属性:"
	@echo "   - CRC非零: 哈希函数不会返回零值"
	@echo "   - 标识符生成: 生成的标识符始终是合法的C语言标识符"
	@echo "   - 哈希冲突: 不同的输入应该产生不同的哈希值"
	@echo ""
	@echo "2. crc32.c 安全属性:"
	@echo "   - 基本正确性: 多次计算相同输入得到相同结果"
	@echo "   - 空输入处理: 对空数据的处理正确且一致"
	@echo "   - 增量计算: 分段计算CRC与一次性计算结果一致"

# KLEE依赖项说明
help-deps:
	@echo "KLEE及其依赖项需要在系统中安装。"
	@echo "可以通过Docker使用KLEE，或按照KLEE官方文档进行安装:"
	@echo "https://klee.github.io/build-llvm11/"

.PHONY: all prepare test test-genkwcrcs test-crc32 clean doc help-deps 