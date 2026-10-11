/* claim_inject.c —— 临时资源"认领 / 身份 / 回滚"路径的测试专用系统调用注入。
 *
 * 为什么必须注入：产品先做一遍同名冲突检查（fstatat），之后才用 mkdirat 排他
 * 认领；而"认领成功、身份却取不到"更是纯文件系统事实做不到的中间态。这里刻意
 * **不改产品代码**（不加测试后门），改用 LD_PRELOAD 拦截系统调用。
 *
 * 规则（严格受限，避免把夹具的问题算成产品问题）：
 *   * 每个动作只命中一次，命中后失效；目标文件内容变化时重新武装；
 *   * 命中的都是产品**真正会调用**的那一个名字 / 路径（由测试写进目标文件）；
 *   * 放置受保护对象用的是真实系统调用，产品看到的 errno 也来自真实语义；
 *   * 只对被测进程设置 LD_PRELOAD，绝不导出到整轮构建或别的进程。
 *
 * 目标文件（环境变量 R9_CLAIM_INJECT_FILE）每行一条指令，均可省略：
 *   protect        <绝对路径>  要放置的受保护目录（内含 precious.txt 哨兵）
 *   mkdirat-eexist <序数> <名字>  第 <序数> 次 mkdirat(<名字>)：放置受保护对象，
 *                                 返回 EEXIST
 *   mkdirat-swap   <序数> <名字>  第 <序数> 次 mkdirat(<名字>)：真正的 mkdirat
 *                                 **成功**，随即把该名字换成受保护对象并返回 0；
 *                                 同时武装"紧随其后的那一次 fstatat(<名字>)
 *                                 必须失败"
 *   mkdir-race     <序数> <路径>  第 <序数> 次 mkdir(<路径>)：先由"别人"把它建
 *                                 出来，再照常调用真正的 mkdir（产品拿到 EEXIST）；
 *                                 给了 protect 就同时把受保护对象放进去
 *   fstatat-errno  <十进制>    武装的那次 fstatat 失败时用的 errno（默认 ENOENT）
 *
 * 为什么要有序数：同一个名字在产品里有**多处**认领调用点（进入时一次、每个
 * delta 之前再一次），只按名字匹配会命中第一处。序数就是"这个名字的第几次调用"，
 * 是夹具里唯一能把各调用点区分开的判据 —— 测试因此可以逐点注入，而不是笼统地
 * "注入一次看看"。序数没命中（产品没走到那一处）时 state 文件不会生成，测试会
 * 如实失败，不会静默放过。
 *
 * 每个动作把放置出来的对象的 dev/ino/mode 记到 <目标文件>.state.<动作>
 * （swap / eexist / race），供测试在恢复之后逐项比对"没有被改动"。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TARGET_MAX 4096
#define BUFFER_MAX 8192

#define SENTINEL_TEXT "USER-DATA-MUST-SURVIVE\n"

static char g_protect[TARGET_MAX];
static char g_eexist[TARGET_MAX];
static char g_swap[TARGET_MAX];
static char g_race[TARGET_MAX];
static char g_prev_protect[TARGET_MAX];
static char g_prev_eexist[TARGET_MAX];
static char g_prev_swap[TARGET_MAX];
static char g_prev_race[TARGET_MAX];
static int g_eexist_ordinal = 1;
static int g_swap_ordinal = 1;
static int g_race_ordinal = 1;
static int g_prev_eexist_ordinal = 1;
static int g_prev_swap_ordinal = 1;
static int g_prev_race_ordinal = 1;
static int g_seen_eexist = 0;
static int g_seen_swap = 0;
static int g_seen_race = 0;
static int g_fstatat_errno = ENOENT;
static int g_prev_fstatat_errno = ENOENT;
static int g_fired_eexist = 0;
static int g_fired_swap = 0;
static int g_fired_race = 0;
static int g_fired_fstatat = 0;
static int g_armed_fstatat = 0;
static int g_loading = 0;
static int g_resolved = 0;

static int (*g_real_mkdir)(const char*, mode_t) = NULL;
static int (*g_real_mkdirat)(int, const char*, mode_t) = NULL;
static int (*g_real_fstatat)(int, const char*, struct stat*, int) = NULL;

static void copy_into(char* dst, size_t dst_size, const char* src) {
  size_t i = 0;
  for (; i + 1 < dst_size && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

/* dlsym 返回 void*；用 *(void**)&fn 的写法取真实符号，避免
 * "ISO C forbids conversion of object pointer to function pointer" 这类告警。 */
static void resolve_reals(void) {
  if (g_resolved) {
    return;
  }
  g_resolved = 1;
  *(void**)(&g_real_mkdir) = dlsym(RTLD_NEXT, "mkdir");
  *(void**)(&g_real_mkdirat) = dlsym(RTLD_NEXT, "mkdirat");
  *(void**)(&g_real_fstatat) = dlsym(RTLD_NEXT, "fstatat");
}

