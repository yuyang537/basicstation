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

#define _GNU_SOURCE  // syncfs(fd)
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "argp2.h"
#include "s2conf.h"
#include "kwcrc.h"
#include "rt.h"
#include "uj.h"
#include "s2e.h"
#include "ral.h"
#include "timesync.h"
#include "sys.h"
#include "sys_linux.h"
#include "fs.h"
#include "selftests.h"

#include "mbedtls/version.h"

extern char* makeFilepath (const char* prefix, const char* suffix, char** pCachedFile, int isReadable); // sys.c
extern int writeFile (str_t file, const char* data, int datalen);
extern dbuf_t readFile (str_t file, int complain);
extern str_t readFileAsString (str_t basename, str_t suffix, str_t* pCachedValue);
extern void setupConfigFilenames ();
extern int checkUris ();
extern void checkRollForward ();



#if defined(CFG_ral_master_slave)
static const char* const SLAVE_ENVS[] = {
    "SLAVE_IDX",
    "SLAVE_WRFD",
    "SLAVE_RDFD",
    NULL
};
#endif // defined(CFG_ral_master_slave)

static struct logfile logfile;
static char* gpsDevice    = NULL;
static tmr_t startupTmr;

str_t sys_slaveExec;
u1_t  sys_deviceMode;
u1_t  sys_modePPS;      // special mode?
u2_t  sys_webPort;
u1_t  sys_noTC;
u1_t  sys_noCUPS;

extern str_t  homeDir;
extern str_t  tempDir;
extern str_t  webDir;
static str_t  homeDirSrc;
static str_t  tempDirSrc;
static str_t  webDirSrc;

static int    daemonPid;
static int    workerPid;
static str_t  radioInit;
static str_t  radioDevice;
static str_t  versionTxt;
static char*  updfile;
static char*  temp_updfile;
static int    updfd = -1;

static str_t  protoEuiSrc;
static str_t  prefixEuiSrc;
static str_t  radioInitSrc;


static void handle_signal (int signum) {
    // Calling exit() in a signal handler is unsafe
    // exit() will run atexit functions! + triggers writting of gcda files (gcov/lcov)
    // but might interrupt pending IO operations somewhere in libc...
    // Quite unlikely - nevertheless we should use pselect in aio_loop
    // and enable signals only when hanging in pselect
    // Termination code throughout station tries first SIGTERM and after a while SIGKILL, thus,
    // we should not end up with a station process lingering.
    exit(128+signum);
    // Signal safe but less convenient
    //_exit(128+signum);
}



static int updateDirSetting (str_t path, str_t source, str_t* pdir, str_t* psrc) {
    int l = strlen(path);
    char* p = rt_mallocN(char, l+5);  // more space for optional "./" and/or "/" + "\0"
    if( path[0] ) {
        strcpy(p,path);
    } else {
        strcpy(p,"./");
        l = 2;
    }
    if( p[l-1] != '/' ) {
        p[l++] = '/';
    }
    if( p[0] != '/' && (p[0] != '.' || p[1] != '/') ) {
        memmove(p+2, p, l+1);
        p[0] = '.';
        p[1] = '/';
    }
    struct stat st;
    if( stat(p, &st) == -1 ) {
        fprintf(stderr, "%s - Cannot access directory '%s': %s\n", source, p, strerror(errno));
        goto err;
    }
    if( !S_ISDIR(st.st_mode) ) {
        fprintf(stderr, "%s - Not a directory: %s\n", source, p);
  err:
        rt_free(p);
        return 0;
    }

    rt_free((void*)*pdir);
    rt_free((void*)*psrc);
    *pdir = p;
    *psrc = rt_strdup(source);
    return 1;
}

static int setWebDir (str_t path, str_t source) {
    return updateDirSetting(path, source, &webDir, &webDirSrc);
}

static int setHomeDir (str_t path, str_t source) {
    return updateDirSetting(path, source, &homeDir, &homeDirSrc);
}

static int setTempDir (str_t path, str_t source) {
    return updateDirSetting(path, source, &tempDir, &tempDirSrc);
}


static dbuf_t stripTrailingWsp (dbuf_t b) {
    while( b.bufsize > 0 && strchr(" \t\r\n", b.buf[b.bufsize-1]) ) {
        b.buf[--b.bufsize] = 0;
    }
    return b;
}


static str_t parseEui (str_t s, int n, uL_t* peui, int nonzero) {
    str_t p = s;
    uL_t eui = rt_readEui(&p, n);
    if( p==s || (n==0 ? (p[0] != 0) : (p != s+n)) )
        return "syntax error";
    if( nonzero && eui == 0 )
        return "must not be zero";
    *peui = eui;
    return NULL;
}


static void findDefaultEui () {
    str_t dirname = "/sys/class/net";
    DIR* D = opendir(dirname);
    if( D==NULL )
        return;
    char path[NAME_MAX+32];
    char ifc[64] = {0};
    uL_t eui = 0;
    struct dirent* de;
    while( (de = readdir(D)) ) {
        char* dname = de->d_name;
        if( strlen(dname) > sizeof(ifc)-1 )
            continue;
        if( strcmp("lo", dname) == 0 )
            continue;
        snprintf(path, sizeof(path), "%s/%s/address", dirname, dname);
        dbuf_t b = stripTrailingWsp(readFile(path, 0));
        if( b.buf == NULL )
            continue;
        uL_t mac = 0;
        str_t err = parseEui(b.buf, b.bufsize, &mac, 1);
        rt_free(b.buf);
        if( err != NULL )
            continue;
        // Prefer ethX devices
        if( ifc[0] != 0 ) {
            if( strncmp(ifc, "eth", 3) == 0 && strncmp(dname, "eth", 3) != 0 )
                continue; // eth trumps other devices
            // Otherwie choose alphabetically lowest - unless eth replaces something else
            if( !((strncmp(ifc, "eth", 3) == 0) ^ (strncmp(dname, "eth", 3) == 0))
                && strcmp(ifc, dname) <= 0 )
                continue; // not lower
        }
        strcpy(ifc, dname);
        eui = mac;
        continue;
    }
    closedir(D);
    if( eui ) {
        snprintf(path, sizeof(path), "%s/%s/address", dirname, ifc);
        protoEUI = eui;
        rt_free((void*)protoEuiSrc);
        protoEuiSrc = rt_strdup(path);
    }
}


