# KLEE测试Makefile
# =================

# 编译器和工具链设置
CC = clang
LLVM_LINK = llvm-link
KLEE = klee

# 编译选项
CFLAGS = -emit-llvm -c -g -O0 \
         -I../../../src \
         -I../../../src-linux \
         -I../../../deps/mbedtls/git-repo/include \
         -D_GNU_SOURCE \
         -D_POSIX_C_SOURCE=200809L \
         -D_XOPEN_SOURCE=700 \
         -DCFG_linux=1 \
         -DCFG_flashsim=1 \
         -DCFG_version=\"0.0.0-test\" \
         -DCFG_bdate=\"$(shell date +%Y-%m-%d)\"

# 源文件
TEST_SRC = klee_rt_test.c
SRC_FILES = ../../../src/rt.c \
            ../../../src/sys.c \
            ../../../src/log.c \
            ../../../src/s2conf.c \
            ../../../src/flashsim.c

LINUX_SRC_FILES = ../../../src-linux/sys_linux.c \
                  ../../../src-linux/sys_log.c

# 目标文件
TEST_OBJ = $(TEST_SRC:.c=.bc)
SRC_OBJS = $(SRC_FILES:.c=.bc)
LINUX_SRC_OBJS = $(LINUX_SRC_FILES:.c=.bc)

# 最终目标
TARGET = rt_test.bc

# 默认目标
all: $(TARGET)

# 链接目标文件
$(TARGET): $(TEST_OBJ) $(SRC_OBJS) $(LINUX_SRC_OBJS)
	$(LLVM_LINK) -o $@ $^

# 编译测试源文件
%.bc: %.c
	$(CC) $(CFLAGS) -o $@ $<

# 编译项目源文件
../../../src/%.bc: ../../../src/%.c
	$(CC) $(CFLAGS) -o $@ $<

# 编译Linux特定源文件
../../../src-linux/%.bc: ../../../src-linux/%.c
	$(CC) $(CFLAGS) -o $@ $<

# 运行KLEE测试
test: $(TARGET)
	$(KLEE) --libc=uclibc --posix-runtime \
		--max-memory=1000 \
		--max-time=3600 \
		--output-dir=klee-out \
		--optimize \
		--use-cex-cache \
		--use-branch-cache \
		--use-independent-solver \
		--use-forked-solver \
		--max-sym-array-size=4096 \
		--max-solver-time=30 \
		--max-static-fork-pct=1 \
		--max-static-solve-pct=1 \
		--max-static-cpfork-pct=1 \
		--switch-type=internal \
		--search=random-path \
		--use-batching-search \
		$(TARGET)

# 分析测试结果
analyze:
	@echo "分析KLEE测试结果..."
	@echo "===================="
	@echo "1. 测试统计:"
	@klee-stats klee-out
	@echo "\n2. 错误报告:"
	@find klee-out -name "test*.err" -exec cat {} \;
	@echo "\n3. 覆盖率报告:"
	@klee-coverage klee-out

# 清理目标
clean:
	rm -f *.bc *.ll
	rm -rf klee-out
	rm -f *.err
	rm -f *.o
	rm -f *.gcno *.gcda
	rm -f *.info

# 帮助信息
help:
	@echo "KLEE运行时测试Makefile"
	@echo "======================="
	@echo "可用目标:"
	@echo "  all     - 编译所有源文件"
	@echo "  test    - 运行KLEE符号执行测试"
	@echo "  analyze - 分析测试结果"
	@echo "  clean   - 清理编译产物"
	@echo "  help    - 显示此帮助信息"

.PHONY: all test analyze clean help 