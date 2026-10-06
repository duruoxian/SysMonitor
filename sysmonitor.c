/* ============================================================
 * SysMonitor — 透明可拖动的桌面系统监视器
 * 纯 C + X11 + FreeType 实现，无 fontconfig/Electron/Python 依赖，
 * 常驻内存约 5MB (< 10MB)。
 *
 * 功能：CPU / 内存 / 交换分区 / 磁盘(/) / 网络 / 开机时长 / 时钟
 *   · 背景透明 (ARGB 视觉 + 合成器合成，客户端自己写像素)
 *   · 蓝色字体 (FreeType 抗锯齿，直接混合进 32 位 ARGB 帧)
 *   · 左键按住拖动任意移动，位置自动记忆
 *   · 右键菜单：暂停/恢复刷新、退出
 *   · 开机自启动 (配合 ~/.config/autostart/sysmonitor.desktop)
 *
 * 构建: make        安装: sudo make install autostart
 * ============================================================ */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/select.h>

#define APP       "SysMonitor"
#define PAD       12          /* 内边距(像素) */
#define BAR_H     3           /* 进度条高度 */
#define FS_MAIN   16          /* 字号(px) */
#define PLATE_A   60          /* 背板透明度 0~255，0=完全透明，60≈24% 暗色薄纱 */
#define ALWAYS_ON_TOP 0      /* 1=永远置顶盖住其他窗口；0=钉在桌面，可被窗口覆盖(桌面组件风格) */
#define GCAP      256         /* 字形缓存容量 */

/* ---------------- 全局状态 ---------------- */
static Display *dpy;  static int scr, depth, xfd;
static Visual *vis;   static Colormap cmap;
static Window root, win, mwin = 0;
static XImage *ximg = NULL, *ximg_m = NULL;
static uint32_t *fb = NULL, *fb_m = NULL;   /* 客户端 ARGB 帧缓冲(预乘 alpha) */
static GC gc_copy;
static Cursor cur_mv;
static Atom wm_del_atom;
static Atom atom_wmstate, atom_above, atom_below, atom_sticky, atom_skip_tb, atom_skip_pg;
static int W = 240, H = 100;
static int menu_w, menu_h, mi_h;
static int px = 60, py = 60;
static volatile sig_atomic_t g_quit = 0;
static int paused = 0, drag = 0, ddx = 0, ddy = 0;
static int menu_open = 0, menu_hi = -1;
static int g_cjk = 0;

/* 界面文字（根据字体是否含汉字自动选择） */
static const char *L_mem, *L_swap, *L_net, *L_up, *L_na, *L_pz, *L_rs, *L_qt, *L_psd;
static const char *AR_UP = "↑", *AR_DN = "↓";

/* 系统数据 */
static double cpu_pct = -1, net_up = 0, net_dn = 0;
static unsigned long mtot, mavail, stot, sfree;        /* kB */
static unsigned long long up_s = 0;
static long c_idle = -1, c_tot = -1;
static unsigned long long p_rx, p_tx; static double p_t; static int p_has;

/* ---------------- 工具 ---------------- */
static double now_mono(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec/1e9; }

static void scan_disks(void);   /* 定义在采集函数之后 */

static void on_sig(int s){ (void)s; g_quit = 1; }

static void cfg_dir(char *out, size_t n){
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/.config/sysmonitor", h ? h : ".");
}
static void save_pos(void){
    char d[256], f[300];
    cfg_dir(d, sizeof d); mkdir(d, 0755);
    snprintf(f, sizeof f, "%s/pos", d);
    FILE *fp = fopen(f, "w");
    if (fp){ fprintf(fp, "%d %d\n", px, py); fclose(fp); }
}
static void load_pos(void){
    char d[256], f[300];
    cfg_dir(d, sizeof d); snprintf(f, sizeof f, "%s/pos", d);
    px = py = -1;
    FILE *fp = fopen(f, "r");
    if (fp){ if (fscanf(fp, "%d %d", &px, &py) != 2){ px = py = -1; } fclose(fp); }
}