static int setEui (str_t spec, str_t source) {
    str_t err;
    if( access(spec, R_OK) == 0 ) {
        dbuf_t b = stripTrailingWsp(sys_readFile(spec));
        if( b.buf && (err = parseEui(b.buf, b.bufsize, &protoEUI, 1)) == NULL ) {
            char sbuf[strlen(source)+strlen(spec)+32];
            snprintf(sbuf, sizeof(sbuf), "%s file %s", source, spec);
            rt_free((void*)protoEuiSrc);
            protoEuiSrc = rt_strdup(sbuf);
            rt_free(b.buf);
            return 1;
        }
        if( b.buf == NULL ) {
            LOG(MOD_SYS|ERROR, "Station proto EUI %s (%s): Cannot read file", spec, source);
        } else {
            LOG(MOD_SYS|ERROR, "Station proto EUI '%s' (%s file %s): %s", b.buf, source, spec, err);
        }
        rt_free(b.buf);
        return 0;
    }
    if( (err = parseEui(spec, strlen(spec), &protoEUI, 1)) == NULL ) {
        rt_free((void*)protoEuiSrc);
        protoEuiSrc = rt_strdup(source);
        return 1;
    }
    LOG(MOD_SYS|ERROR, "Station proto EUI: '%s' (%s): %s", spec, source, err);
    return 0;
}


// Check if there is a process that has an open file handle to a specific device/file
int sys_findPids (str_t device, u4_t* pids, int n_pids) {
    if( device[0] != '/' )
        return 0;

    char path[NAME_MAX+64];
    struct dirent* de;
    DIR *D, *DD;
    int dlen, cnt = 0;

    dlen = strlen(device);
    assert(dlen < NAME_MAX);
    strcpy(path, "/proc");
    if( (D = opendir("/proc")) == NULL )
        return 0;
    while( (de = readdir(D)) ) {
        const char* pid_s = de->d_name;
        sL_t pid = rt_readDec(&pid_s);
        if( pid < 0 )
            continue;
        int n_prefix = snprintf(path, sizeof(path), "/proc/%s/fd", de->d_name);
        if( (DD = opendir(path)) == NULL )
            continue;
        while( (de = readdir(DD)) ) {
            if( de->d_type != DT_LNK )
                continue;
            char linkpath[NAME_MAX];
            snprintf(path+n_prefix, sizeof(path)-n_prefix, "/%s", de->d_name);
            int err = readlink(path, linkpath, sizeof(linkpath));
            if( err != dlen )
                continue;
            linkpath[err] = 0;
            if( strcmp(device, linkpath) == 0 ) {
                if( cnt < n_pids )
                    pids[cnt] = pid;
                cnt++;
            }
        }
        closedir(DD);
    }
    closedir(D);
    return cnt;
}


str_t sys_radioDevice (str_t device, u1_t* comtype) {
    str_t f = device==NULL ? radioDevice : device;
    if( f == NULL )
        f = RADIODEV;
    // check for comtype prefix
    if( comtype )
	*comtype = COMTYPE_SPI;
    char *colon = index(f, ':');
    if( colon ) {
	if( strncmp(f, "spi:", 4) == 0 ) {
	    if( comtype )
		*comtype = COMTYPE_SPI;
	} else if( strncmp(f, "usb:", 4) == 0 ) {
	    if( comtype )
		*comtype = COMTYPE_USB;
	} else {
	    LOG(MOD_SYS|ERROR, "Unknown device comtype '%.*s' (using SPI)", colon-f, f);
	}
	f = colon + 1;
    }
    // Caller must free result
    return sys_makeFilepath(f, 0);
}


void sys_fatal (int code) {
    exit(code==0 ? FATAL_GENERIC : code);
}

// 生成PID文件路径
static char* makePidFilename() {
    return makeFilepath("~temp/station",".pid",NULL,0);  // 生成临时目录下的station.pid文件路径
}

// 读取PID文件中的进程ID
static int readPid() {
    char* pidfile = makePidFilename();  // 获取PID文件路径
    dbuf_t b = readFile(pidfile,0);  // 读取PID文件内容（不投诉错误）
    const char* s = b.buf;  // 获取文件内容指针
    int pid = rt_readDec(&s);  // 从字符串中读取十进制数值
    rt_free(pidfile);  // 释放文件路径内存
    rt_free(b.buf);  // 释放文件内容内存
    return max(0,pid);  // 返回PID（确保非负值）
}

// 写入当前进程ID到PID文件
static void writePid () {
    char buf[16];  // 临时缓冲区
    dbuf_t b = dbuf_ini(buf);  // 初始化动态缓冲区
    xprintf(&b, "%d", daemonPid ? daemonPid : getpid());  // 格式化PID字符串（守护进程PID或当前进程PID）
    char* pidsfile = makePidFilename();  // 获取PID文件路径
    writeFile(pidsfile, b.buf, b.pos);  // 写入PID到文件
    rt_free(pidsfile);  // 释放文件路径内存
}


// 杀死旧的进程
static void killOldPid () {
    int pid = readPid();  // 读取已存在的PID
    if( daemonPid && pid == daemonPid )  // 如果PID与当前守护进程PID相同
        return;  // 工作进程在守护进程下启动，无需杀死
    if( pid > 0 ) {  // 如果存在有效的PID
        pid_t pgid = getpgid(pid);  // 获取进程组ID
        if( pgid == pid ) {  // 如果是进程组领导者
            fprintf(stderr, "Killing process group %d\n", pid);  // 打印杀死进程组信息
            kill(-pid, SIGINT);  // 发送SIGINT信号给整个进程组
            rt_usleep(2000);  // 等待2毫秒
            kill(-pid, SIGKILL);  // 发送SIGKILL信号给整个进程组（强制杀死）
        } else {  // 如果是普通进程
            fprintf(stderr, "Killing process %d\n", pid);  // 打印杀死进程信息
            kill( pid, SIGINT);  // 发送SIGINT信号给单个进程
            rt_usleep(2000);  // 等待2毫秒
            kill( pid, SIGKILL);  // 发送SIGKILL信号给单个进程（强制杀死）
        }
    }
}



static void leds_off () {
    sys_inState(SYSIS_STATION_DEAD);
}

