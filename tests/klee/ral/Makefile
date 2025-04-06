# KLEE测试构建配置 - RAL模块
# ============================

# 编译器设置
CC = clang
CFLAGS = -I../../src -I../../include -I../../../src -I../../../include -emit-llvm -c -g -O0 -Xclang -disable-O0-optnone

# 源文件
SRC = klee_ral_test.c
OBJ = $(SRC:.c=.bc)

# 默认目标
all: $(OBJ)
	klee --libc=uclibc --posix-runtime $(OBJ)

# 编译规则
%.bc: %.c
	$(CC) $(CFLAGS) $< -o $@

# 清理生成的文件
clean:
	rm -f *.bc
	rm -rf klee-out-*

# 帮助信息
help:
	@echo "KLEE测试构建系统 - RAL模块"
	@echo "用法:"
	@echo "  make        - 编译并运行所有测试"
	@echo "  make clean  - 清理生成的文件"
	@echo "  make help   - 显示此帮助信息" 