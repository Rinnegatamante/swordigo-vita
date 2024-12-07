/* main.c -- Swordigo .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2023 Rinnegatamante
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"
#include "trophies.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <AL/efx.h>

//#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define dlog printf
#else
#define dlog
#endif

extern void init_soloud();
extern void load_music(const char *fname);
extern void play_music();
extern void set_music_loop(int looping);
extern void set_music_volume(float vol);
extern void stop_music();

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static char data_path[256];

static char fake_vm[0x1000];
static char fake_env[0x1000];

int framecap = 0;

long sysconf(int name) {
	return 0;
}

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

#if 1
void *__wrap_calloc(uint32_t nmember, uint32_t size) { return vglCalloc(nmember, size); }
void __wrap_free(void *addr) { vglFree(addr); };
void *__wrap_malloc(uint32_t size) { return vglMalloc(size); };
void *__wrap_memalign(uint32_t alignment, uint32_t size) { return vglMemalign(alignment, size); };
void *__wrap_realloc(void *ptr, uint32_t size) { return vglRealloc(ptr, size); };
#else
void *__wrap_calloc(uint32_t nmember, uint32_t size) { return __real_calloc(nmember, size); }
void __wrap_free(void *addr) { __real_free(addr); };
void *__wrap_malloc(uint32_t size) { return __real_malloc(size); };
void *__wrap_memalign(uint32_t alignment, uint32_t size) { return __real_memalign(alignment, size); };
void *__wrap_realloc(void *ptr, uint32_t size) { return __real_realloc(ptr, size); };	
#endif

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module main_mod, lua_mod;

int ret4() { return 4; }

void *__wrap_memset(void *s, int c, size_t n) {
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size) {
	strcpy(buf, data_path);
	return buf;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	*memptr = memalign(alignment, size);
	return 0;
}

int debugPrintf(char *text, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/swordigo_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...) {
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list) {
#ifdef ENABLE_DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void) {
	return 0;
}

int ret1(void) {
	return 1;
}

#define  MUTEX_TYPE_NORMAL     0x0000
#define  MUTEX_TYPE_RECURSIVE  0x4000
#define  MUTEX_TYPE_ERRORCHECK 0x8000

static pthread_t s_pthreadSelfRet;

static void init_static_mutex(pthread_mutex_t **mutex)
{
    pthread_mutex_t *mtxMem = NULL;

    switch ((int)*mutex) {
	case MUTEX_TYPE_NORMAL: {
	    pthread_mutex_t initTmpNormal = PTHREAD_MUTEX_INITIALIZER;
	    mtxMem = calloc(1, sizeof(pthread_mutex_t));
	    sceClibMemcpy(mtxMem, &initTmpNormal, sizeof(pthread_mutex_t));
	    *mutex = mtxMem;
	    break;
	}
	case MUTEX_TYPE_RECURSIVE: {
	    pthread_mutex_t initTmpRec = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
	    mtxMem = calloc(1, sizeof(pthread_mutex_t));
	    sceClibMemcpy(mtxMem, &initTmpRec, sizeof(pthread_mutex_t));
	    *mutex = mtxMem;
	    break;
	}
	case MUTEX_TYPE_ERRORCHECK: {
	    pthread_mutex_t initTmpErr = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
	    mtxMem = calloc(1, sizeof(pthread_mutex_t));
	    sceClibMemcpy(mtxMem, &initTmpErr, sizeof(pthread_mutex_t));
	    *mutex = mtxMem;
	    break;
	}
	default:
	    break;
    }
}

static void init_static_cond(pthread_cond_t **cond)
{
    if (*cond == NULL) {
	pthread_cond_t initTmp = PTHREAD_COND_INITIALIZER;
	pthread_cond_t *condMem = calloc(1, sizeof(pthread_cond_t));
	sceClibMemcpy(condMem, &initTmp, sizeof(pthread_cond_t));
	*cond = condMem;
    }
}

int pthread_attr_destroy_soloader(pthread_attr_t **attr)
{
    int ret = pthread_attr_destroy(*attr);
    free(*attr);
    return ret;
}

int pthread_attr_getstack_soloader(const pthread_attr_t **attr,
				   void **stackaddr,
				   size_t *stacksize)
{
    return pthread_attr_getstack(*attr, stackaddr, stacksize);
}

__attribute__((unused)) int pthread_condattr_init_soloader(pthread_condattr_t **attr)
{
    *attr = calloc(1, sizeof(pthread_condattr_t));

    return pthread_condattr_init(*attr);
}

__attribute__((unused)) int pthread_condattr_destroy_soloader(pthread_condattr_t **attr)
{
    int ret = pthread_condattr_destroy(*attr);
    free(*attr);
    return ret;
}

int pthread_cond_init_soloader(pthread_cond_t **cond,
			       const pthread_condattr_t **attr)
{
    *cond = calloc(1, sizeof(pthread_cond_t));

    if (attr != NULL)
	return pthread_cond_init(*cond, *attr);
    else
	return pthread_cond_init(*cond, NULL);
}

int pthread_cond_destroy_soloader(pthread_cond_t **cond)
{
    int ret = pthread_cond_destroy(*cond);
    free(*cond);
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t **cond)
{
    init_static_cond(cond);
    return pthread_cond_signal(*cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t **cond,
				    pthread_mutex_t **mutex,
				    struct timespec *abstime)
{
    init_static_cond(cond);
    init_static_mutex(mutex);
    return pthread_cond_timedwait(*cond, *mutex, abstime);
}

int pthread_create_soloader(pthread_t **thread,
			    const pthread_attr_t **attr,
			    void *(*start)(void *),
			    void *param)
{
    *thread = calloc(1, sizeof(pthread_t));
	
	int r;
    if (attr != NULL) {
		pthread_attr_setstacksize(*attr, 512 * 1024);
		r = pthread_create(*thread, *attr, start, param);
    } else {
		pthread_attr_t attrr;
		pthread_attr_init(&attrr);
		pthread_attr_setstacksize(&attrr, 512 * 1024);
		r = pthread_create(*thread, &attrr, start, param);
    }
	printf("pthread_create %x\n", r);
	return r;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t **attr)
{
    *attr = calloc(1, sizeof(pthread_mutexattr_t));

    return pthread_mutexattr_init(*attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t **attr, int type)
{
    return pthread_mutexattr_settype(*attr, type);
}

int pthread_mutexattr_setpshared_soloader(pthread_mutexattr_t **attr, int pshared)
{
    return pthread_mutexattr_setpshared(*attr, pshared);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t **attr)
{
    int ret = pthread_mutexattr_destroy(*attr);
    free(*attr);
    return ret;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t **mutex)
{
    int ret = pthread_mutex_destroy(*mutex);
    free(*mutex);
    return ret;
}

int pthread_mutex_init_soloader(pthread_mutex_t **mutex,
				const pthread_mutexattr_t **attr)
{
    *mutex = calloc(1, sizeof(pthread_mutex_t));

    if (attr != NULL)
	return pthread_mutex_init(*mutex, *attr);
    else
	return pthread_mutex_init(*mutex, NULL);
}

int pthread_mutex_lock_soloader(pthread_mutex_t **mutex)
{
    init_static_mutex(mutex);
    return pthread_mutex_lock(*mutex);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t **mutex)
{
    init_static_mutex(mutex);
    return pthread_mutex_trylock(*mutex);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t **mutex)
{
    return pthread_mutex_unlock(*mutex);
}

int pthread_join_soloader(const pthread_t *thread, void **value_ptr)
{
    return pthread_join(*thread, value_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t **cond, pthread_mutex_t **mutex)
{
    return pthread_cond_wait(*cond, *mutex);
}

int pthread_cond_broadcast_soloader(pthread_cond_t **cond)
{
    return pthread_cond_broadcast(*cond);
}

int pthread_attr_init_soloader(pthread_attr_t **attr)
{
    *attr = calloc(1, sizeof(pthread_attr_t));

    return pthread_attr_init(*attr);
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t **attr, int state)
{
    return pthread_attr_setdetachstate(*attr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t **attr, size_t stacksize)
{
    return pthread_attr_setstacksize(*attr, stacksize);
}

int pthread_attr_setschedparam_soloader(pthread_attr_t **attr,
					const struct sched_param *param)
{
    return pthread_attr_setschedparam(*attr, param);
}

int pthread_attr_setstack_soloader(pthread_attr_t **attr,
				   void *stackaddr,
				   size_t stacksize)
{
    return pthread_attr_setstack(*attr, stackaddr, stacksize);
}

int pthread_setschedparam_soloader(const pthread_t *thread, int policy,
				   const struct sched_param *param)
{
    return pthread_setschedparam(*thread, policy, param);
}

int pthread_getschedparam_soloader(const pthread_t *thread, int *policy,
				   struct sched_param *param)
{
    return pthread_getschedparam(*thread, policy, param);
}

int pthread_detach_soloader(const pthread_t *thread)
{
    return pthread_detach(*thread);
}

int pthread_getattr_np_soloader(pthread_t* thread, pthread_attr_t *attr) {
    fprintf(stderr, "[WARNING!] Not implemented: pthread_getattr_np\n");
    return 0;
}

int pthread_equal_soloader(const pthread_t *t1, const pthread_t *t2)
{
	if (t1 == t2)
		return 1;
	if (!t1 || !t2)
		return 0;
    return pthread_equal(*t1, *t2);
}

pthread_t *pthread_self_soloader()
{
    s_pthreadSelfRet = pthread_self();
    return &s_pthreadSelfRet;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(const pthread_t *thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
	return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
	return ERANGE;
    }

    // TODO: Implement the actual name setting if possible
    fprintf(stderr, "PTHR: pthread_setname_np with name %s\n", thread_name);

    return 0;
}

int clock_gettime_hook(int clk_id, struct timespec *t) {
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}


int GetCurrentThreadId(void) {
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;

int GetEnv(void *vm, void **env, int r2) {
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b) {
	printf("throwing %s\n", *str);
}

int remove_hook(const char *fname) {
#ifdef ENABLE_DEBUG
	printf("remove %s\n", fname);
#endif
	char real_fname[256];
	char *src = fname;
	if (strncmp(fname, "ux0:", 4)) {
		if (fname[0] == '.')
			sprintf(real_fname, "%s/%s", data_path, &fname[2]);
		else
			sprintf(real_fname, "%s/%s", data_path, fname);
		src = real_fname;
	}
	remove(src);
	return 0;
}

int rename_hook(const char *fname, const char *fname2) {
#ifdef ENABLE_DEBUG
	printf("rename %s -> %s\n", fname, fname2);
#endif
	char real_fname[256], real_fname2[256];
	char *src = fname, *dst = fname2;
	if (strncmp(fname, "ux0:", 4)) {
		if (fname[0] == '.')
			sprintf(real_fname, "%s/%s", data_path, &fname[2]);
		else
			sprintf(real_fname, "%s/%s", data_path, fname);
		src = real_fname;
	}
	if (strncmp(fname2, "ux0:", 4)) {
		if (fname2[0] == '.')
			sprintf(real_fname2, "%s/%s", data_path, &fname2[2]);
		else
			sprintf(real_fname2, "%s/%s", data_path, fname2);
		dst = real_fname2;
	}
	return rename(src, dst);
}

FILE *fopen_hook(char *fname, char *mode) {
	FILE *f;
	char real_fname[256];
#ifdef ENABLE_DEBUG
	printf("fopen(%s,%s)\n", fname, mode);
#endif
	if (!strcmp(fname, "/dev/urandom")) {
			f = fopen("ux0:data/swordigo/urandom.txt", "wb");
			uint32_t random_val = rand();
			fwrite(&random_val, 1, 4, f);
			fclose(f);
			sceKernelDelayThread(1000 * 1000);
			f = fopen("ux0:data/swordigo/urandom.txt", mode);
	} else if (strncmp(fname, "ux0:", 4)) {
		if (fname[0] == '.')
			sprintf(real_fname, "%s/assets/%s", data_path, &fname[2]);
		else
			sprintf(real_fname, "%s/assets/%s", data_path, fname);
		f = fopen(real_fname, mode);
	} else {
		f = fopen(fname, mode);
	}
	return f;
}

int open_hook(const char *fname, int flags, mode_t mode) {
	int f;
	char real_fname[256];
#ifdef ENABLE_DEBUG
	printf("open(%s)\n", fname);
#endif
	if (strncmp(fname, "ux0:", 4)) {
		sprintf(real_fname, "%s/%s", data_path, fname);
		f = open(real_fname, flags, mode);
	} else {
		f = open(fname, flags, mode);
	}
	return f;
}

extern void *__aeabi_atexit;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int chk_guard = 0x42424242;
static int *__stack_chk_guard_fake = &chk_guard;

static FILE __sF_fake[0x1000][3];

typedef struct __attribute__((__packed__)) stat64_bionic {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long long __pad4;
} stat64_bionic;

inline void stat_newlib_to_stat_bionic(const struct stat * src, stat64_bionic * dst)
{
    if (!src) return;
    if (!dst) dst = malloc(sizeof(stat64_bionic));

    dst->st_dev = src->st_dev;
    dst->st_ino = src->st_ino;
    dst->st_mode = src->st_mode;
    dst->st_nlink = src->st_nlink;
    dst->st_uid = src->st_uid;
    dst->st_gid = src->st_gid;
    dst->st_rdev = src->st_rdev;
    dst->st_size = src->st_size;
    dst->st_blksize = src->st_blksize;
    dst->st_blocks = src->st_blocks;
    dst->st_atime = src->st_atime;
    dst->st_atime_nsec = 0;
    dst->st_mtime = src->st_mtime;
    dst->st_mtime_nsec = 0;
    dst->st_ctime = src->st_ctime;
    dst->st_ctime_nsec = 0;
}

int stat_hook(const char *_pathname, void *statbuf) {
	//printf("stat(%s)\n", _pathname);
	struct stat st;
    int res = stat(_pathname, &st);

    if (res == 0)
        stat_newlib_to_stat_bionic(&st, statbuf);

    return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length) {
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf) {
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void *sceClibMemclr(void *dst, SceSize len) {
	if (!dst) {
		printf("memclr on NULL\n");
		return NULL;
	}
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv() {
	return fake_env;
}

void abort_hook() {
	printf("abort called from %p\n", __builtin_return_address(0));
	exit(0);
}

int ret99() {
	return 99;
}

int chdir_hook(const char *path) {
	return 0;
}

GLint glGetUniformLocation_fake(GLuint program, const GLchar *name) {
	if (!strcmp(name, "texture"))
		return glGetUniformLocation(program, "_texture");
	return glGetUniformLocation(program, name);
}

static so_default_dynlib gl_hook[] = {
	{"glPixelStorei", (uintptr_t)&ret0},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent {
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct {
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp) {
	if (!dirp || dirp->uid < 0) {
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname) {
	printf("opendir(%s)\n", dirname);
	SceUID uid = sceIoDopen(dirname);

	if (uid < 0) {
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp) {
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp) {
	if (!dirp) {
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0) {
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0) {
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

size_t __strlen_chk(const char *s, size_t s_len) {
	return strlen(s);
}

uint64_t lseek64(int fd, uint64_t offset, int whence) {
	return lseek(fd, offset, whence);
}

void glBindAttribLocation_fake(GLuint program, GLuint index, const GLchar *name) {
	if (index == 2) {
		glBindAttribLocation(program, 2, "extents");
		glBindAttribLocation(program, 2, "vertcol");
	}
	glBindAttribLocation(program, index, name);
}

void *dlsym_hook( void *handle, const char *symbol);

void *dlopen_hook(const char *filename, int flags) {
	printf("dlopen %s\n", filename);
	return 1;
}

int AAssetManager_open(void *mgr, const char *fname, int mode) {
	char full_fname[256];
	sprintf(full_fname, "ux0:data/swordigo/assets/%s", fname);
	int r = open(full_fname, 0);
	if (r == -1)
		return 0;
	return r;
}

int skip_close = 0;
int AAsset_openFileDescriptor(int f, int32_t *start, int32_t *len) {
	lseek(f, *start, SEEK_SET);
	skip_close = 1;
	return f;
}

int AAsset_close(int f) {
	if (skip_close) {
		skip_close = 0;
		return 0;
	}
	return close(f);
}

off_t AAsset_getLength(int f) {
	off_t res = lseek(f, 0, SEEK_END);
	lseek(f, 0, SEEK_SET);
	return res;
}

int AAsset_read(int f, void *buf, size_t count) {
	int r = read(f, buf, count);
	return r;
}

size_t AAsset_seek(int f, size_t offs, int whence) {
	return lseek(f, offs, whence);
}

uint32_t exidx_end = 0x42A788;
uint32_t exidx_start = 0x410FF8;
long unsigned int *__gnu_Unwind_Find_exidx(long unsigned int *PC, int *pcount) {
	*pcount = (exidx_end - exidx_start) / 8;
	return (main_mod.text_base + exidx_start);
}

ALCcontext  *al_context_id;
ALCdevice   *al_device;

ALCdevice *alcOpenDevice_hook(const ALCchar *devicename) {
	printf("al_device is %x\n", al_device);
	return al_device;
}

ALCcontext *alcCreateContext_hook(ALCdevice *device, const ALCint* attrlist) {
	printf("al_context is %x\n", al_context_id);
	return al_context_id;
}

static so_default_dynlib default_dynlib[] = {
	{ "AAssetManager_fromJava", (uintptr_t)&ret1 },
	{ "AAssetManager_open", (uintptr_t)&AAssetManager_open },
	{ "AAsset_close", (uintptr_t)&AAsset_close },
	{ "AAsset_getLength", (uintptr_t)&AAsset_getLength },
	{ "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
	//{ "AAsset_getRemainingLength", (uintptr_t)&ret0 },
	{ "AAsset_read", (uintptr_t)&AAsset_read },
	{ "AAsset_seek", (uintptr_t)&AAsset_seek },
	{ "glTexParameteri", (uintptr_t)&glTexParameteri},
	{ "glGetError", (uintptr_t)&ret0},
	{ "glReadPixels", (uintptr_t)&glReadPixels},
	{ "glShaderSource", (uintptr_t)&glShaderSource},
	{ "glGetUniformLocation", (uintptr_t)&glGetUniformLocation_fake},
	{ "glBindAttribLocation", (uintptr_t)&glBindAttribLocation_fake},
	{ "sincosf", (uintptr_t)&sincosf },
	{ "opendir", (uintptr_t)&opendir_fake },
	{ "readdir", (uintptr_t)&readdir_fake },
	{ "closedir", (uintptr_t)&closedir_fake },
	{ "__aeabi_memclr", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr4", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memclr8", (uintptr_t)&sceClibMemclr },
	{ "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove4", (uintptr_t)&memmove },
	{ "__aeabi_memmove8", (uintptr_t)&memmove },
	{ "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
	{ "__aeabi_memmove", (uintptr_t)&memmove },
	{ "__aeabi_memset", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset4", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_memset8", (uintptr_t)&sceClibMemset2 },
	{ "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
	{ "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
	{ "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
	{ "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
	{ "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
	{ "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
	{ "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
	{ "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
	{ "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
	{ "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
	{ "__android_log_print", (uintptr_t)&__android_log_print },
	{ "__android_log_vprint", (uintptr_t)&__android_log_vprint },
	{ "__android_log_write", (uintptr_t)&__android_log_write },
	{ "__cxa_atexit", (uintptr_t)&__cxa_atexit },
	{ "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
	{ "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
	{ "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
	{ "__cxa_finalize", (uintptr_t)&__cxa_finalize },
	{ "__errno", (uintptr_t)&__errno },
	{ "__strlen_chk", (uintptr_t)&__strlen_chk },
	{ "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
	{ "__gnu_Unwind_Find_exidx", (uintptr_t)&__gnu_Unwind_Find_exidx },
	{ "dl_unwind_find_exidx", (uintptr_t)&__gnu_Unwind_Find_exidx },
	{ "__sF", (uintptr_t)&__sF_fake },
	{ "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
	{ "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
	{ "_ctype_", (uintptr_t)&BIONIC_ctype_},
	{ "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{ "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{ "abort", (uintptr_t)&abort_hook },
	{ "access", (uintptr_t)&access },
	{ "acos", (uintptr_t)&acos },
	{ "acosh", (uintptr_t)&acosh },
	{ "asctime", (uintptr_t)&asctime },
	{ "acosf", (uintptr_t)&acosf },
	{ "asin", (uintptr_t)&asin },
	{ "asinh", (uintptr_t)&asinh },
	{ "asinf", (uintptr_t)&asinf },
	{ "atan", (uintptr_t)&atan },
	{ "atanh", (uintptr_t)&atanh },
	{ "atan2", (uintptr_t)&atan2 },
	{ "atan2f", (uintptr_t)&atan2f },
	{ "atanf", (uintptr_t)&atanf },
	{ "atoi", (uintptr_t)&atoi },
	{ "atol", (uintptr_t)&atol },
	{ "atoll", (uintptr_t)&atoll },
	{ "basename", (uintptr_t)&basename },
	// { "bind", (uintptr_t)&bind },
	{ "bsd_signal", (uintptr_t)&ret0 },
	{ "bsearch", (uintptr_t)&bsearch },
	{ "btowc", (uintptr_t)&btowc },
	{ "calloc", (uintptr_t)&calloc },
	{ "ceil", (uintptr_t)&ceil },
	{ "ceilf", (uintptr_t)&ceilf },
	{ "chdir", (uintptr_t)&chdir_hook },
	{ "clearerr", (uintptr_t)&clearerr },
	{ "clock", (uintptr_t)&clock },
	{ "clock_gettime", (uintptr_t)&clock_gettime_hook },
	{ "close", (uintptr_t)&close },
	{ "cos", (uintptr_t)&cos },
	{ "cosf", (uintptr_t)&cosf },
	{ "cosh", (uintptr_t)&cosh },
	{ "crc32", (uintptr_t)&crc32 },
	{ "deflate", (uintptr_t)&deflate },
	{ "deflateEnd", (uintptr_t)&deflateEnd },
	{ "deflateInit_", (uintptr_t)&deflateInit_ },
	{ "deflateInit2_", (uintptr_t)&deflateInit2_ },
	{ "deflateReset", (uintptr_t)&deflateReset },
	{ "dlopen", (uintptr_t)&dlopen_hook },
	{ "dlclose", (uintptr_t)&ret0 },
	{ "dlsym", (uintptr_t)&dlsym_hook },
	{ "exit", (uintptr_t)&exit },
	{ "exp", (uintptr_t)&exp },
	{ "exp2", (uintptr_t)&exp2 },
	{ "exp2f", (uintptr_t)&exp2f },
	{ "expf", (uintptr_t)&expf },
	{ "fabsf", (uintptr_t)&fabsf },
	{ "fclose", (uintptr_t)&fclose },
	{ "fcntl", (uintptr_t)&ret0 },
	// { "fdopen", (uintptr_t)&fdopen },
	{ "feof", (uintptr_t)&feof },
	{ "ferror", (uintptr_t)&ferror },
	{ "fflush", (uintptr_t)&fflush },
	{ "fgetc", (uintptr_t)&fgetc },
	{ "fgets", (uintptr_t)&fgets },
	{ "floor", (uintptr_t)&floor },
	{ "fileno", (uintptr_t)&fileno },
	{ "floorf", (uintptr_t)&floorf },
	{ "fmod", (uintptr_t)&fmod },
	{ "fmodf", (uintptr_t)&fmodf },
	{ "funopen", (uintptr_t)&funopen },
	{ "fopen", (uintptr_t)&fopen_hook },
	{ "open", (uintptr_t)&open_hook },
	{ "fprintf", (uintptr_t)&fprintf },
	{ "fputc", (uintptr_t)&fputc },
	// { "fputwc", (uintptr_t)&fputwc },
	{ "fputs", (uintptr_t)&fputs },
	{ "fread", (uintptr_t)&fread },
	{ "free", (uintptr_t)&free },
	{ "frexp", (uintptr_t)&frexp },
	{ "frexpf", (uintptr_t)&frexpf },
	// { "fscanf", (uintptr_t)&fscanf },
	{ "fseek", (uintptr_t)&fseek },
	{ "fseeko", (uintptr_t)&fseeko },
	{ "fstat", (uintptr_t)&fstat_hook },
	{ "ftell", (uintptr_t)&ftell },
	{ "ftello", (uintptr_t)&ftello },
	// { "ftruncate", (uintptr_t)&ftruncate },
	{ "fwrite", (uintptr_t)&fwrite },
	{ "getc", (uintptr_t)&getc },
	{ "getpid", (uintptr_t)&ret0 },
	{ "getcwd", (uintptr_t)&getcwd_hook },
	{ "getenv", (uintptr_t)&ret0 },
	{ "getwc", (uintptr_t)&getwc },
	{ "gettimeofday", (uintptr_t)&gettimeofday },
	{ "gzdopen", (uintptr_t)&gzdopen },
	{ "gzclose", (uintptr_t)&gzclose },
	{ "gzread", (uintptr_t)&gzread },
	{ "gzwrite", (uintptr_t)&gzwrite },
	{ "gzopen", (uintptr_t)&gzopen },
	{ "inflate", (uintptr_t)&inflate },
	{ "inflateEnd", (uintptr_t)&inflateEnd },
	{ "inflateInit_", (uintptr_t)&inflateInit_ },
	{ "inflateInit2_", (uintptr_t)&inflateInit2_ },
	{ "inflateReset", (uintptr_t)&inflateReset },
	{ "isascii", (uintptr_t)&isascii },
	{ "isalnum", (uintptr_t)&isalnum },
	{ "isalpha", (uintptr_t)&isalpha },
	{ "iscntrl", (uintptr_t)&iscntrl },
	{ "isdigit", (uintptr_t)&isdigit },
	{ "islower", (uintptr_t)&islower },
	{ "ispunct", (uintptr_t)&ispunct },
	{ "isprint", (uintptr_t)&isprint },
	{ "isspace", (uintptr_t)&isspace },
	{ "isupper", (uintptr_t)&isupper },
	{ "iswalpha", (uintptr_t)&iswalpha },
	{ "iswcntrl", (uintptr_t)&iswcntrl },
	{ "iswctype", (uintptr_t)&iswctype },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswdigit", (uintptr_t)&iswdigit },
	{ "iswlower", (uintptr_t)&iswlower },
	{ "iswprint", (uintptr_t)&iswprint },
	{ "iswpunct", (uintptr_t)&iswpunct },
	{ "iswspace", (uintptr_t)&iswspace },
	{ "iswupper", (uintptr_t)&iswupper },
	{ "iswxdigit", (uintptr_t)&iswxdigit },
	{ "isxdigit", (uintptr_t)&isxdigit },
	{ "ldexp", (uintptr_t)&ldexp },
	{ "ldexpf", (uintptr_t)&ldexpf },
	// { "listen", (uintptr_t)&listen },
	{ "localtime", (uintptr_t)&localtime },
	{ "localtime_r", (uintptr_t)&localtime_r },
	{ "log", (uintptr_t)&log },
	{ "logf", (uintptr_t)&logf },
	{ "log10", (uintptr_t)&log10 },
	{ "log10f", (uintptr_t)&log10f },
	{ "longjmp", (uintptr_t)&longjmp },
	{ "lrand48", (uintptr_t)&lrand48 },
	{ "lrint", (uintptr_t)&lrint },
	{ "lrintf", (uintptr_t)&lrintf },
	{ "lseek", (uintptr_t)&lseek },
	{ "lseek64", (uintptr_t)&lseek64 },
	{ "malloc", (uintptr_t)&malloc },
	{ "mbrtowc", (uintptr_t)&mbrtowc },
	{ "memalign", (uintptr_t)&memalign },
	{ "memchr", (uintptr_t)&sceClibMemchr },
	{ "memcmp", (uintptr_t)&memcmp },
	{ "memcpy", (uintptr_t)&sceClibMemcpy },
	{ "memmove", (uintptr_t)&memmove },
	{ "memset", (uintptr_t)&sceClibMemset },
	{ "mkdir", (uintptr_t)&mkdir },
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{ "modf", (uintptr_t)&modf },
	{ "modff", (uintptr_t)&modff },
	// { "poll", (uintptr_t)&poll },
	{ "pow", (uintptr_t)&pow },
	{ "powf", (uintptr_t)&powf },
	{ "printf", (uintptr_t)&printf },
	{ "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
	{ "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_soloader },
	{ "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
	{ "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
	{ "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_soloader },
	{ "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_soloader },
	{ "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
	{ "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
	{ "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
	{ "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
	{ "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
	{ "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
	{ "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },
	{ "pthread_create", (uintptr_t) &pthread_create_soloader },
	{ "pthread_detach", (uintptr_t) &pthread_detach_soloader },
	{ "pthread_equal", (uintptr_t) &pthread_equal_soloader },
	{ "pthread_exit", (uintptr_t) &pthread_exit },
	{ "pthread_getattr_np", (uintptr_t) &pthread_getattr_np_soloader },
	{ "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
	{ "pthread_getspecific", (uintptr_t)&pthread_getspecific },
	{ "pthread_key_create", (uintptr_t)&pthread_key_create },
	{ "pthread_key_delete", (uintptr_t)&pthread_key_delete },
	{ "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
	{ "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
	{ "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
	{ "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader},
	{ "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
	{ "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader},
	{ "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader},
	{ "pthread_mutexattr_setpshared", (uintptr_t) &pthread_mutexattr_setpshared_soloader},
	{ "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader},
	{ "pthread_once", (uintptr_t)&pthread_once },
	{ "pthread_self", (uintptr_t) &pthread_self_soloader },
	{ "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
	{ "pthread_setspecific", (uintptr_t)&pthread_setspecific },
	{ "sched_get_priority_min", (uintptr_t)&ret0 },
	{ "sched_get_priority_max", (uintptr_t)&ret99 },
	{ "putc", (uintptr_t)&putc },
	{ "puts", (uintptr_t)&puts },
	{ "putwc", (uintptr_t)&putwc },
	{ "qsort", (uintptr_t)&qsort },
	{ "rand", (uintptr_t)&rand },
	{ "read", (uintptr_t)&read },
	{ "rename", (uintptr_t)&rename_hook },
	{ "realpath", (uintptr_t)&realpath },
	{ "realloc", (uintptr_t)&realloc },
	// { "recv", (uintptr_t)&recv },
	{ "roundf", (uintptr_t)&roundf },
	{ "rint", (uintptr_t)&rint },
	{ "rintf", (uintptr_t)&rintf },
	// { "send", (uintptr_t)&send },
	// { "sendto", (uintptr_t)&sendto },
	{ "setenv", (uintptr_t)&ret0 },
	{ "setjmp", (uintptr_t)&setjmp },
	{ "setlocale", (uintptr_t)&ret0 },
	// { "setsockopt", (uintptr_t)&setsockopt },
	{ "setvbuf", (uintptr_t)&setvbuf },
	{ "sin", (uintptr_t)&sin },
	{ "sinf", (uintptr_t)&sinf },
	{ "sinh", (uintptr_t)&sinh },
	{ "sincos", (uintptr_t)&sincos },
	{ "snprintf", (uintptr_t)&snprintf },
	// { "socket", (uintptr_t)&socket },
	{ "sprintf", (uintptr_t)&sprintf },
	{ "sqrt", (uintptr_t)&sqrt },
	{ "sqrtf", (uintptr_t)&sqrtf },
	{ "srand", (uintptr_t)&srand },
	{ "srand48", (uintptr_t)&srand48 },
	{ "sscanf", (uintptr_t)&sscanf },
	{ "stat", (uintptr_t)&stat_hook },
	{ "strcasecmp", (uintptr_t)&strcasecmp },
	{ "strcasestr", (uintptr_t)&strstr },
	{ "strcat", (uintptr_t)&strcat },
	{ "strchr", (uintptr_t)&strchr },
	{ "strcmp", (uintptr_t)&sceClibStrcmp },
	{ "strcoll", (uintptr_t)&strcoll },
	{ "strcpy", (uintptr_t)&strcpy },
	{ "strcspn", (uintptr_t)&strcspn },
	{ "strdup", (uintptr_t)&strdup },
	{ "strerror", (uintptr_t)&strerror },
	{ "strftime", (uintptr_t)&strftime },
	{ "strlcpy", (uintptr_t)&strlcpy },
	{ "strlen", (uintptr_t)&strlen },
	{ "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
	{ "strncat", (uintptr_t)&sceClibStrncat },
	{ "strncmp", (uintptr_t)&sceClibStrncmp },
	{ "strncpy", (uintptr_t)&sceClibStrncpy },
	{ "strpbrk", (uintptr_t)&strpbrk },
	{ "strrchr", (uintptr_t)&sceClibStrrchr },
	{ "strstr", (uintptr_t)&sceClibStrstr },
	{ "strtod", (uintptr_t)&strtod },
	{ "strtol", (uintptr_t)&strtol },
	{ "strtoul", (uintptr_t)&strtoul },
	{ "strtoll", (uintptr_t)&strtoll },
	{ "strtoull", (uintptr_t)&strtoull },
	{ "strtok", (uintptr_t)&strtok },
	{ "strxfrm", (uintptr_t)&strxfrm },
	{ "sysconf", (uintptr_t)&ret0 },
	{ "tan", (uintptr_t)&tan },
	{ "round", (uintptr_t)&round },
	{ "lround", (uintptr_t)&lround },
	{ "tanf", (uintptr_t)&tanf },
	{ "tanh", (uintptr_t)&tanh },
	{ "time", (uintptr_t)&time },
	{ "tolower", (uintptr_t)&tolower },
	{ "toupper", (uintptr_t)&toupper },
	{ "towlower", (uintptr_t)&towlower },
	{ "towupper", (uintptr_t)&towupper },
	{ "truncf", (uintptr_t)&truncf },
	{ "ungetc", (uintptr_t)&ungetc },
	{ "ungetwc", (uintptr_t)&ungetwc },
	{ "usleep", (uintptr_t)&usleep },
	{ "vfprintf", (uintptr_t)&vfprintf },
	{ "vprintf", (uintptr_t)&vprintf },
	{ "vsnprintf", (uintptr_t)&vsnprintf },
	{ "vsscanf", (uintptr_t)&vsscanf },
	{ "vasprintf", (uintptr_t)&vasprintf },
	{ "vsprintf", (uintptr_t)&vsprintf },
	{ "vswprintf", (uintptr_t)&vswprintf },
	{ "wcrtomb", (uintptr_t)&wcrtomb },
	{ "wcscoll", (uintptr_t)&wcscoll },
	{ "wcscmp", (uintptr_t)&wcscmp },
	{ "wcsncpy", (uintptr_t)&wcsncpy },
	{ "wcsftime", (uintptr_t)&wcsftime },
	{ "wcslen", (uintptr_t)&wcslen },
	{ "wcsxfrm", (uintptr_t)&wcsxfrm },
	{ "wctob", (uintptr_t)&wctob },
	{ "wctype", (uintptr_t)&wctype },
	{ "wmemchr", (uintptr_t)&wmemchr },
	{ "wmemcmp", (uintptr_t)&wmemcmp },
	{ "wmemcpy", (uintptr_t)&wmemcpy },
	{ "wmemmove", (uintptr_t)&wmemmove },
	{ "wmemset", (uintptr_t)&wmemset },
	{ "write", (uintptr_t)&write },
	{ "sigaction", (uintptr_t)&ret0 },
	{ "zlibVersion", (uintptr_t)&zlibVersion },
	// { "writev", (uintptr_t)&writev },
	{ "unlink", (uintptr_t)&unlink },
	{ "alIsSource", (uintptr_t)&alIsSource },
	{ "alBufferData", (uintptr_t)&alBufferData },
	{ "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
	{ "alDeleteSources", (uintptr_t)&alDeleteSources },
	{ "alDistanceModel", (uintptr_t)&alDistanceModel },
	{ "alGenBuffers", (uintptr_t)&alGenBuffers },
	{ "alGenSources", (uintptr_t)&alGenSources },
	{ "alcGetCurrentContext", (uintptr_t)&alcGetCurrentContext },
	{ "alGetBufferi", (uintptr_t)&alGetBufferi },
	{ "alGetEnumValue", (uintptr_t)&alGetEnumValue },
	{ "alGetError", (uintptr_t)&alGetError },
	{ "alGetSourcei", (uintptr_t)&alGetSourcei },
	{ "alGetString", (uintptr_t)&alGetString },
	{ "alGetSourcef", (uintptr_t)&alGetSourcef },
	{ "alIsBuffer", (uintptr_t)&alIsBuffer },
	{ "alListener3f", (uintptr_t)&alListener3f },
	{ "alListenerf", (uintptr_t)&alListenerf },
	{ "alListenerfv", (uintptr_t)&alListenerfv },
	{ "alSource3f", (uintptr_t)&alSource3f },
	{ "alSourcePause", (uintptr_t)&alSourcePause },
	{ "alSourcePlay", (uintptr_t)&alSourcePlay },
	{ "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
	{ "alSourceStop", (uintptr_t)&alSourceStop },
	{ "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
	{ "alSourcef", (uintptr_t)&alSourcef },
	{ "alSourcei", (uintptr_t)&alSourcei },
	{ "alcCaptureSamples", (uintptr_t)&alcCaptureSamples },
	{ "alcCaptureStart", (uintptr_t)&alcCaptureStart },
	{ "alcCaptureStop", (uintptr_t)&alcCaptureStop },
	{ "alcCaptureOpenDevice", (uintptr_t)&alcCaptureOpenDevice },
	{ "alcCloseDevice", (uintptr_t)&alcCloseDevice },
	{ "alcCreateContext", (uintptr_t)&alcCreateContext_hook },
	{ "alcGetContextsDevice", (uintptr_t)&alcGetContextsDevice },
	{ "alcGetError", (uintptr_t)&alcGetError },
	{ "alcGetIntegerv", (uintptr_t)&alcGetIntegerv },
	{ "alcGetString", (uintptr_t)&alcGetString },
	{ "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
	{ "alcDestroyContext", (uintptr_t)&alcDestroyContext },
	{ "alcOpenDevice", (uintptr_t)&alcOpenDevice_hook },
	{ "alcProcessContext", (uintptr_t)&alcProcessContext },
	{ "alcPauseCurrentDevice", (uintptr_t)&ret0 },
	{ "alcResumeCurrentDevice", (uintptr_t)&ret0 },
	{ "alcSuspendContext", (uintptr_t)&alcSuspendContext },
	{ "alcIsExtensionPresent", (uintptr_t)&alcIsExtensionPresent },
	{ "alcGetProcAddress", (uintptr_t)&alcGetProcAddress },
	{ "alIsExtensionPresent", (uintptr_t)&alIsExtensionPresent },
	{ "alcSuspend", (uintptr_t)&ret0 }, // FIXME
	{ "alcResume", (uintptr_t)&ret0 }, // FIXME
	{ "alGetListenerf", (uintptr_t)&alGetListenerf },
	{ "alSourceRewind", (uintptr_t)&alSourceRewind },
	{ "raise", (uintptr_t)&raise },
	{ "posix_memalign", (uintptr_t)&posix_memalign },
	{ "swprintf", (uintptr_t)&swprintf },
	{ "wcscpy", (uintptr_t)&wcscpy },
	{ "wcscat", (uintptr_t)&wcscat },
	{ "wcstombs", (uintptr_t)&wcstombs },
	{ "wcsstr", (uintptr_t)&wcsstr },
	{ "compress", (uintptr_t)&compress },
	{ "uncompress", (uintptr_t)&uncompress },
	{ "atof", (uintptr_t)&atof },
	{ "remove", (uintptr_t)&remove_hook },
	{ "__system_property_get", (uintptr_t)&ret0 },
	{ "strnlen", (uintptr_t)&strnlen },
	{ "glNormalPointer", (uintptr_t)&ret0 },
	{ "glViewport", (uintptr_t)&glViewport },
};
static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

void *dlsym_hook( void *handle, const char *symbol) {
	printf("dlsym %s\n", symbol);
	for (size_t i = 0; i < numhooks; ++i) {
		if (!strcmp(symbol, default_dynlib[i].symbol)) {
			return default_dynlib[i].func;
		}
	}
	return vglGetProcAddress(symbol);
}

int check_kubridge(void) {
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs {
	UNKNOWN = 0,
	INIT,
	IS_AGE_KNOWN,
	GET_PLATFORM_CONSENT_STATE,
	LOAD_FILE,
	PLAY,
	PAUSE,
	STOP,
	SET_LOOPING,
	SET_VOLUME,
	REPORT_ACHIEVEMENT_PROGRESS,
	IS_GOOGLE_GAME_SERVICES_AVAILABLE
} MethodIDs;

typedef struct {
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{ "<init>", INIT },
	{ "isAgeKnown", IS_AGE_KNOWN },
	{ "getPlatformConsentState", GET_PLATFORM_CONSENT_STATE },
	{ "loadFile", LOAD_FILE },
	{ "play", PLAY },
	{ "pause", PAUSE },
	{ "stop", STOP },
	{ "setLooping", SET_LOOPING },
	{ "setVolume", SET_VOLUME },
	{ "reportAchievementProgress", REPORT_ACHIEVEMENT_PROGRESS },
};

int GetMethodID(void *env, void *class, const char *name, const char *sig) {
	printf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0) {
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig) {
	//printf("GetStaticMethodID: %s\n", name);
	
	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++) {
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

char *ach_name[] = {
	"scavenger",
	"beammeupscotty",
	"angrybats",
	"blastabat",
	"golddigger",
	"thedestroyer",
	"thegardener",
	"recklessshooting",
	"bombabat",
	"mishandlingexplosives",
	"pancaketime",
	"animatedearth",
	"lostsword",
	"theundead",
	"lawandjustice",
	"likeaninja",
	"duel",
	"anotherentrance",
	"lordoftheundead",
	"toofast",
	"treasurehunter",
	"deadlymage",
	"toorich",
	"takingtheshortcut",
	"indianajones",
	"shatteredblade",
	"theexplorer",
	"masteroforder",
};

void unlock_achievement(char *name, int progress, int locked) {
	printf("unlock_achievement(%s, %x, %x)\n", name, progress, locked);
	
	if (!locked && progress > 0) {
		for (int i = 0; i < sizeof(ach_name) / sizeof(*ach_name); i++) {
			if (!strcmp(ach_name[i], name)) {
				trophies_unlock(i + 1);
				break;
			}
		}
	}
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case REPORT_ACHIEVEMENT_PROGRESS:
		unlock_achievement(args[0], args[1], args[2]);
		break;
	default:
		break;
	}
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case IS_AGE_KNOWN:
	case GET_PLATFORM_CONSENT_STATE:
		return 1;
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;	
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return -1;
}

void *FindClass(void) {
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str) {
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str) {
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args) {
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj) {
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes) {
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy) {
	return string;
}

size_t GetStringUTFLength(void *env, char *string) {
	return strlen(string);	
}

int GetJavaVM(void *env, void **vm) {
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
	printf("GetFieldID %s\n", name);
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID) {
	return 1;
}

void *GetObjectArrayElement(void *env, uint8_t *obj, int idx) {
	return NULL;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	case LOAD_FILE:
		//printf("load file %s\n", args[0]);
		load_music(args[0]);
		return 1;
	default:
		return 0;
	}
}

void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0x34343434;
	}
}

int CallIntMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, va_list args) {
	float farg;
	int iarg;
	
	switch (methodID) {
	case PAUSE:
		//printf("pause\n");
		pause_music();
		break;
	case PLAY:
		//printf("play\n");
		play_music();
		break;
	case SET_LOOPING:
		//printf("set looping\n");
		iarg = va_arg(args, int);
		set_music_loop(iarg);
		break;
	case SET_VOLUME:
		farg = va_arg(args, double);
		set_music_volume(farg);
		break;
	case STOP:
		//printf("stop\n");
		stop_music();
		break;
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
	printf("Static Field %s\n", name);
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID) {
	switch (fieldID) {
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf) {
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID) {
	switch (fieldID) {
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args) {
	switch (methodID) {
	default:
		if (methodID != UNKNOWN) {
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetArrayLength(void *env, void *array) {
	return 0;
}

/*int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}*/