/* "<序数> <名字>"：序数是"这个名字的第几次调用"，缺省（或写 0）算第 1 次。 */
static void parse_ordinal(const char* text, int* ordinal, char* name,
                          size_t name_size) {
  int value = 0;
  const char* cursor = text;
  while (*cursor >= '0' && *cursor <= '9') {
    value = value * 10 + (*cursor - '0');
    ++cursor;
  }
  if (*cursor == ' ') {
    ++cursor;
  }
  *ordinal = (value > 0) ? value : 1;
  copy_into(name, name_size, cursor);
}

/* 每次拦截都重新读目标文件：测试在几个场景之间会改写它。目标集合一旦变化就
 * **重新武装**，于是"每个场景恰好注入一次"在同一个进程里也能成立。 */
static void load_targets(void) {
  char protect[TARGET_MAX];
  char eexist[TARGET_MAX];
  char swap[TARGET_MAX];
  char race[TARGET_MAX];
  int eexist_ordinal = 1;
  int swap_ordinal = 1;
  int race_ordinal = 1;
  int fail_errno = ENOENT;
  if (g_loading) {
    return;
  }
  g_loading = 1;
  protect[0] = '\0';
  eexist[0] = '\0';
  swap[0] = '\0';
  race[0] = '\0';
  const char* file = getenv("R9_CLAIM_INJECT_FILE");
  if (file != NULL && file[0] != '\0') {
    FILE* handle = fopen(file, "r");
    if (handle != NULL) {
      char line[BUFFER_MAX];
      while (fgets(line, sizeof(line), handle) != NULL) {
        char* newline = strchr(line, '\n');
        if (newline != NULL) {
          *newline = '\0';
        }
        if (strncmp(line, "protect ", 8) == 0) {
          copy_into(protect, sizeof(protect), line + 8);
        } else if (strncmp(line, "mkdirat-eexist ", 15) == 0) {
          parse_ordinal(line + 15, &eexist_ordinal, eexist, sizeof(eexist));
        } else if (strncmp(line, "mkdirat-swap ", 13) == 0) {
          parse_ordinal(line + 13, &swap_ordinal, swap, sizeof(swap));
        } else if (strncmp(line, "mkdir-race ", 11) == 0) {
          parse_ordinal(line + 11, &race_ordinal, race, sizeof(race));
        } else if (strncmp(line, "fstatat-errno ", 14) == 0) {
          fail_errno = atoi(line + 14);
        }
      }
      fclose(handle);
    }
  }
  g_loading = 0;
  if (strcmp(protect, g_prev_protect) != 0 ||
      strcmp(eexist, g_prev_eexist) != 0 || strcmp(swap, g_prev_swap) != 0 ||
      strcmp(race, g_prev_race) != 0 || fail_errno != g_prev_fstatat_errno ||
      eexist_ordinal != g_prev_eexist_ordinal ||
      swap_ordinal != g_prev_swap_ordinal ||
      race_ordinal != g_prev_race_ordinal) {
    copy_into(g_prev_protect, sizeof(g_prev_protect), protect);
    copy_into(g_prev_eexist, sizeof(g_prev_eexist), eexist);
    copy_into(g_prev_swap, sizeof(g_prev_swap), swap);
    copy_into(g_prev_race, sizeof(g_prev_race), race);
    copy_into(g_protect, sizeof(g_protect), protect);
    copy_into(g_eexist, sizeof(g_eexist), eexist);
    copy_into(g_swap, sizeof(g_swap), swap);
    copy_into(g_race, sizeof(g_race), race);
    g_prev_fstatat_errno = fail_errno;
    g_fstatat_errno = fail_errno;
    g_prev_eexist_ordinal = eexist_ordinal;
    g_prev_swap_ordinal = swap_ordinal;
    g_prev_race_ordinal = race_ordinal;
    g_eexist_ordinal = eexist_ordinal;
    g_swap_ordinal = swap_ordinal;
    g_race_ordinal = race_ordinal;
    g_seen_eexist = 0;
    g_seen_swap = 0;
    g_seen_race = 0;
    g_fired_eexist = 0;
    g_fired_swap = 0;
    g_fired_race = 0;
    g_fired_fstatat = 0;
    g_armed_fstatat = 0;
  }
}

/* 把放置出来的对象身份写进 <目标文件>.state.<动作>。 */
static void write_state(const char* action, const struct stat* info) {
  const char* file = getenv("R9_CLAIM_INJECT_FILE");
  char path[BUFFER_MAX];
  if (file == NULL || file[0] == '\0') {
    return;
  }
  snprintf(path, sizeof(path), "%s.state.%s", file, action);
  FILE* handle = fopen(path, "w");
  if (handle == NULL) {
    return;
  }
  fprintf(handle, "%llu %llu %o\n", (unsigned long long)info->st_dev,
          (unsigned long long)info->st_ino,
          (unsigned)(info->st_mode & 07777));
  fclose(handle);
}

/* 相对一个已经打开的目录 fd 放置受保护目录 + 哨兵。
 * action 是 state 文件后缀，label 是给人看 / 给脚本 grep 的指令名。 */