void sys_ini () {
    LOG(MOD_SYS|INFO, "Logging     : %s (maxsize=%d, rotate=%d)\n",
        logfile.path==NULL ? "stderr" : logfile.path, logfile.size, logfile.rotate);  // 记录日志信息
    LOG(MOD_SYS|INFO, "Station Ver : %s",  CFG_version " " CFG_bdate);  // 记录站点版本信息
    LOG(MOD_SYS|INFO, "Package Ver : %s",  sys_version());  // 记录软件包版本信息
    LOG(MOD_SYS|INFO, "mbedTLS Ver : %s",  MBEDTLS_VERSION_STRING);  // 记录mbedTLS版本信息
    LOG(MOD_SYS|INFO, "proto EUI   : %:E\t(%s)", protoEUI, protoEuiSrc);  // 记录协议EUI信息
    LOG(MOD_SYS|INFO, "prefix EUI  : %:E\t(%s)", prefixEUI, prefixEuiSrc);  // 记录前缀EUI信息
    LOG(MOD_SYS|INFO, "Station EUI : %:E", sys_eui());  // 记录站点EUI信息
    LOG(MOD_SYS|INFO, "Station home: %s\t(%s)",  homeDir, homeDirSrc);  // 记录站点主目录信息
    LOG(MOD_SYS|INFO, "Station temp: %s\t(%s)",  tempDir, tempDirSrc);  // 记录站点临时目录信息
    if( sys_slaveIdx >= 0 ) {
        LOG(MOD_SYS|INFO, "Station slave: %d", sys_slaveIdx);  // 记录站点从模式信息
    } else {
        if( gpsDevice )
            LOG(MOD_SYS|INFO, "GPS device: %s", gpsDevice);  // 记录GPS设备信息
    }
    if( sys_noTC || sys_noCUPS ) {
        LOG(MOD_SYS|WARNING, "Station in NO-%s mode", sys_noTC ? "TC" : "CUPS");  // 记录站点无TC或无CUPS模式信息
    }
    int seed;
    sys_seed((u1_t*)&seed, sizeof(seed));  // 生成随机种子
    srand(seed);  // 初始化随机数生成器
}


void sys_seed (unsigned char* seed, int len) {
    int fd;
    if( (fd = open("/dev/urandom", O_RDONLY)) == -1 ) {
        if( (fd = open("/dev/random", O_RDONLY)) == -1 ) {
            // Some fallback
        fail:
            LOG(MOD_SYS|CRITICAL, "Unable to properly seed cryptographic random number generator!");
            ustime_t t = sys_time();
            for( int i=0; i<8 && i<len; i++, t>>=8 )
                seed[i] ^= t;
            uL_t p = (ptrdiff_t)seed;
            for( int i=8; i<16 && i <len; i++, p>>=8 )
                seed[i] ^= p;
            return;
        }
    }
    int n = read(fd, seed, len);
    if( n != len )
        goto fail;
    close(fd);
}


void sys_usleep (sL_t us) {
    if( us <= 0 )
        return;
    struct timespec slp, rem = { .tv_sec = us/1000000, .tv_nsec = us%1000000*1000 }; // 200ms
    while( rem.tv_sec > 0 || rem.tv_nsec > 0 ) {
        slp = rem;
        if( nanosleep(&slp, &rem) == 0 )
            break;
    }
}


sL_t sys_time () {
    struct timespec tp;
    int err = clock_gettime(CLOCK_MONOTONIC, &tp);
    if( err == -1 )
        rt_fatal("clock_gettime(2) failed: %s\n", strerror(errno));      // LCOV_EXCL_LINE
    return tp.tv_sec*(sL_t)1000000 + tp.tv_nsec/1000;
}


sL_t sys_utc () {
    struct timespec tp;
    int err = clock_gettime(CLOCK_REALTIME, &tp);
    if( err == -1 )
        rt_fatal("clock_gettime(2) failed: %s\n", strerror(errno));      // LCOV_EXCL_LINE
    return (tp.tv_sec*(sL_t)1000000 + tp.tv_nsec/1000);
}


str_t sys_version () {
    return readFileAsString("version", ".txt", &versionTxt);
}

/* FW Update ************************************ */

void sys_updateStart (int len) {
    close(updfd);
    if( len == 0 ) {
        updfd = -1;
        return;
    }
    makeFilepath("/tmp/update", ".bi_", &temp_updfile, 0);
    updfd = open(temp_updfile, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP);
    if( updfd == -1 )
        LOG(MOD_SYS|ERROR, "Failed to open '%s': %s", temp_updfile, strerror(errno));
}

void sys_updateWrite (u1_t* data, int off, int len) {
    if( updfd == -1 ) return;
    int err = write(updfd, data, len);
    if( err == -1 ) {
        LOG(MOD_SYS|ERROR, "Failed to write '%s': %s", temp_updfile, strerror(errno));
        close(updfd);
        updfd = -1;
    }
}

int sys_updateCommit (int len) {
    // Rename file and start a process
    if( len == 0 )
        return 1;
    if( updfd == -1 ) {
        if( temp_updfile )
            unlink(temp_updfile);
        return 0;
    }
    close(updfd);
    sync();
    updfd = -1;
    makeFilepath("/tmp/update", ".bin", &updfile, 0);
    if( rename(temp_updfile, updfile) == -1 ) {
        LOG(MOD_SYS|ERROR, "Rename of update file failed '%s': %s", temp_updfile, strerror(errno));
    }
    sync();
    return 1;
}


void sys_runUpdate () {
    makeFilepath("/tmp/update", ".bin", &updfile, 0);
    if( access(updfile, X_OK) != 0 )
        return; // no such file or not executable
    str_t argv[2] = { updfile, NULL };
    sys_execCommand(0, argv);  // 0=detach, don't wait for update to finish
}

void sys_abortUpdate () {
    unlink("/tmp/update.bin");
    sync();
}

int sys_runRadioInit (str_t device) {
    setenv("LORAGW_SPI", device, 1);   // for libloragw (SPI module)
    if( !radioInit )
        return 1;
    char buf[16];
    str_t argv[4] = { radioInit, device, NULL, NULL };
    if( sys_slaveIdx >= 0 ) {
        snprintf(buf, sizeof(buf), "%d", sys_slaveIdx);
        argv[2] = buf;
    }
    return sys_execCommand(RADIO_INIT_WAIT, argv) == 0;
}