static void fmt_kb(unsigned long kb, char *out){          /* 输入单位 kB, G 值保留一位小数 */
    if (kb >= 1048576UL)       sprintf(out, "%.1fG", kb/1048576.0);
    else if (kb >= 1024UL)     sprintf(out, "%.0fM", kb/1024.0);
    else                       sprintf(out, "%luK", kb);
}
static void fmt_bytes(unsigned long long by, char *out){
    if (by >= (1ULL<<30))      sprintf(out, "%.1fG", by/1073741824.0);
    else if (by >= (1ULL<<20)) sprintf(out, "%.0fM", by/1048576.0);
    else                       sprintf(out, "%lluK", by/1024);
}
static void fmt_spd(double bps, char *out){
    if      (bps >= 1073741824.0) sprintf(out, "%.1fGB/s", bps/1073741824.0);
    else if (bps >= 1048576.0)    sprintf(out, "%.1fMB/s", bps/1048576.0);
    else if (bps >= 1024.0)       sprintf(out, "%.0fKB/s", bps/1024.0);
    else                          sprintf(out, "%.0fB/s", bps);
}

/* ---------------- 系统数据采集 ---------------- */
static void read_stats(void){
    FILE *f;
    /* CPU */
    f = fopen("/proc/stat", "r");
    if (f){
        char b[512];
        if (fgets(b, sizeof b, f)){
            long v[10] = {0}; int n = 0; char *t = strtok(b, " ");
            while ((t = strtok(NULL, " ")) && n < 10) v[n++] = atol(t);
            long idle = v[3] + v[4], tot = 0;
            for (int i = 0; i < n; i++) tot += v[i];
            if (c_tot >= 0 && tot > c_tot){
                double di = idle - c_idle, dt = tot - c_tot;
                cpu_pct = (1.0 - di/dt) * 100.0;
                if (cpu_pct < 0) cpu_pct = 0;
                if (cpu_pct > 100) cpu_pct = 100;
            }
            c_idle = idle; c_tot = tot;
        }
        fclose(f);
    }
    /* 内存 / 交换 */
    f = fopen("/proc/meminfo", "r");
    if (f){
        char line[256], k[64]; unsigned long vb;
        while (fgets(line, sizeof line, f)){
            if (sscanf(line, "%63s %lu", k, &vb) == 2){
                if      (!strcmp(k, "MemTotal:"))     mtot   = vb;
                else if (!strcmp(k, "MemAvailable:")) mavail = vb;
                else if (!strcmp(k, "SwapTotal:"))    stot   = vb;
                else if (!strcmp(k, "SwapFree:"))     sfree  = vb;
            }
        }
        fclose(f);
    }
    /* 磁盘(/) 已由 scan_disks 统一处理 */
    /* 网络(汇总所有非 lo 接口) */
    f = fopen("/proc/net/dev", "r");
    if (f){
        char line[512]; unsigned long long rx = 0, tx = 0; int hdr = 0;
        while (fgets(line, sizeof line, f)){
            if (hdr++ < 2) continue;
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            char *nm = line; while (*nm == ' ') nm++;
            if (!strncmp(nm, "lo", 2) && (nm[2] == 0 || nm[2] == ' ')) continue;
            unsigned long long r = 0, t = 0;
            if (sscanf(colon+1, "%llu %*s %*s %*s %*s %*s %*s %*s %llu", &r, &t) == 2){ rx += r; tx += t; }
        }
        fclose(f);
        double t2 = now_mono();
        if (p_has && t2 > p_t){
            long long drx = (long long)(rx - p_rx), dtx = (long long)(tx - p_tx);
            if (drx < 0) drx = 0;
            if (dtx < 0) dtx = 0;
            net_dn = drx / (t2 - p_t);
            net_up = dtx / (t2 - p_t);
        }
        p_rx = rx; p_tx = tx; p_t = t2; p_has = 1;
    }
    /* 开机时长 */
    f = fopen("/proc/uptime", "r");
    if (f){ double up = 0; if (fscanf(f, "%lf", &up) == 1) up_s = (unsigned long long)up; fclose(f); }
    /* 所有真实磁盘分区 */
    scan_disks();
}

/* ---------------- 磁盘枚举 ---------------- */
typedef struct { char label[16]; char mnt[256]; unsigned long long tot, used; int pct; } Disk;
static Disk disks[8]; static int ndisks;

static const char *FS_OK[] = {
    "ext2","ext3","ext4","btrfs","xfs","f2fs","zfs","reiserfs","jfs",
    "ntfs","ntfs3","ntfs-3g","vfat","exfat","fuseblk","udf", NULL };