static void place_protected_at(int dirfd, const char* name, const char* action,
                               const char* label) {
  char dir[BUFFER_MAX];
  /* 比 dir 多留一截，好让编译器能证明 "/precious.txt" 一定塞得下
   * （-Wformat-truncation 在两者同为 BUFFER_MAX 时会如实报"可能被截断"）。 */
  char sentinel[BUFFER_MAX + 64];
  struct stat info;
  if (mkdirat(dirfd, name, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "[claiminject] cannot create protected dir %s: %s\n", name,
            strerror(errno));
    return;
  }
  snprintf(dir, sizeof(dir), "/proc/self/fd/%d/%s", dirfd, name);
  snprintf(sentinel, sizeof(sentinel), "%s/precious.txt", dir);
  FILE* handle = fopen(sentinel, "w");
  if (handle != NULL) {
    fputs(SENTINEL_TEXT, handle);
    fclose(handle);
  }
  if (fstatat(dirfd, name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
    write_state(action, &info);
  }
  fprintf(stderr,
          "[claiminject] FIRED: %s placed protected dir + sentinel at %s\n",
          label, dir);
}

/* 按绝对路径放置受保护目录 + 哨兵。 */
static void place_protected_abs(const char* absolute, const char* action,
                                const char* label) {
  char sentinel[BUFFER_MAX + 64];
  struct stat info;
  if (mkdir(absolute, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "[claiminject] cannot create protected dir %s: %s\n",
            absolute, strerror(errno));
    return;
  }
  snprintf(sentinel, sizeof(sentinel), "%s/precious.txt", absolute);
  FILE* handle = fopen(sentinel, "w");
  if (handle != NULL) {
    fputs(SENTINEL_TEXT, handle);
    fclose(handle);
  }
  if (stat(absolute, &info) == 0) {
    write_state(action, &info);
  }
  fprintf(stderr,
          "[claiminject] FIRED: %s placed protected dir + sentinel at %s\n",
          label, absolute);
}

int mkdirat(int dirfd, const char* path, mode_t mode) {
  resolve_reals();
  load_targets();
  if (g_eexist[0] != '\0' && strcmp(path, g_eexist) == 0) {
    ++g_seen_eexist;
  }
  if (!g_fired_eexist && g_eexist[0] != '\0' && strcmp(path, g_eexist) == 0 &&
      g_seen_eexist == g_eexist_ordinal) {
    g_fired_eexist = 1;
    place_protected_at(dirfd, path, "eexist", "mkdirat-eexist");
    errno = EEXIST;
    return -1;
  }
  if (g_swap[0] != '\0' && strcmp(path, g_swap) == 0) {
    ++g_seen_swap;
  }
  if (!g_fired_swap && g_swap[0] != '\0' && strcmp(path, g_swap) == 0 &&
      g_seen_swap == g_swap_ordinal) {
    g_fired_swap = 1;
    const int code = g_real_mkdirat(dirfd, path, mode);
    if (code == 0) {
      /* 产品的排他创建**真的成功了**；紧接着这个名字被换成别人的对象。 */
      if (unlinkat(dirfd, path, AT_REMOVEDIR) == 0) {
        place_protected_at(dirfd, path, "swap", "mkdirat-swap");
        g_armed_fstatat = 1;
      }
    }
    return code;
  }
  return g_real_mkdirat(dirfd, path, mode);
}

int fstatat(int dirfd, const char* path, struct stat* info, int flags) {
  resolve_reals();
  load_targets();
  if (g_armed_fstatat && !g_fired_fstatat && g_swap[0] != '\0' &&
      strcmp(path, g_swap) == 0) {
    g_fired_fstatat = 1;
    fprintf(stderr,
            "[claiminject] FIRED: fstatat-fail identity probe on %s -> %s\n",
            path, strerror(g_fstatat_errno));
    errno = g_fstatat_errno;
    return -1;
  }
  return g_real_fstatat(dirfd, path, info, flags);
}

int mkdir(const char* path, mode_t mode) {
  resolve_reals();
  load_targets();
  if (g_race[0] != '\0' && strcmp(path, g_race) == 0) {
    ++g_seen_race;
  }
  if (!g_fired_race && g_race[0] != '\0' && strcmp(path, g_race) == 0 &&
      g_seen_race == g_race_ordinal) {
    g_fired_race = 1;
    /* 先由"别的进程"把它建出来：产品随后那次 mkdir 会拿到真实的 EEXIST。 */
    if (g_real_mkdir(path, mode) != 0 && errno != EEXIST) {
      fprintf(stderr, "[claiminject] race mkdir %s failed: %s\n", path,
              strerror(errno));
    }
    if (g_protect[0] != '\0') {
      place_protected_abs(g_protect, "race", "mkdir-race");
    } else {
      fprintf(stderr, "[claiminject] FIRED: mkdir-race created %s\n", path);
    }
  }
  return g_real_mkdir(path, mode);
}