int sys_execCommand (ustime_t max_wait, str_t* argv) {
    int argc = 0;
    while( argv[argc] ) argc++;
    if( argc == 0 || (argc==1 && argv[0][0]==0) )
        return 0;
    sys_flushLog();
    pid_t pid1;
    if( (pid1 = fork()) == 0 ) {
        pid_t pid2 = 0;
        if( max_wait!=0 || (pid2 = fork()) == 0 ) {
            if( access(argv[0], X_OK) != 0 ) {
                // Not an executable file
                str_t* argv2 = rt_mallocN(str_t, argc+4);
                memcpy(&argv2[3], &argv[0], sizeof(argv[0])*(argc+1)); // also copy trailing NULL
                if( access(argv[0], F_OK) == -1 ) {
                    // Not even a file - assume shell statements
                    argv2[0] = "/bin/sh";
                    argv2[1] = "-c";
                    argv2[2] = argv[0];
                    argv = argv2;
                } else {
                    // Assume file with Shell statements
                    argv2[1] = "/bin/bash";
                    argv2[2] = argv[0];
                    argv = &argv2[1];
                }
            }
            for( int i=0; argv[i]; i++ )
                LOG(MOD_SYS|DEBUG, "%s argv[%d]: <%s>\n", i==0?"execvp":"      ", i, argv[i]);
            log_flushIO();

            if( execvp(argv[0], (char*const*)argv) == -1 ) {
                LOG(MOD_SYS|ERROR, "%s: Failed to exec: %s", argv[0], strerror(errno));
                log_flushIO();
                exit(9);
            }
        } else if( pid2 < 0 ) {
            LOG(MOD_SYS|ERROR, "%s: Fork(2) failed: %s", argv[0], strerror(errno));
            log_flushIO();
            exit(8);
        }
        exit(0);
    }
    if( pid1 < 0 ) {
        LOG(MOD_SYS|ERROR, "%s: Fork failed: %s", argv[0], strerror(errno));
        return -1;
    }
    LOG(MOD_SYS|VERBOSE, "%s: Forked, waiting...", argv[0]);
    log_flushIO();
    int wmode = WNOHANG;
    if( max_wait == 0 ) {       // detached child forks a grand child - child exits
        max_wait = USTIME_MAX;  // basically forever
        wmode = 0;
    }
    for( ustime_t u=0; u < max_wait; u+=rt_millis(1) ) {
        int status = 0;
        int err = waitpid(pid1, &status, wmode);
        if( err == -1 ) {
            LOG(MOD_SYS|ERROR, "Process %s (pid=%d) - waitpid failed: %s", argv[0], pid1, strerror(errno));
            return -1;
        }
        if( err == pid1 ) {
            if( WIFEXITED(status) ) {
                int xcode = WEXITSTATUS(status);
                if( xcode == 0 ) {
                    LOG(MOD_SYS|INFO, "Process %s (pid=%d) completed", argv[0], pid1);
                    log_flushIO();
                    return 0;
                }
                LOG(MOD_SYS|ERROR, "Process %s (pid=%d) failed with exit code %d", argv[0], pid1, xcode);
                return xcode;
            }
            if( WIFSIGNALED(status) ) {
                int signo = WTERMSIG(status);
                LOG(MOD_SYS|ERROR, "Process %s (pid=%d) terminated by signal %d", argv[0], pid1, signo);
                return -2;
            }
            LOG(MOD_SYS|ERROR, "Process %s (pid=%d) with strange exit state 0x%X", argv[0], pid1, status);
            return -4;
        }
        rt_usleep(rt_millis(2));
    }
    kill(pid1, SIGTERM);
    LOG(MOD_SYS|ERROR, "Process %s (pid=%d) did not terminate within %ldms - killing it (SIGTERM)",
        argv[0], pid1, max_wait/1000);
    return -3;
}

static int setLogLevel (str_t arg, str_t source) {
    str_t err = log_parseLevels(arg);
    if( err  ) {
        int n = strlen(err);
        fprintf(stderr, "%s: Failed to parse log level: %.*s%s\n", source, 8, err, n>8 ? ".." : "");
        return 0;
    }
    return 1;
}

static int setLogFile (str_t logdef, str_t source) {
    // override  builtin defaults
    if( strcmp(logdef, "stderr") == 0 || strcmp(logdef, "-") == 0 ) {
        logfile.path = NULL;
        return 1;
    }
    free((void*)logfile.path);
    str_t spec = strchr(logdef,',');
    if( spec != NULL ) {
        str_t path = rt_strdupn(logfile.path, spec-logfile.path);
        logfile.path = sys_makeFilepath(path,1);
        rt_free((void*)path);
        sL_t logsz = rt_readDec((str_t*)&spec);
        if( logsz > 0 )
            logfile.size = min(max(logsz, 10000), (sL_t)100e6);
        if( spec[0] == ',' ) {
            int logrot = rt_readDec((str_t*)&spec);
            if( logrot > 0 )
                logfile.rotate = min(max(logrot, 0), (sL_t)100);
        }
        if( !spec[0] ) {
            fprintf(stderr, "%s: Illegal log file spec: %s\n", source, logdef);
            return 0;
        }
    } else {
        logfile.path = sys_makeFilepath(logdef,0);
    }
    return 1;
}