static void disk_label(const char *mnt, char *out, size_t n){
    if (!strcmp(mnt, "/")){ snprintf(out, n, "%s", g_cjk ? "系统" : "Root"); return; }
    const char *b = strrchr(mnt, '/'); b = b ? b + 1 : mnt;
    if (strlen(b) == 5 && !strncasecmp(b, "win_", 4) && isalnum((unsigned char)b[4])){
        snprintf(out, n, g_cjk ? "%c盘" : "%c:", toupper((unsigned char)b[4]));
        return;
    }
    snprintf(out, n, "%.10s", b);
}

static void scan_disks(void){
    ndisks = 0;
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[512], devs[8][64]; int ndev = 0;
    while (fgets(line, sizeof line, f) && ndisks < 8){
        char dev[64], mnt[256], fs[32];
        if (sscanf(line, "%63s %255s %31s", dev, mnt, fs) != 3) continue;
        if (strncmp(dev, "/dev/", 5)) continue;                    /* 只看真实块设备 */
        /* EFI 引导分区不占用显示行，想显示可删掉本行 */
        if (!strcmp(mnt, "/boot") || !strcmp(mnt, "/boot/efi") || !strcmp(mnt, "/efi")) continue;
        int ok = 0;
        for (int i = 0; FS_OK[i]; i++) if (!strcmp(fs, FS_OK[i])){ ok = 1; break; }
        if (!ok) continue;
        int dup = 0;
        for (int i = 0; i < ndev; i++) if (!strcmp(devs[i], dev)){ dup = 1; break; }
        if (dup) continue;                                          /* 同一设备多挂载点只显示一次 */
        struct statvfs s;
        if (statvfs(mnt, &s)) continue;
        unsigned long long fr = s.f_frsize ? s.f_frsize : s.f_bsize;
        unsigned long long tot = (unsigned long long)s.f_blocks * fr;
        unsigned long long av  = (unsigned long long)s.f_bavail * fr;
        if (!tot) continue;
        strcpy(devs[ndev++], dev);
        Disk *d = &disks[ndisks++];
        snprintf(d->mnt, sizeof d->mnt, "%s", mnt);
        disk_label(mnt, d->label, sizeof d->label);
        d->tot = tot; d->used = tot - av;
        d->pct = (int)(d->used * 100.0 / tot);
    }
    fclose(f);
}

/* ---------------- 字体 (FreeType 直读, 不经 fontconfig) ---------------- */
static FT_Library ft_lib;
static FT_Face face;
static int asc_px, lh_px;

static uint32_t u8_next(const char **pp){          /* UTF-8 → 码点 */
    const unsigned char *s = (const unsigned char*)*pp;
    uint32_t c; int n;
    if (!*s) return 0;
    if (*s < 0x80)           { c = *s & 0x7F; n = 1; }
    else if ((*s & 0xE0) == 0xC0){ c = *s & 0x1F; n = 2; }
    else if ((*s & 0xF0) == 0xE0){ c = *s & 0x0F; n = 3; }
    else                     { c = *s & 0x07; n = 4; }
    for (int i = 1; i < n; i++) c = (c << 6) | (s[i] & 0x3F);
    *pp += n;
    return c;
}

typedef struct { uint32_t cp; int w, h, bx, by, adv; unsigned char *bm; } Gly;
static Gly gcache[GCAP]; static int gcount = 0;

static const char *FONT_CANDIDATES[] = {
    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
    "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJKsc-Regular.otf",
    "/usr/share/fonts/adobe-source-han-sans/SourceHanSansCN-Regular.otf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    NULL
};

