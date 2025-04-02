# --- Revised 3-Clause BSD License ---
# Copyright Semtech Corporation 2022. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
#     * Redistributions of source code must retain the above copyright notice,
#       this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice,
#       this list of conditions and the following disclaimer in the documentation
#       and/or other materials provided with the distribution.
#     * Neither the name of the Semtech corporation nor the names of its
#       contributors may be used to endorse or promote products derived from this
#       software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION. BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

TD=.
include ${TD}/setup.gmk

.PHONY: all
all:	build-local/bin/crc32 \
	src/kwcrc.h \
	deps \
	s-all \
	klee-test

# Shortcuts to run station specific goals
.PHONY: s-all s-load s-clean
s-all s-load s-clean: ${BD}/s2core/makefile
	${MAKE} -C ${<D} ${@:s-%=%}

${BD}/%/makefile : makefile.%
	mkdir -p ${@D} ${BD}/bin ${BD}/lib
	cd ${@D} && (	echo "platform=${platform}"; \
			echo "variant=${variant}"; \
			echo "TD=../.."; \
			echo "-include ../../makefile.${platform}"; \
			echo "include ../../makefile.s2core") > makefile

src/kwcrc.h: build-local/bin/genkwcrcs src/kwlist.txt
	build-local/bin/genkwcrcs $$(cat src/kwlist.txt | sed -e '/^#/d;s/[ \t]\+#.*//') > build-local/temp-kwcrc.h
	mv build-local/temp-kwcrc.h $@

build-local/bin/genkwcrcs: src/genkwcrcs.c src/uj.h
	mkdir -p ${@D}
	gcc -std=gnu11 -Isrc -DCFG_prog_genkwcrcs $< -o $@

build-local/bin/crc32: src/crc32.c
	mkdir -p ${@D}
	gcc -std=gnu11 -Isrc -DCFG_prog_crc32 $< -o $@

DEPS.goals = $(patsubst %, deps/%, ${DEPS})

.PHONY: deps ${DEPS.goals}
deps: ${DEPS.goals}

${DEPS.goals}:
	platform=${platform} variant=${variant} ${MAKE} -C $@

.PHONY: build-clean
clean-build:
	for d in build-*/s2core; do \
	  if [ -d $$d ]; then ${MAKE} -C $$d clean; fi \
	done

.PHONY: clean super-clean
clean super-clean: clean-build
	for d in deps/*; do \
	  ${MAKE} -C $$d $@; \
	done

# KLEE 测试配置
KLEE_CFLAGS = -emit-llvm -c -g -O0 -Wall -I./src -I./deps/mbedtls/platform-linux/include
KLEE_LDFLAGS = -emit-llvm
KLEE_SOURCES = $(wildcard src/*.c)
KLEE_BC_FILES = $(KLEE_SOURCES:src/%.c=build-local/bin/%.bc)
KLEE_TEST_BC = build-local/bin/klee-tests.bc

# KLEE 测试目标
.PHONY: klee-test
klee-test: deps $(KLEE_TEST_BC)
	klee --libc=uclibc --posix-runtime $(KLEE_TEST_BC)

# 编译源文件为 LLVM bitcode
build-local/bin/%.bc: src/%.c
	@mkdir -p build-local/bin
	clang $(KLEE_CFLAGS) $< -o $@

# 链接所有 bitcode 文件
$(KLEE_TEST_BC): $(KLEE_BC_FILES)
	llvm-link $(KLEE_BC_FILES) -o $@

# 清理 KLEE 测试文件
.PHONY: clean-klee
clean-klee:
	rm -f $(KLEE_BC_FILES) $(KLEE_TEST_BC)
	rm -f klee-last
	rm -rf klee-out-*

# 更新 all 目标
all: $(BINS) klee-test

# 更新 clean 目标
clean: clean-klee