typedef struct lua_longjmp {
	struct lua_longjmp *previous;
	int b;
	volatile int status;  /* error code */
} lua_longjmp;

so_hook throw_hook, catch_hook, rethrow_hook;
void __cxa_throw_hook(void *thrown_exception, void *tinfo, void (*dest)(void *)) {
	printf("throw called on %p\n", __builtin_return_address(0));
	
	if (tinfo == so_symbol(&main_mod, "_ZTIP11lua_longjmp")) {
		lua_longjmp *jmp = *(lua_longjmp **)thrown_exception;
		printf("lua_longjmp %x\n", thrown_exception);
		printf("status: %x\n", jmp->status);
		printf("previous: %x\n", jmp->previous);
		printf("b: %x\n", jmp->b);
	}
	SO_CONTINUE(int, throw_hook, thrown_exception, tinfo, dest);
}

void __cxa_rethrow_hook() {
	printf("rethrow called on %p\n", __builtin_return_address(0));
	SO_CONTINUE(int, rethrow_hook);
}

void __cxa_begin_catch_hook(void *obj) {
	printf("begin_catch called on %p\n", __builtin_return_address(0));
	SO_CONTINUE(int, catch_hook, obj);
}

void lexical_cast_traceback() {
	printf("lexical_cast called on %p\n", __builtin_return_address(0));
}