static int load_font(void){
    if (FT_Init_FreeType(&ft_lib)) return 0;
    for (int i = 0; FONT_CANDIDATES[i]; i++){
        if (access(FONT_CANDIDATES[i], R_OK) == 0 &&
            FT_New_Face(ft_lib, FONT_CANDIDATES[i], 0, &face) == 0)
            break;
        face = NULL;
    }
    if (!face) return 0;
    FT_Set_Pixel_Sizes(face, 0, FS_MAIN);
    asc_px = (int)((face->size->metrics.ascender + 32) >> 6);
    lh_px  = (int)((face->size->metrics.height  + 63) >> 6);
    if (asc_px <= 0) asc_px = FS_MAIN;
    if (lh_px  <= 0) lh_px  = asc_px + 4;

    g_cjk = FT_Get_Char_Index(face, 0x4E2D) != 0;   /* “中” */
    if (!FT_Get_Char_Index(face, 0x2191)){ AR_UP = "^"; AR_DN = "v"; }

    if (g_cjk){
        L_mem="内存"; L_swap="交换"; L_net="网络"; L_up="开机"; L_na="未启用";
        L_pz="暂停刷新"; L_rs="恢复刷新"; L_qt="退出 SysMonitor"; L_psd="已暂停";
    } else {
        L_mem="MEM"; L_swap="SWAP"; L_net="NET"; L_up="UP"; L_na="n/a";
        L_pz="Pause"; L_rs="Resume"; L_qt="Quit SysMonitor"; L_psd="paused";
    }
    return 1;
}

static Gly *get_glyph(uint32_t cp){
    for (int i = 0; i < gcount && i < GCAP; i++)
        if (gcache[i].cp == cp) return &gcache[i];
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) return NULL;
    FT_GlyphSlot g = face->glyph;
    FT_Bitmap *b = &g->bitmap;
    int slot;
    if (gcount < GCAP) slot = gcount++;
    else { slot = gcount % GCAP; gcount++; }
    Gly *e = &gcache[slot];
    free(e->bm);
    e->cp = cp;
    e->w = (int)b->width; e->h = (int)b->rows;
    e->bx = g->bitmap_left; e->by = g->bitmap_top;
    e->adv = (int)((g->advance.x + 32) >> 6);
    e->bm = NULL;
    if (e->w > 0 && e->h > 0){
        e->bm = malloc((size_t)e->w * e->h);
        if (!e->bm) return NULL;
        if (b->pixel_mode == FT_PIXEL_MODE_MONO){
            for (int y = 0; y < e->h; y++)
                for (int x = 0; x < e->w; x++){
                    int byte = b->buffer[y*b->pitch + x/8];
                    e->bm[y*e->w + x] = (byte >> (7 - x%8)) & 1 ? 255 : 0;
                }
        } else {
            int pitch = b->pitch < 0 ? -b->pitch : b->pitch;
            for (int y = 0; y < e->h; y++)
                memcpy(e->bm + y*e->w, b->buffer + y*pitch, (size_t)e->w);
        }
    }
    return e;
}

/* ---------------- 绘制 (预乘 alpha 混合) ---------------- */
typedef struct { int r, g, b; } Clr;
static const Clr CLR_TITLE = {  96, 171, 255 };  /* 标题亮蓝 */
static const Clr CLR_LAB   = {  64, 140, 255 };  /* 标签蓝 */
static const Clr CLR_VAL   = { 140, 200, 255 };  /* 数值浅蓝 */
static const Clr CLR_BAR   = {  30, 144, 255 };  /* 进度条/菜单高亮蓝 */
static const Clr CLR_LINE  = {  60, 150, 255 };  /* 菜单边框 */

static uint32_t *cur_fb; static int cur_w, cur_h;   /* 当前绘制目标 */

