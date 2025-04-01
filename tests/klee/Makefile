# KLEE 测试 Makefile

# 基本变量
CC = clang
KLEE = klee
CFLAGS = -g -O0 -Xclang -disable-O0-optnone -emit-llvm -c

# 项目源代码路径
SRC_DIR = ../../src

# 输出路径
OUT_DIR = ./out

# 测试源文件
TEST_SRC = klee_genkwcrcs_test.c

# 目标文件
TEST_BC = $(OUT_DIR)/klee_genkwcrcs_test.bc

# 默认目标
.PHONY: all
all: prepare test

# 准备输出目录
.PHONY: prepare
prepare:
	mkdir -p $(OUT_DIR)

# 编译成LLVM字节码
$(TEST_BC): $(TEST_SRC)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $< -o $@

# 运行KLEE符号执行
.PHONY: test
test: $(TEST_BC)
	$(KLEE) --emit-all-errors $<

# 运行KLEE特定测试
.PHONY: test-hash
test-hash: $(TEST_BC)
	$(KLEE) --emit-all-errors --entry-point=test_crc_not_zero $<

.PHONY: test-identifier
test-identifier: $(TEST_BC)
	$(KLEE) --emit-all-errors --entry-point=test_identifier_generation $<

.PHONY: test-collision
test-collision: $(TEST_BC)
	$(KLEE) --emit-all-errors --entry-point=test_collision_detection $<

# 查看KLEE测试结果
.PHONY: show-results
show-results:
	ktest-tool $(shell find klee-last -name "*.ktest")

# 清理生成的文件
.PHONY: clean
clean:
	rm -rf $(OUT_DIR) klee-* *.log

# 安装KLEE依赖项（Ubuntu/Debian）
.PHONY: install-deps
install-deps:
	@echo "安装KLEE依赖项..."
	@echo "请以root权限运行以下命令："
	@echo "apt-get update"
	@echo "apt-get install build-essential cmake llvm-dev llvm-tools clang python3 python3-pip"
	@echo "pip3 install tabulate wllvm"

# 使用说明
.PHONY: help
help:
	@echo "KLEE测试Makefile"
	@echo ""
	@echo "用法:"
	@echo "  make              - 编译并运行所有KLEE测试"
	@echo "  make test-hash    - 运行哈希值测试"
	@echo "  make test-identifier - 运行标识符生成测试"
	@echo "  make test-collision - 运行哈希冲突测试"
	@echo "  make show-results - 显示KLEE测试结果"
	@echo "  make clean        - 清理生成的文件"
	@echo "  make install-deps - 显示安装KLEE依赖的命令"
	@echo "  make help         - 显示此帮助信息" 