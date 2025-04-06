# KLEE测试构建文件
# ================

# 编译器设置
CC = clang
CFLAGS = -I../../src -emit-llvm -c -g -O0 -Xclang -disable-O0-optnone

# 源文件
SRCS = klee_fs_test.c
OBJS = $(SRCS:.c=.bc)

# 默认目标
all: $(OBJS)
	klee --libc=uclibc --posix-runtime $(OBJS)

# 编译规则
%.bc: %.c
	$(CC) $(CFLAGS) $< -o $@

# 清理
clean:
	rm -f *.bc
	rm -rf klee-out-*

# 帮助信息
help:
	@echo "KLEE测试构建系统"
	@echo "用法:"
	@echo "  make        - 编译并运行所有测试"
	@echo "  make clean  - 清理生成的文件"
	@echo "  make help   - 显示此帮助信息"

.PHONY: all clean help 