static void blend_px(int idx, const Clr *c, int cov){
    if (cov <= 0) return;
    uint32_t d = cur_fb[idx];
    int ia = 255 - cov;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, da = d >> 24;
    int nr = (c->r * cov + dr * ia + 127) / 255;
    int ng = (c->g * cov + dg * ia + 127) / 255;
    int nb = (c->b * cov + db * ia + 127) / 255;
    int na = (cov * 255 + da * ia + 127) / 255;
    cur_fb[idx] = ((uint32_t)na << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
}

static void fill_rect(int x, int y, int w, int h, const Clr *c, int a){
    if (x < 0){ w += x; x = 0; }
    if (y < 0){ h += y; y = 0; }
    if (x + w > cur_w) w = cur_w - x;
    if (y + h > cur_h) h = cur_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            blend_px((y + j) * cur_w + x + i, c, a);
}

static int text_w(const char *s){
    int x = 0;
    while (*s){
        const char *p = s;
        uint32_t cp = u8_next(&p);
        Gly *g = get_glyph(cp);
        if (g) x += g->adv;
        s = p;
    }
    return x;
}

static void draw_text(int x, int y, const char *s, const Clr *c){   /* y 为基线 */
    while (*s){
        const char *p = s;
        uint32_t cp = u8_next(&p);
        Gly *g = get_glyph(cp);
        if (g && g->bm){
            for (int j = 0; j < g->h; j++){
                int yy = y - g->by + j;
                if (yy < 0 || yy >= cur_h) continue;
                for (int i = 0; i < g->w; i++){
                    int xx = x + g->bx + i;
                    if (xx < 0 || xx >= cur_w) continue;
                    blend_px(yy * cur_w + xx, c, g->bm[j * g->w + i]);
                }
            }
        }
        if (g) x += g->adv;
        s = p;
    }
}

/* ---------------- 窗口 ---------------- */
static int layout_height(void){
    return 12 + (lh_px + 10) + (3 + ndisks)*(lh_px + 12) + 2*(lh_px + 8);
}
static void compute_layout(void){
    W = 240;
    H = layout_height();
}

/* 行数变化时调整帧缓冲与窗口高度 */
static void ensure_size(int nh){
    if (nh == H || !fb || !ximg) return;
    H = nh;
    XDestroyImage(ximg);                       /* 同时释放旧 fb */
    fb = calloc((size_t)W * H, 4);
    ximg = XCreateImage(dpy, vis, 32, ZPixmap, 0, (char*)fb, W, H, 32, W * 4);
    XResizeWindow(dpy, win, W, H);
    int dh = DisplayHeight(dpy, scr);
    if (py > dh - H){ py = dh - H; if (py < 0) py = 0; XMoveWindow(dpy, win, px, py); }
}

static void create_window(void){
    XVisualInfo vi;
    int have32 = (XMatchVisualInfo(dpy, scr, 32, TrueColor, &vi) != 0);  /* 成功返回非 0 */
    XSetWindowAttributes a;
    if (have32){
        depth = 32; vis = vi.visual;
        cmap = XCreateColormap(dpy, root, vis, AllocNone);
    } else {
        depth = DefaultDepth(dpy, scr); vis = DefaultVisual(dpy, scr); cmap = DefaultColormap(dpy, scr);
        fprintf(stderr, "SysMonitor: 警告 — 无 32 位 ARGB 视觉（可能无合成器），透明将退化为深色底\n");
    }
    a.colormap = cmap; a.background_pixel = 0; a.border_pixel = 0;
    a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | StructureNotifyMask;
    win = XCreateWindow(dpy, root, px, py, W, H, 0, depth, InputOutput, vis,
                        CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &a);
    gc_copy = XCreateGC(dpy, win, 0, NULL);
    cur_mv = XCreateFontCursor(dpy, XC_fleur);
    XDefineCursor(dpy, win, cur_mv);

    fb = calloc((size_t)W * H, 4);
    ximg = XCreateImage(dpy, vis, 32, ZPixmap, 0, (char*)fb, W, H, 32, W * 4);

    XStoreName(dpy, win, APP);
    XClassHint ch = { (char*)APP, (char*)APP };
    XSetClassHint(dpy, win, &ch);
    wm_del_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_del_atom, 1);

    /* 桌面部件属性 */
    atom_wmstate = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_above   = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    atom_below   = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
    atom_sticky  = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    atom_skip_tb = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    atom_skip_pg = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom t = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
#if ALWAYS_ON_TOP
    /* 面板层：盖住普通窗口，不进任务栏 */
    Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, win, t, XA_ATOM, 32, PropModeReplace, (unsigned char*)&dock, 1);
    Atom sts[2] = { atom_above, atom_sticky };
    XChangeProperty(dpy, win, atom_wmstate, XA_ATOM, 32, PropModeReplace, (unsigned char*)sts, 2);
#else
    /* 普通层 + 保持在最下：所有工作区可见、不进任务栏、可被其他窗口覆盖 */
    Atom normal = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(dpy, win, t, XA_ATOM, 32, PropModeReplace, (unsigned char*)&normal, 1);
    Atom sts[4] = { atom_below, atom_sticky, atom_skip_tb, atom_skip_pg };
    XChangeProperty(dpy, win, atom_wmstate, XA_ATOM, 32, PropModeReplace, (unsigned char*)sts, 4);
    /* 去掉 WM 标题栏/边框（Motif 无装饰提示） */
    {
        struct { unsigned long flags, functions, decorations, input_mode, status; } mwm = {0};
        mwm.flags = 1UL << 1;                  /* MWM_HINTS_DECORATIONS */
        mwm.decorations = 0;                   /* 无标题栏无边框 */
        Atom mwm_atom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
        XChangeProperty(dpy, win, mwm_atom, mwm_atom, 32, PropModeReplace,
                        (unsigned char*)&mwm, 5);
    }