static int parseStationConf () {
    str_t filename = "station.conf";
    dbuf_t jbuf = sys_readFile(filename);
    if( jbuf.buf == NULL ) {
        LOG(MOD_SYS|ERROR, "No such file (or not readable): %s", filename);
        return 0;
    }
    ujdec_t D;
    uj_iniDecoder(&D, jbuf.buf, jbuf.bufsize);
    if( uj_decode(&D) ) {
        LOG(MOD_SYS|ERROR, "Parsing of JSON failed - '%s' ignored", filename);
        free(jbuf.buf);
        return 0;
    }
    u1_t ccaDisabled=0, dcDisabled=0, dwellDisabled=0;   // fields not present
    ujcrc_t field;
    uj_enterObject(&D);
    while( (field = uj_nextField(&D)) ) {
        switch(field) {
        case J_station_conf: {
            uj_enterObject(&D);
            while( (field = uj_nextField(&D)) ) {
                switch(field) {
                case J_routerid: {
                    if( !setEui(uj_str(&D), filename) )
                        uj_error(&D, "Illegal EUI");
                    break;
                }
                case J_euiprefix: {
                    str_t err = parseEui(uj_str(&D), 0, &prefixEUI, 0);
                    if( err != NULL )
                        uj_error(&D, "Illegal EUI: %s", err);
                    free((void*)prefixEuiSrc);
                    prefixEuiSrc = rt_strdup(filename);
                    break;
                }
                case J_log_file: {
                    if( !setLogFile(uj_str(&D), filename) )
                        uj_error(&D, "Illegal log file spec: %s", D.str.beg);
                    break;
                }
                case J_log_size: {
                    logfile.size = uj_num(&D);
                    break;
                }
                case J_log_rotate: {
                    logfile.rotate = uj_int(&D);
                    break;
                }
                case J_log_level: {
                    if( !setLogLevel(uj_str(&D), filename) )
                        uj_error(&D, "Illegal log level: %s", D.str.beg);
                    break;
                }
                case J_gps: {
                    makeFilepath(uj_str(&D),"",&gpsDevice,0);
                    break;
                }
                case J_pps: {
                    str_t mode = uj_str(&D);
                    if( strcmp(mode,"gps") == 0 ) {
                        sys_modePPS = PPS_GPS;
                    }
                    else if( strcmp(mode,"fuzzy") == 0 ) {
                        sys_modePPS = PPS_FUZZY;
                    }
                    else if( strcmp(mode,"testpin") == 0 ) {
                        sys_modePPS = PPS_TESTPIN;
                    }
                    else {
                        uj_error(&D, "Illegal pps mode: %s", mode);
                    }
                    break;
                }
                case J_radio_init: {
                    free((void*)radioInit);
                    radioInit = rt_strdup(uj_str(&D));
                    radioInitSrc = filename;
                    break;
                }
#if defined(CFG_prod)
                case J_nocca:
                case J_nodc:
                case J_nodwell:
                case J_device_mode: {
                    LOG(MOD_S2E|WARNING, "Feature not supported in production level code (station.conf) - ignored: %s", D.field.name);
                    uj_skipValue(&D);
                    break;
                }
#else // !defined(CFG_prod)
                case J_nocca: {
                    ccaDisabled = uj_bool(&D) ? 2 : 1;
                    break;
                }
                case J_nodc: {
                    dcDisabled = uj_bool(&D) ? 2 : 1;
                    break;
                }
                case J_nodwell: {
                    dwellDisabled = uj_bool(&D) ? 2 : 1;
                    break;
                }
                case J_device_mode: {
                    sys_deviceMode = uj_bool(&D) ? 1 : 0;
                    break;
                }
#endif // !defined(CFG_prod)
                case J_device: {
                    free((void*)radioDevice);
                    radioDevice = rt_strdup(uj_str(&D));
                    break;
                }
                case J_web_port: {
                    sys_webPort = uj_intRange(&D, 1, 65535);
                    break;
                }
                case J_web_dir: {
                    setWebDir(uj_str(&D), filename);
                    break;
                }
                default: {
                    dbuf_t b = uj_skipValue(&D);
                    int err = s2conf_set(filename, D.field.name, rt_strdupn(b.buf, b.bufsize));
                    if( err == -1 )
                        LOG(MOD_SYS|WARNING, "Ignoring field: %s", D.field.name);
                    break;
                }
                }
            }
            uj_exitObject(&D);
            break;
        }
        default: {
            uj_skipValue(&D);
            break;
        }
        }
    }
    uj_exitObject(&D);
    uj_assertEOF(&D);
    free(jbuf.buf);
    if( ccaDisabled   ) s2e_ccaDisabled   = ccaDisabled   & 2;
    if( dcDisabled    ) s2e_dcDisabled    = dcDisabled    & 2;
    if( dwellDisabled ) s2e_dwellDisabled = dwellDisabled & 2;
    return 1;
}


static struct opts {
    str_t logLevel;
    str_t logFile;
    str_t homeDir;
    str_t tempDir;
    str_t radioInit;
    str_t euiprefix;
    int   slaveMode;
    str_t slaveExec;
    u1_t  params;
    u1_t  daemon;
    u1_t  force;
    u1_t  kill;
    u1_t  notc;
} *opts;


static struct argp_option options[] = {
    { "log-file", 'L', "FILE[,SIZE[,ROT]]", 0,
      ("Write log entries to FILE. If FILE is '-' then write to stderr. "
       "Optionally followed by a max file SIZE and a number of rotation files. "
       "If ROT is 0 then keep only FILE. If ROT is 1 then keep one more old "
       "log file around. "
       "Overrides environment STATION_LOGFILE.")
    },
    { "log-level", 'l', "LVL|0..7", 0,
      ("Set a log level LVL=#loglvls# or use a numeric value. "
       "Overrides environment STATION_LOGLEVEL.")
    },
    { "home", 'h', "DIR", 0,
      ("Home directory for configuration files. "
       "Default is the current working directory. "
       "Overrides environment STATION_DIR.")
    },
    { "temp", 't', "DIR", 0,
      ("Temp directory for frequently written files. "
       "Default is /tmp. "
       "Overrides environment STATION_TEMPDIR.")
    },
    { "radio-init", 'i', "cmd", 0,
      ("Program/script to run before reinitializing radio hardware. "
       "By default nothing is being executed. "
       "Overrides environment STATION_RADIOINIT.")
    },
    { "eui-prefix", 'x', "id6", 0,
      ("Turn MAC address into EUI by adding this prefix. If the argument has value "
       "ff:fe00:0 then the EUI is formed by inserting FFFE in the middle. "
       "If absent use MAC or routerid as is. "
       "Overrides environment STATION_EUIPREFIX.")
    },
    { "params", 'p', NULL, 0,
      ("Print current parameter settings.")
    },
    { "version", 'v', NULL, 0,
      ("Print station version."),
    },
    { "daemon", 'd', NULL, 0,
      ("First check if another process is still alive. If so do nothing and exit. "
       "Otherwise fork a worker process to operate the radios and network protocols. "
       "If the subprocess died respawn it with an appropriate back off.")
    },
    { "force", 'f', NULL, 0,
      ("If a station process is already running, kill it before continuing with requested operation mode.")
    },
    { "kill", 'k', NULL, 0,
      ("Kill a currently running station process.")
    },
    { "no-tc", 'N', NULL, 0,
      ("Do not connect to a LNS. Only run CUPS functionality.")
    },
    { "slave", 'S', NULL, OPTION_HIDDEN,
      ("Station process is slave to a master process. For internal use only."),
    },
    { "exec", 'X', "CMD", OPTION_HIDDEN,
      ("Template for exec of slave processes. For internal/test use only."),
    },
    { "selftests", 256, NULL, OPTION_HIDDEN,
      ("If compiled with builtin selftests run them. For internal/test use only."),
    },
    { "fscmd", 257, "cmdline", OPTION_HIDDEN,
      ("Run a command on the simulated flash."),
    },
    { "fskey", 258, "hex", OPTION_HIDDEN,
      ("Specify an encryption key for the simulated flash."),
    },
    { "fscd",  259, "dir", OPTION_HIDDEN,
      ("Specify an current working dir for the simulated flash."),
    },
    { 0 }
};


