# KLEE测试Makefile

# 编译器设置
CC = clang
KLEE = klee

# 编译标志
CFLAGS = -I../../../src -emit-llvm -c -g -O0 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -DCFG_linux=1 -DCFG_flashsim=1
LDFLAGS = 

# 源文件
TEST_SRC = klee_fs_test.c
FS_SRC = ../../../src/fs.c
TEST_BC = klee_fs_test.bc
FS_BC = fs.bc
LINKED_BC = linked.bc

# 目标
.PHONY: all clean help test

all: $(LINKED_BC)

# 编译测试文件
$(TEST_BC): $(TEST_SRC)
	$(CC) $(CFLAGS) $< -o $@

# 编译文件系统源文件
$(FS_BC): $(FS_SRC)
	$(CC) $(CFLAGS) $< -o $@

# 链接生成的字节码文件
$(LINKED_BC): $(TEST_BC) $(FS_BC)
	llvm-link $^ -o $@

# 运行KLEE测试
test: $(LINKED_BC)
	$(KLEE) --libc=uclibc --posix-runtime $<

# 清理生成的文件
clean:
	rm -f *.bc
	rm -f *.o
	rm -rf klee-*

# 帮助信息
help:
	@echo "KLEE文件系统测试Makefile"
	@echo ""
	@echo "可用目标："
	@echo "  all     - 编译所有源文件"
	@echo "  test    - 运行KLEE符号执行测试"
	@echo "  clean   - 清理生成的文件"
	@echo "  help    - 显示此帮助信息"
	@echo ""
	@echo "使用方法："
	@echo "  make        - 编译所有文件"
	@echo "  make test   - 运行KLEE测试"
	@echo "  make clean  - 清理文件" 