#endif
    long pid = (long)getpid();
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)&pid, 1);
}

/* ---------------- 主窗口绘制 ---------------- */
static void draw_ra(const char *s, int y, int xr, const Clr *c){ draw_text(xr - text_w(s), y + asc_px, s, c); }

static void draw_row(int *y, const char *lab, const char *val, double pct){
    draw_text(PAD, *y + asc_px, lab, &CLR_LAB);
    draw_ra(val, *y, W - PAD, &CLR_VAL);
    if (pct >= 0){
        int bw = W - 2*PAD;
        fill_rect(PAD, *y + lh_px + 3, bw, BAR_H, &CLR_BAR, 70);
        fill_rect(PAD, *y + lh_px + 3, (int)(bw * pct / 100.0 + 0.5), BAR_H, &CLR_BAR, 225);
    }
    *y += lh_px + 12;
}

static void draw_plain(int *y, const char *lab, const char *val){
    draw_text(PAD, *y + asc_px, lab, &CLR_LAB);
    draw_ra(val, *y, W - PAD, &CLR_VAL);
    *y += lh_px + 8;
}

static void render(void){
    char s[128], a[48], b[48];
    ensure_size(layout_height());
    memset(fb, 0, (size_t)W * H * 4);
    cur_fb = fb; cur_w = W; cur_h = H;
    fill_rect(0, 0, W, H, &(Clr){6,10,26}, PLATE_A);     /* 极淡深蓝背板 */

    int y = 6;
    draw_text(PAD, y + asc_px, APP, &CLR_TITLE);
    if (paused) draw_text(PAD + text_w(APP) + 5, y + asc_px, L_psd, &CLR_LAB);
    time_t tt = time(NULL); struct tm tmv; localtime_r(&tt, &tmv);
    snprintf(s, sizeof s, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    draw_ra(s, y, W - PAD, &CLR_LAB);
    y += lh_px + 10;

    if (cpu_pct < 0) snprintf(s, sizeof s, "--");
    else             snprintf(s, sizeof s, "%d%%", (int)(cpu_pct + 0.5));
    draw_row(&y, "CPU", s, cpu_pct);

    unsigned long mused = mtot > mavail ? mtot - mavail : 0;
    fmt_kb(mused, a); fmt_kb(mtot, b);
    snprintf(s, sizeof s, "%s/%s", a, b);
    draw_row(&y, L_mem, s, mtot ? (double)mused * 100.0 / mtot : 0.0);

    if (stot){
        unsigned long sused = stot > sfree ? stot - sfree : 0;
        fmt_kb(sused, a); fmt_kb(stot, b);
        snprintf(s, sizeof s, "%s/%s", a, b);
        draw_row(&y, L_swap, s, (double)sused * 100.0 / stot);
    } else draw_row(&y, L_swap, L_na, -1);

    /* 每个真实磁盘分区一行：系统盘、C盘、D盘…… */
    for (int i = 0; i < ndisks; i++){
        Disk *d = &disks[i];
        fmt_bytes(d->used, a); fmt_bytes(d->tot, b);
        snprintf(s, sizeof s, "%s/%s", a, b);
        draw_row(&y, d->label, s, d->pct);
    }

    fmt_spd(net_up, a); fmt_spd(net_dn, b);
    snprintf(s, sizeof s, "%s %s  %s %s", AR_UP, a, AR_DN, b);
    draw_plain(&y, L_net, s);

    unsigned long long u = up_s;
    if      (u >= 86400) snprintf(s, sizeof s, g_cjk ? "%llu天 %02llu:%02llu" : "%llud %02llu:%02llu",
                                  u/86400, (u%86400)/3600, (u%3600)/60);
    else if (u >= 3600)  snprintf(s, sizeof s, g_cjk ? "%llu小时%02llu分" : "%lluh %02llum",
                                  u/3600, (u%3600)/60);
    else                 snprintf(s, sizeof s, g_cjk ? "%llu分钟" : "%llumin", u/60);
    draw_plain(&y, L_up, s);

    XPutImage(dpy, win, gc_copy, ximg, 0, 0, 0, 0, W, H);
}

/* ---------------- 右键菜单 ---------------- */
static void menu_activate(int i){
    if (i == 0) paused = !paused;
    else if (i == 1) g_quit = 1;
}
static void menu_close(int act){
    if (act >= 0) menu_activate(act);
    XUngrabPointer(dpy, CurrentTime);
    XUnmapWindow(dpy, mwin);
    menu_open = 0; menu_hi = -1;
    XFlush(dpy);
}
static int menu_hit_at(int x, int y){
    if (y < 5 || x < 0 || x >= menu_w || y >= menu_h - 5) return -1;
    int i = (y - 5) / mi_h;
    return (i >= 0 && i < 2) ? i : -1;
}
static void render_menu(void){
    memset(fb_m, 0, (size_t)menu_w * menu_h * 4);
    cur_fb = fb_m; cur_w = menu_w; cur_h = menu_h;
    fill_rect(0, 0, menu_w, menu_h, &(Clr){8,14,34}, 235);
    fill_rect(0, 0, menu_w, 1, &CLR_LINE, 180);
    fill_rect(0, menu_h-1, menu_w, 1, &CLR_LINE, 180);
    fill_rect(0, 0, 1, menu_h, &CLR_LINE, 180);
    fill_rect(menu_w-1, 0, 1, menu_h, &CLR_LINE, 180);
    const char *items[2] = { paused ? L_rs : L_pz, L_qt };
    for (int i = 0; i < 2; i++){
        if (i == menu_hi) fill_rect(2, 5 + i*mi_h, menu_w - 4, mi_h, &CLR_BAR, 60);
        draw_text(12, 5 + i*mi_h + (mi_h - lh_px)/2 + asc_px, items[i], &CLR_VAL);
    }
    cur_fb = fb; cur_w = W; cur_h = H;
    XPutImage(dpy, mwin, gc_copy, ximg_m, 0, 0, 0, 0, menu_w, menu_h);
    XFlush(dpy);
}
static void menu_open_at(int rx, int ry){
    mi_h = lh_px + 14;
    menu_w = 150; menu_h = 10 + 2*mi_h;
    if (!mwin){
        XSetWindowAttributes ma;
        ma.colormap = cmap; ma.background_pixel = 0; ma.border_pixel = 0;
        ma.override_redirect = True;
        ma.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
        mwin = XCreateWindow(dpy, root, 0, 0, menu_w, menu_h, 0, depth, InputOutput, vis,
                             CWBackPixel | CWBorderPixel | CWColormap | CWEventMask | CWOverrideRedirect, &ma);
        XDefineCursor(dpy, mwin, cur_mv);
        fb_m = calloc((size_t)menu_w * menu_h, 4);
        ximg_m = XCreateImage(dpy, vis, 32, ZPixmap, 0, (char*)fb_m, menu_w, menu_h, 32, menu_w * 4);
    }
    int dw = DisplayWidth(dpy, scr), dh = DisplayHeight(dpy, scr);
    int mx = rx, my = ry;
    if (mx > dw - menu_w) mx = dw - menu_w;
    if (my > dh - menu_h) my = dh - menu_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    XMoveResizeWindow(dpy, mwin, mx, my, menu_w, menu_h);
    XMapRaised(dpy, mwin);
    menu_open = 1; menu_hi = -1;
    render_menu();
    if (XGrabPointer(dpy, mwin, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess){
        menu_open = 0; XUnmapWindow(dpy, mwin);
    }
    XFlush(dpy);
}

/* ---------------- 窗口状态申请 ---------------- */
static void apply_states(void){
    XEvent e; memset(&e, 0, sizeof e);
    e.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = atom_wmstate;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 1;            /* _NET_WM_STATE_ADD */
#if ALWAYS_ON_TOP
    e.xclient.data.l[1] = (long)atom_above;
#else
    e.xclient.data.l[1] = (long)atom_below;    /* 保持在普通窗口之下 → 可被覆盖 */
    e.xclient.data.l[2] = (long)atom_sticky;   /* 所有工作区可见 */
#endif
    e.xclient.data.l[3] = 1;            /* source: application */
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
#if ALWAYS_ON_TOP
    XRaiseWindow(dpy, win);
#endif
}

/* ---------------- 事件 ---------------- */
static void handle(XEvent *e){
    switch (e->type){
    case Expose:
        if (e->xexpose.window == win) XPutImage(dpy, win, gc_copy, ximg, 0, 0, 0, 0, W, H);
        else if (mwin && e->xexpose.window == mwin && menu_open) render_menu();
        break;
    case MotionNotify:
        if (menu_open){
            int h = menu_hit_at(e->xmotion.x, e->xmotion.y);
            if (h != menu_hi){ menu_hi = h; render_menu(); }
        } else if (drag){
            int nx = e->xmotion.x_root - ddx, ny = e->xmotion.y_root - ddy;
            int dw = DisplayWidth(dpy, scr), dh = DisplayHeight(dpy, scr);
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > dw - W) nx = dw - W;
            if (ny > dh - H) ny = dh - H;
            if (nx != px || ny != py){ px = nx; py = ny; XMoveWindow(dpy, win, px, py); }
        }
        break;
    case ButtonPress:
        if (menu_open){
            if (e->xbutton.button != 1){ menu_close(-1); break; }
            if (menu_hit_at(e->xbutton.x, e->xbutton.y) < 0) menu_close(-1);
            break;
        }
        if (e->xbutton.window == win && e->xbutton.button == 1){
            drag = 1; ddx = e->xbutton.x_root - px; ddy = e->xbutton.y_root - py;
        } else if (e->xbutton.window == win && e->xbutton.button == 3){
            menu_open_at(e->xbutton.x_root, e->xbutton.y_root);
        }
        break;
    case ButtonRelease:
        if (menu_open){
            if (e->xbutton.button == 1) menu_close(menu_hit_at(e->xbutton.x, e->xbutton.y));
            break;
        }
        if (e->xbutton.window == win && e->xbutton.button == 1 && drag){
            drag = 0; save_pos();
        }
        break;
    case ConfigureNotify:
        if (e->xconfigure.window == win){ px = e->xconfigure.x; py = e->xconfigure.y; }
        break;
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == wm_del_atom) g_quit = 1;
        break;
    }
}