static int parse_opt (int key, char* arg, struct argp_state* state) {
    switch(key) {
    case 259: {
        int err = fs_chdir(arg);
        if( err != 0 ) {
            fprintf(stderr, "Failed --fscd: %s\n", strerror(errno));
            exit(8);
        }
        return 0;
    }
    case 258: {
        u4_t key[4] = {0};
        for( int ki=0; ki<16; ki++ ) {
            int b = (rt_hexDigit(arg[2*ki])<<4) | rt_hexDigit(arg[2*ki+1]);
            if( b < 0 ) {
                fprintf(stderr, "Illegal --fskey argument - expecting 32 hex digits\n");
                exit(7);
            }
            key[ki/4] |= b<<(24 - ki%4*8);
        }
        fs_ini(key);
        return 0;
    }
    case 257: {
        fs_ini(NULL);
        exit(fs_shell(arg));
    }
    case 256: {
        setenv("STATION_SELFTESTS", "1", 1);
        return 0;
    }
    case 'S': {
        opts->slaveMode = 1;
        return 0;
    }
    case 'X': {
        free((void*)sys_slaveExec);
        sys_slaveExec = rt_strdup(arg);
        return 0;
    }
    case 'x': {
        opts->euiprefix = arg;
        return 0;
    }
    case 'l': {
        opts->logLevel = arg;
        return 0;
    }
    case 'L': {
        opts->logFile = arg;
        return 0;
    }
    case 'h': {
        opts->homeDir = arg;
        return 0;
    }
    case 't': {
        opts->tempDir = arg;
        return 0;
    }
    case 'i': {
        opts->radioInit = arg;
        return 0;
    }
    case 'p': {
        opts->params = 1;
        return 0;
    }
    case 'd': {
        opts->daemon = 1;
        return 0;
    }
    case 'f': {
        opts->force = 1;
        return 0;
    }
    case 'k': {
        opts->kill = 1;
        return 0;
    }
    case 'N': {
        opts->notc = 1;
        return 0;
    }
    case 'v': {
        fputs("Station: " CFG_version " " CFG_bdate "\n", stdout);
        readFileAsString("version", ".txt", &versionTxt);
        fprintf(stdout, "Package: %s\n", versionTxt);
        exit(0);
    }
    case ARGP_KEY_END: {
        return 0;
    }
    case ARGP_KEY_ARG: {
        break;
    }
    }
    return ARGP_ERR_UNKNOWN;
}

struct argp argp = { options, parse_opt, "", NULL, NULL, NULL, NULL };


// 进一步初始化系统的各个子系统
static void startupMaster2 (tmr_t* tmr) {
#if !defined(CFG_no_rmtsh)
    rt_addFeature("rmtsh"); // 添加远程shell特性
#endif
#if defined(CFG_prod)
    rt_addFeature("prod");  // 添加生产环境特性，某些开发/测试/调试特性不被接受
#endif
    sys_enableCmdFIFO(makeFilepath("~/cmd",".fifo",NULL,0)); // 启用命令FIFO，用于进程间通信
    if( gpsDevice ) {
        rt_addFeature("gps"); // 如果存在GPS设备，添加GPS特性
        sys_enableGPS(gpsDevice); // 启用GPS设备
    }
    sys_iniTC(); // 初始化时间同步子系统
    sys_startTC(); // 启动时间同步子系统
    sys_iniCUPS(); // 初始化CUPS（配置和更新服务器）子系统
    sys_triggerCUPS(0); // 触发CUPS子系统
    sys_iniWeb(); // 初始化Web子系统
}

// 启动主进程的初始化函数
static void startupMaster (tmr_t* tmr) {
    sys_startLogThread(); // 启动一个独立的日志线程
    if( getenv("STATION_SELFTESTS") ) {
        selftests(); // 如果环境变量 STATION_SELFTESTS 存在，执行自检功能
        // NOT REACHED
    }
    // 终止任何旧的进程 - 创建一个包含当前进程ID的文件
    writePid();
    // 如果有待处理的更新 - 执行更新
    sys_runUpdate();
    ral_ini(); // 初始化无线电抽象层，准备与无线电硬件进行通信
    atexit(leds_off); // 注册程序退出时关闭LED的函数
    // 等待从进程启动
    //startupMaster2(tmr);
    rt_setTimerCb(tmr, rt_millis_ahead(200), startupMaster2); // 设置一个定时器，在200毫秒后调用startupMaster2函数
}


// 前向声明
static void startupDaemon (tmr_t* tmr);

// 我们在这里使用轮询而不是SIGCHLD信号，因为使用SIGCHLD会需要在aio中使用pselect，
// 这在某些平台上不太便携（例如FreeRTOS上的LWIP）。轮询不是问题，
// 因为我们还希望减慢重启速度，以避免在紧密的重启循环中阻塞系统。
static void waitForWorker (tmr_t* tmr) {
    int wstatus;  // 用于存储子进程状态
    pid_t wpid = waitpid(workerPid, &wstatus, WNOHANG);  // 非阻塞方式等待工作进程
    //NOT-NEEDED sys_inState(SYSIS_STATION_DEAD);
    if( wpid < 0 || wpid == workerPid ) {  // 如果等待失败或工作进程已死亡
        LOG(MOD_SYS|ERROR, "DAEMON: Station process %d died (exit code 0x%X)", workerPid, wstatus);  // 记录进程死亡日志
        workerPid = 0;  // 清除工作进程ID
        startupDaemon(&startupTmr);  // 重新启动守护进程
    } else {
        rt_setTimer(&startupTmr, rt_millis_ahead(500));  // 设置定时器，500毫秒后再次检查
    }
}


