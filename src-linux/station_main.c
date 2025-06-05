/*
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *       this list of conditions and the following disclaimer in the documentation
 *       and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the names of its
 *       contributors may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION. BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Linux平台条件编译：确保此代码仅在Linux平台上编译
#if defined(CFG_linux)

// 声明sys_main函数，这是程序的实际入口点
// sys_main函数在sys_linux.c中实现，包含所有主要的初始化逻辑
extern int sys_main (int argc, char** argv);

/**
 * 程序主入口点
 * 这是LoRa Basics Station程序启动的第一个函数
 * 
 * @param argc 命令行参数个数
 * @param argv 命令行参数字符串数组
 * @return 程序退出状态码：0表示成功，非0表示错误
 * 
 * 功能说明：
 * - 这个main函数只是一个简单的包装器
 * - 实际的程序逻辑都在sys_main函数中实现
 * - 这种设计允许在不同平台上有不同的main函数实现
 */
int main (int argc, char** argv) {
    // 直接调用sys_main函数并返回其结果
    // sys_main包含所有平台相关的初始化代码和主要业务逻辑
    return sys_main(argc, argv);
}

#endif // defined(CFG_linux)