/* ---------------- 主程序 ---------------- */
int main(void){
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig);

    dpy = XOpenDisplay(NULL);
    if (!dpy){ fprintf(stderr, "SysMonitor: 无法连接 X 显示，请在图形会话中运行。\n"); return 1; }
    scr = DefaultScreen(dpy); root = RootWindow(dpy, scr); xfd = ConnectionNumber(dpy);

    load_pos();
    if (!load_font()){ fprintf(stderr, "SysMonitor: 找不到可用字体\n"); return 1; }
    read_stats();                 /* 含磁盘扫描 → 决定窗口高度 */

    compute_layout();
    int dw = DisplayWidth(dpy, scr), dh = DisplayHeight(dpy, scr);
    if (px < 0 || py < 0){ px = dw - W - 60; py = 60; if (px < 0) px = 20; }
    if (px > dw - W) px = dw - W;
    if (py > dh - H) py = dh - H;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    create_window();
    double last = now_mono();
    render();
    XMapWindow(dpy, win);
    XMoveWindow(dpy, win, px, py);   /* 映射后再定位，防止 WM 重新摆放 */
    apply_states();                  /* 映射后立即申请 置顶/垫底 状态 */
    XFlush(dpy);

    while (!g_quit){
        fd_set rf; FD_ZERO(&rf); FD_SET(xfd, &rf);
        struct timeval tv = { 1, 0 };
        int r = select(xfd + 1, &rf, NULL, NULL, &tv);
        if (g_quit) break;
        if (r > 0) while (XPending(dpy) > 0){ XEvent ev; XNextEvent(dpy, &ev); handle(&ev); }
        double n2 = now_mono();
        if (n2 - last >= 1.0){
            last = n2;
            if (!paused){ read_stats(); render(); }
#if ALWAYS_ON_TOP
            apply_states();          /* 每秒兜底置顶，防止被新窗口覆盖 */
#endif
            XFlush(dpy);
        }
    }

    save_pos();
    XCloseDisplay(dpy);
    return 0;
}