extern void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *));
extern void *__cxa_begin_catch;
extern void *__cxa_end_catch;
extern void *__cxa_rethrow;

void patch_game(void) {
	hook_addr(so_symbol(&main_mod, "_ZN5Caver24OnlineController_Android18ShowInterstitialAdERKSsif"), (uintptr_t)&ret0);
}

void *GetIntArrayElements(void *env, void *obj) {
	return NULL;
}

void SetIntArrayRegion(void *env, void *array, size_t start, size_t len, int *buf) {
	printf("SetIntArrayRegion: %d %d %x\n", start, len, buf);
}

void *real_main(void *argv) {
	srand(time(NULL));
	
	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(SceAppUtilInitParam));
	memset(&boot_param, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&init_param, &boot_param);
	
	printf("Booting...\n");
	//sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	//SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	//sceKernelStartThread(crasher_thread, 0, NULL);	
	
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error libshacccg.suprx is not installed.");
	
	char fname[256];
	sprintf(data_path, "ux0:data/swordigo");
	
	printf("Loading libswordigo\n");
	sprintf(fname, "%s/libswordigo.so", data_path);
	if (so_file_load(&main_mod, fname, LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&main_mod);
	so_resolve(&main_mod, default_dynlib, sizeof(default_dynlib), 0);	
	patch_game();
	so_flush_caches(&main_mod);
	so_initialize(&main_mod);
	
	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x38) = (uintptr_t)ret0; // ThrowNew
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
	*(uintptr_t *)(fake_env + 0xD0) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B4) = (uintptr_t)GetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x2CC) = (uintptr_t)ret0; //NewIntArray;
	*(uintptr_t *)(fake_env + 0x2EC) = (uintptr_t)GetIntArrayElements;
	*(uintptr_t *)(fake_env + 0x30C) = (uintptr_t)ret0; // ReleaseIntArrayElements;
	*(uintptr_t *)(fake_env + 0x34C) = (uintptr_t)SetIntArrayRegion;
	*(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)ret0; // RegisterNatives
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
	
	void (*setFilesDir)(void *env, int obj, char *path) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_setFilesDir");
	void (*setCacheDir)(void *env, int obj, char *path) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_setCacheDir");
	void (*setAssetManager)(void *env, int obj, void *asset_manager) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_setAssetManager");
	void (*drawApplication)(void *env, int obj) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_drawApplication");
	void (*setupNativeInterface)(void *env, int obj) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_setupNativeInterface");
	void (*setupApplication)(void *env, int obj) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_setupApplication");
	void (*setApplicationViewSize)(void *env, int obj, int w, int h, int is_pad) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_setApplicationViewSize");
	void (*handleApplicationLaunch)(void *env, int obj) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_handleApplicationLaunch");
	void (*applicationDidBecomeActive)(void *env, int obj) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_applicationDidBecomeActive");
	void (*updateApplication)(void *env, int obj, float tickrate) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_updateApplication");
	void (*initMusicPlayer)(void *env, int obj) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_MusicPlayer_initMusicPlayer");
	void (*handleTouchEvent)(void *env, int obj, int i, int id, double time, float x, float y, float old_x, float old_y, int tap_count) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_handleTouchEvent");
	void (*googleSignInCompleted)(void *env, int obj, uint8_t logged) = (void *)so_symbol(&main_mod, "Java_com_touchfoo_swordigo_Native_googleSignInCompleted");

	vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, 0, 0, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
	
	// Initing trophy system
	SceIoStat st;
	int r = trophies_init();
	if (r < 0 && sceIoGetstat(TROPHIES_FILE, &st) < 0) {
		FILE *f = fopen(TROPHIES_FILE, "w");
		fclose(f);
		warning("This game features unlockable trophies but NoTrpDrm is not installed. If you want to be able to unlock trophies, please install it.");
	}
	
	printf("setFilesDir\n");
	setFilesDir(&fake_env, 0, "ux0:data/swordigo");
	
	printf("setCacheDir\n");
	setCacheDir(&fake_env, 0, "ux0:data/swordigo");
	
	printf("setAssetManager\n");
	setAssetManager(&fake_env, 0, NULL);
	
	printf("googleSignInCompleted\n");
	googleSignInCompleted(&fake_env, 0, 0);
	
	printf("handleApplicationLaunch\n");
	handleApplicationLaunch(&fake_env, 0);
	
	glClearDepthf(1.0f);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glViewport(0, 0, SCREEN_W, SCREEN_H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	
	printf("initMusicPlayer\n");
	init_soloud();
	initMusicPlayer(&fake_env, 0);
	
	printf("setupNativeInterface\n");
	setupNativeInterface(&fake_env, 0);
	
	printf("setupApplication\n");
	setupApplication(&fake_env, 0);
	
	printf("setApplicationViewSize\n");
	setApplicationViewSize(&fake_env, 0, SCREEN_W, SCREEN_H, 1);
	
	printf("applicationDidBecomeActive\n");
	applicationDidBecomeActive(&fake_env, 0);

	enum {
		TOUCH_BEGAN = 1,
		TOUCH_ENDED,
		TOUCH_CANCELLED,
		TOUCH_MOVED
	};

	const float tickRate = 1.0f / sceRtcGetTickResolution();
	SceRtcTick lastTick;
	sceRtcGetCurrentTick(&lastTick);
		
	double time = 0.0f;
	uint32_t oldpad;
	printf("Entering main loop\n");
	float lastX[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
	float lastY[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};

	for (;;) {
		SceTouchData touch;
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
		for (int i = 0; i < SCE_TOUCH_MAX_REPORT; i++) {
			if (i < touch.reportNum) {
				float x = (float)touch.report[i].x * 0.5f;
				float y = 544.0f - (float)touch.report[i].y * 0.5f;

				if (lastX[i] == -1 || lastY[i] == -1)
					handleTouchEvent(&fake_env, 0, TOUCH_BEGAN, i + 1, time, x, y, x, y, 1);
				else
					handleTouchEvent(&fake_env, 0, TOUCH_MOVED, i + 1, time, x, y, (float)lastX[i], (float)lastY[i], 0);
				lastX[i] = x;
				lastY[i] = y;
			} else {
				if (lastX[i] != -1 || lastY[i] != -1) {
					handleTouchEvent(&fake_env, 0, TOUCH_ENDED, i + 1, time, (float)lastX[i], (float)lastY[i], (float)lastX[i], (float)lastY[i], 0);
					lastX[i] = -1;
					lastY[i] = -1;
				}
			}
		}
		
		#define fakeInput(btn, x, y, id) \
			if ((pad.buttons & btn) == btn) { \
				if ((oldpad & btn) == btn) { \
					handleTouchEvent(&fake_env, 0, TOUCH_MOVED, id, time, x, y, x, y, 0); \
				} else { \
					handleTouchEvent(&fake_env, 0, TOUCH_BEGAN, id, time, x, y, x, y, 1); \
				} \
			} else if ((oldpad & btn) == btn) { \
				handleTouchEvent(&fake_env, 0, TOUCH_ENDED, id, time, x, y, x, y, 0); \
			}
		
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		fakeInput(SCE_CTRL_CROSS, 900.0f, 94.0f, 5) // Jump
		fakeInput(SCE_CTRL_LEFT, 60.0f, 94.0f, 6) // Move left
		fakeInput(SCE_CTRL_RIGHT, 155.0f, 94.0f, 7) // Move right
		fakeInput(SCE_CTRL_SQUARE, 790.0f, 94.0f, 8) // Attack
		fakeInput(SCE_CTRL_START, 48.0f, 500.0f, 9) // Menu
		fakeInput(SCE_CTRL_CIRCLE, 900.0f, 184.0f, 10) // Magic
		fakeInput(SCE_CTRL_SELECT, 900.0f, 500.0f, 11) // Magic Equip
		fakeInput(SCE_CTRL_TRIANGLE, 425.0f, 54.0f, 12) // Use Item
		oldpad = pad.buttons;
		
		
		SceRtcTick tick;
		sceRtcGetCurrentTick(&tick);
		const unsigned int deltaTick = tick.tick - lastTick.tick;
		float deltaSecond = deltaTick * tickRate;
		lastTick.tick = tick.tick;
		
		// Prevent player from falling out of bounds when transitioning between stages
		if (deltaSecond > 0.1f)
			deltaSecond = 0.016666668f;
			
		time += deltaSecond;
		updateApplication(&fake_env, 0, deltaSecond);
		drawApplication(&fake_env, 0);
		
		vglSwapBuffers(GL_FALSE);
	}
}

int __real_sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode);
int __wrap_sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) {
	if (len == 1024) {
		type = SCE_AUDIO_OUT_PORT_TYPE_VOICE;
	}
	return __real_sceAudioOutOpenPort(type, len, freq, mode);
}

int main(int argc, char *argv[]) {
	al_device = alcOpenDevice(NULL);
	int attrlist[6];
	attrlist[0] = ALC_FREQUENCY;
	attrlist[1] = 48000;
	attrlist[2] = ALC_SYNC;
	attrlist[3] = AL_FALSE;
	attrlist[4] = 0;

	al_context_id = alcCreateContext(al_device, attrlist);
	
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 0x400000);
	pthread_create(&t, &attr, real_main, NULL);

	return sceKernelExitDeleteThread(0);
}