// 启动守护进程函数
static void startupDaemon (tmr_t* tmr) {
    int subprocPid;  // 子进程ID
    // 重新生成station工作进程
    sys_inState(SYSIS_STATION_DEAD);  // 设置系统状态为站点死亡
    sys_flushLog();  // 刷新日志
    if( (subprocPid = fork()) == -1 )  // 创建子进程
        rt_fatal("DAEMON: Failed to fork station: %s", strerror(errno));  // 如果fork失败，打印错误并退出
    if( subprocPid == 0 ) {  // 子进程代码
        // 子进程
        sys_iniLogging(&logfile, 1);  // 初始化日志系统（允许stdio捕获）
        LOG(MOD_SYS|INFO, "DAEMON: Station process %d started...", getpid());  // 记录子进程启动日志
        rt_yieldTo(&startupTmr, startupMaster);  // 将控制权交给主进程启动函数
    } else {
        // 父进程代码
        workerPid = subprocPid;  // 保存工作进程ID
        rt_yieldTo(&startupTmr, waitForWorker);  // 将控制权交给工作进程等待函数
    }
}


// 主函数入口
int sys_main (int argc, char** argv) {
    // 在rt_ini()之前就开始记录日志，所以需要设置UTC偏移
    rt_utcOffset = sys_utc() - rt_getTime();

    // 设置信号处理
    signal(SIGHUP,  SIG_IGN);    // 忽略SIGHUP信号
    signal(SIGINT,  handle_signal);  // 处理SIGINT信号
    signal(SIGTERM, handle_signal);  // 处理SIGTERM信号

    // 获取并设置当前工作目录
    char cwd[MAX_FILEPATH_LEN];  // 定义存储当前工作目录的字符数组
    if( getcwd(cwd, sizeof(cwd)) != NULL )  // 获取当前工作目录
        fs_chdir(cwd);  // 设置当前工作目录

    // 初始化配置和日志设置
    s2conf_ini();  // 初始化s2配置
    logfile.size = LOGFILE_SIZE;  // 设置日志文件大小
    logfile.rotate = LOGFILE_ROTATE;  // 设置日志文件轮换
    setHomeDir(".", "builtin");  // 设置主目录
    // setWebDir("./web", "builtin");  // 设置Web目录（注释掉）
    setTempDir(access("/var/tmp", W_OK) < 0 ? "/tmp" : "/var/tmp", "builtin");  // 设置临时目录
    prefixEuiSrc = rt_strdup("builtin");  // 复制字符串"builtin"到prefixEuiSrc
    findDefaultEui();  // 查找默认EUI

    // 解析命令行参数
    opts = rt_malloc(struct opts);  // 分配内存给opts结构体
    int err = argp_parse (&argp, argc, argv, 0, NULL, NULL);  // 解析命令行参数
    if( err != 0 )  // 如果解析出错
        return err;  // 返回错误码

#if defined(CFG_ral_master_slave)
    // 主从模式相关设置
    int slave_rdfd = -1, slave_wrfd = -1;  // 初始化从模式读写文件描述符
    if( opts->slaveMode ) {  // 如果是从模式
        str_t const* sn = SLAVE_ENVS;  // 获取从模式环境变量
        while( *sn ) {  // 遍历环境变量
            str_t sv = getenv(*sn);  // 获取环境变量值
            if( sv == NULL )  // 如果环境变量不存在
                rt_fatal("Missing mandatory env var: %s", *sn);  // 打印错误信息并退出
            str_t sve = sv;  // 复制环境变量值
            sL_t v = rt_readDec(&sve);  // 读取十进制值
            if( v < 0 )  // 如果值非法
                rt_fatal("Env var %s has illegal value: %s", *sn, sv);  // 打印错误信息并退出
            switch(sn[0][6]) {  // 根据环境变量名的第七个字符进行判断
            case 'I':  // 如果是从模式索引
                log_setSlaveIdx(sys_slaveIdx = v);  // 设置从模式索引
                break;
            case 'R':  // 如果是读文件描述符
                slave_rdfd = v;  // 设置从模式读文件描述符
                break;
            case 'W':  // 如果是写文件描述符
                slave_wrfd = v;  // 设置从模式写文件描述符
                break;
            }
            sn++;  // 移动到下一个环境变量
        }
    }
    if( sys_slaveExec == NULL ) {  // 如果从模式执行命令为空
        sys_slaveExec = rt_strdup("/proc/self/exe -S");  // 设置默认从模式执行命令
    }
#endif // defined(CFG_ral_master_slave)

    // 处理EUI前缀设置
    {
        str_t prefix = opts->euiprefix;  // 获取EUI前缀
        str_t source = "--eui-prefix";  // 设置EUI前缀来源
        if( prefix == NULL ) {  // 如果EUI前缀为空
            source = "STATION_EUIPREFIX";  // 设置EUI前缀来源为环境变量
            prefix = getenv(source);  // 获取环境变量值
        } else {
            setenv("STATION_EUIPREFIX", prefix, 1);  // 设置环境变量
        }
        if( prefix ) {  // 如果EUI前缀存在
            str_t err = parseEui(prefix, 0, & prefixEUI, 0);  // 解析EUI前缀
            if( err )  // 如果解析出错
                rt_fatal("%s has illegal EUI value: %s", source, err);  // 打印错误信息并退出
            free((void*)prefixEuiSrc);  // 释放prefixEuiSrc内存
            prefixEuiSrc = rt_strdup(source);  // 复制EUI前缀来源
        }
    }

    // 设置临时目录
    if( opts->tempDir ) {  // 如果指定了临时目录
        if( !setTempDir(opts->tempDir, "--temp") )  // 设置临时目录
            return 1;  // 如果设置失败，返回1
        setenv("STATION_TEMPDIR", opts->tempDir, 1);  // 设置环境变量
    } else {
        str_t source = "STATION_TEMPDIR";  // 设置临时目录来源
        str_t v = getenv(source);  // 获取环境变量值
        if( v && !setTempDir(v, source) )  // 如果环境变量存在且设置失败
            return 1;  // 返回1
    }

    // 设置主目录
    if( opts->homeDir ) {  // 如果指定了主目录
        if( !setHomeDir(opts->homeDir, "--home") )  // 设置主目录
            return 1;  // 如果设置失败，返回1
        setenv("STATION_HOME", opts->homeDir, 1);  // 设置环境变量
    } else {
        str_t source = "STATION_HOME";  // 设置主目录来源
        str_t v = getenv(source);  // 获取环境变量值
        if( v && !setHomeDir(v, source) )  // 如果环境变量存在且设置失败
            return 1;  // 返回1
    }

    // 解析站点配置
    if( !parseStationConf() )  // 如果解析站点配置失败
        return 1;  // 返回1
    if( opts->params ) {  // 如果指定了参数
        s2conf_printAll();  // 打印所有配置
    }

    // 设置日志文件
    if( opts->logFile ) {  // 如果指定了日志文件
        if( !setLogFile(opts->logFile, "--log-file") )  // 设置日志文件
            return 1;  // 如果设置失败，返回1
        setenv("STATION_LOGFILE", opts->logFile, 1);  // 设置环境变量
    } else {
        str_t source = "STATION_LOGFILE";  // 设置日志文件来源
        str_t v = getenv(source);  // 获取环境变量值
        if( v && !setLogFile(v, source) )  // 如果环境变量存在且设置失败
            return 1;  // 返回1
    }

    // 设置无线电初始化
    if( opts->radioInit ) {  // 如果指定了无线电初始化
        radioInitSrc = "--radio-init";  // 设置无线电初始化来源
        free((char*)radioInit);  // 释放radioInit内存
        radioInit = rt_strdup(opts->radioInit);  // 复制无线电初始化
        setenv("STATION_RADIOINIT", radioInit, 1);  // 设置环境变量
    } else {
        str_t s = "STATION_RADIOINIT";  // 设置无线电初始化来源
        str_t v = getenv(s);  // 获取环境变量值
        if( v ) {  // 如果环境变量存在
            radioInitSrc = s;  // 设置无线电初始化来源
            free((char*)radioInit);  // 释放radioInit内存
            radioInit = rt_strdup(v);  // 复制无线电初始化
        }
    }

    // 设置日志级别
    if( opts->logLevel ) {  // 如果指定了日志级别
        if( !setLogLevel(opts->logLevel, "--log-level") )  // 设置日志级别
            return 1;  // 如果设置失败，返回1
        setenv("STATION_LOGLEVEL", opts->logLevel, 1);  // 设置环境变量
    } else {
        str_t source = "STATION_LOGLEVEL";  // 设置日志级别来源
        str_t v = getenv(source);  // 获取环境变量值
        if( v && !setLogLevel(v, source) )  // 如果环境变量存在且设置失败
            return 1;  // 返回1
    }

    // 设置TLS调试级别
    {
        str_t source = "STATION_TLSDBG";  // 设置TLS调试级别来源
        str_t v = getenv(source);  // 获取环境变量值
        if( v && (v[0]&0xF0) == '0' )  // 如果环境变量存在且符合条件
            tls_dbgLevel = v[0] - '0';  // 设置TLS调试级别
    }

    // 处理kill选项
    if( opts->kill ) {  // 如果指定了kill选项
        if( opts->daemon || opts->force ) {  // 如果同时指定了守护进程或强制选项
            fprintf(stderr, "Option -k is incompatible with -d/-f\n");  // 打印错误信息
            return 1;  // 返回1
        }
        killOldPid();  // 杀死旧进程
        return 0;  // 返回0
    }
    sys_noTC = opts->notc;  // 设置不使用TC选项

    // 保存守护进程和强制选项
    int daemon = opts->daemon;  // 获取守护进程选项
    int force = opts->force;  // 获取强制选项
    free(opts);  // 释放opts内存
    opts = NULL;  // 将opts置为空

#if defined(CFG_ral_master_slave)
    int isSlave = (sys_slaveIdx >= 0);  // 判断是否为从模式
#else
    int isSlave = 0;  // 默认不是从模式
#endif

    // 检查进程是否已经在运行
    if( !isSlave ) {  // 如果不是从模式
        if( !force ) {  // 如果没有强制选项
            int pid = readPid();  // 读取进程ID
            if( pid && kill(pid, 0) == 0 ) {  // 如果进程存在
                fprintf(stderr, "A station with pid=%d is still running (use -f to take over)\n", pid);  // 打印错误信息
                exit(EXIT_NOP);  // 退出程序
            }
        } else {
            killOldPid();  // 杀死旧进程
        }
    }

    // 设置配置文件名并检查
    setupConfigFilenames();  // 设置配置文件名
    checkRollForward();  // 检查版本前滚
    if( !checkUris() )  // 检查URI
        return 1;  // 如果检查失败，返回1

    // 处理守护进程模式（-d选项）
    if( daemon ) {  // 如果是守护进程模式
        // 设置守护进程的默认日志文件
        if( logfile.path == NULL ) {  // 如果用户没有指定日志文件路径
            setLogFile("~temp/station.log", "builtin");  // 将默认的stderr输出改为文件输出
            setenv("STATION_TEMPDIR", tempDir, 1);  // 设置临时目录环境变量
        }
        int subprocPid;  // 定义子进程ID变量
        // 第一次fork：创建守护进程
        if( (subprocPid = fork()) == -1 )  // 创建子进程，如果失败返回-1
            rt_fatal("Daemonize fork failed: %s\n", strerror(errno));  // 如果fork失败，打印错误信息并退出
        if( subprocPid != 0 ) {  // 如果是父进程（fork返回子进程PID）
            // 父进程代码块：打印守护进程信息后退出
            fprintf(stderr, "Daemon pid=%d running...\n", subprocPid);  // 打印守护进程ID信息
            daemonPid = subprocPid;  // 保存守护进程ID到全局变量
            writePid();  // 将守护进程ID写入PID文件
            exit(0);  // 父进程完成任务后退出，让子进程继续运行
        }
        // 子进程代码块：成为守护进程
        daemonPid = getpid();  // 获取当前进程ID（现在是守护进程）
        setsid();  // 创建新的会话，脱离控制终端，成为会话领导者
    }

    // 初始化各个子系统
    aio_ini();  // 初始化异步IO
    sys_iniLogging(&logfile, !isSlave && !daemon);  // 初始化日志系统
    sys_ini();  // 初始化系统
    rt_ini();  // 初始化运行时
    ts_iniTimesync();  // 初始化时间同步

#if defined(CFG_ral_master_slave)
    // 如果是从模式，启动从进程
    if( isSlave ) {  // 如果是从模式
        sys_startupSlave(slave_rdfd, slave_wrfd);  // 启动从进程
        // 不会到达这里
        assert(0);  // 断言失败
    }
#endif // defined(CFG_ral_master_slave)

    // 启动主循环
    rt_yieldTo(&startupTmr, daemon ? startupDaemon : startupMaster);  // 根据模式启动相应的主循环
    aio_loop();  // 进入异步IO循环
    // 不会到达这里
    assert(0);  // 断言失败
}
