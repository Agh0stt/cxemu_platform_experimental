/* cxemu.c — CXIS Machine Emulator + ABIOS
 * Build:  gcc -O2 -std=c11 -Wall -o cxemu cxemu.c
 * Usage:  cxemu <kernel.cxe> [disk0.img [disk1.img ...]]
 *         cxemu --help
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "include/cxis.h"
#include "include/cxe.h"
#include "include/abios.h"

/* ════════════════════════════════════════════════════════════════
   CPU MODEL TABLE
════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    const char *freq_str;      /* display string e.g. "1.1GHz" */
    uint32_t    freq_mhz;
    uint32_t    feature_bits;  /* returned by SI_CPUID a2 */
    const char *feature_str;   /* display string */
} CpuModel;

/* feature bits */
#define CPU_FEAT_FP       (1<<0)   /* single precision float */
#define CPU_FEAT_DBL      (1<<1)   /* double precision float */
#define CPU_FEAT_ABIOS    (1<<2)   /* ABIOS present */
#define CPU_FEAT_SIMD64   (1<<3)   /* 64-bit SIMD */
#define CPU_FEAT_SIMD128  (1<<4)   /* 128-bit SIMD */
#define CPU_FEAT_BRANCH   (1<<5)   /* branch predictor sim */
#define CPU_FEAT_OOO      (1<<6)   /* out-of-order sim */
#define CPU_FEAT_HT       (1<<7)   /* hyperthreading sim */
#define CPU_FEAT_VEC      (1<<8)   /* vector extensions */
#define CPU_FEAT_CRYPTO   (1<<9)   /* crypto extensions */

static const CpuModel cpu_models[] = {
    { "cxis-v1",   "800MHz",  800,  0,
      "INT" },
    { "cxis-v1.1", "1.0GHz",  1000, CPU_FEAT_FP,
      "FP" },
    { "cxis-v2",   "1.1GHz",  1100, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS,
      "FP+DBL+ABIOS" },
    { "cxis-v2.1", "1.4GHz",  1400, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD64,
      "FP+DBL+ABIOS+SIMD64" },
    { "cxis-v3",   "1.8GHz",  1800, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD64|CPU_FEAT_SIMD128,
      "FP+DBL+ABIOS+SIMD64+SIMD128" },
    { "cxis-v3.1", "2.2GHz",  2200, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD64|CPU_FEAT_SIMD128|CPU_FEAT_BRANCH,
      "FP+DBL+ABIOS+SIMD128+BRANCH" },
    { "cxis-v4",   "2.8GHz",  2800, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD128|CPU_FEAT_BRANCH|CPU_FEAT_OOO,
      "FP+DBL+ABIOS+SIMD128+BRANCH+OOO" },
    { "cxis-v4.1", "3.2GHz",  3200, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD128|CPU_FEAT_BRANCH|CPU_FEAT_OOO|CPU_FEAT_HT,
      "FP+DBL+ABIOS+SIMD128+OOO+HT" },
    { "cxis-v5",   "4.0GHz",  4000, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD128|CPU_FEAT_BRANCH|CPU_FEAT_OOO|CPU_FEAT_HT|CPU_FEAT_VEC,
      "FP+DBL+ABIOS+SIMD128+OOO+HT+VEC" },
    { "cxis-v5.1", "4.8GHz",  4800, CPU_FEAT_FP|CPU_FEAT_DBL|CPU_FEAT_ABIOS|CPU_FEAT_SIMD128|CPU_FEAT_BRANCH|CPU_FEAT_OOO|CPU_FEAT_HT|CPU_FEAT_VEC|CPU_FEAT_CRYPTO,
      "FP+DBL+ABIOS+SIMD128+OOO+HT+VEC+CRYPTO" },
};
#define CPU_MODEL_COUNT (int)(sizeof(cpu_models)/sizeof(cpu_models[0]))
#define CPU_MODEL_DEFAULT 2   /* cxis-v2 */

/* ════════════════════════════════════════════════════════════════
   GPU MODEL TABLE
════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    uint32_t    vram_mb;
    const char *features;
} GpuModel;

static const GpuModel gpu_models[] = {
    { "cxgpu-1",  4,   "text+basic2D" },
    { "cxgpu-2",  16,  "text+GDO-2D+palette" },
    { "cxgpu-3",  64,  "text+GDO-2D+blitter+palette" },
    { "cxgpu-4",  256, "text+GDO-2D+blitter+palette+3Dpipeline" },
};
#define GPU_MODEL_COUNT (int)(sizeof(gpu_models)/sizeof(gpu_models[0]))
#define GPU_MODEL_DEFAULT 0   /* cxgpu-1 */

/* ════════════════════════════════════════════════════════════════
   MACHINE CONFIG  (set by CLI, read by emulator + SI handlers)
════════════════════════════════════════════════════════════════ */

typedef struct {
    const CpuModel *cpu;
    const GpuModel *gpu;
    int             cores;
    uint32_t        ram_mb;
} MachineConfig;

static MachineConfig g_machine = {
    .cpu    = &cpu_models[CPU_MODEL_DEFAULT],
    .gpu    = &gpu_models[GPU_MODEL_DEFAULT],
    .cores  = 1,
    .ram_mb = 64,
};

/* ════════════════════════════════════════════════════════════════
   DISK STATE
════════════════════════════════════════════════════════════════ */

typedef enum {
    DISK_TYPE_HDD    = 0,
    DISK_TYPE_FLOPPY = 1,
    DISK_TYPE_SSD    = 2,
    DISK_TYPE_CDROM  = 3,
} DiskType;

static const char *disk_type_name[] = { "HDD", "Floppy", "SSD", "CDROM" };

typedef struct {
    FILE    *fp;
    uint32_t sector_count;
    char     path[256];
    DiskType type;
} Disk;

/* boot signature */
#define CXEBOOT_MAGIC     "CXEBOOT"
#define CXEBOOT_MAGIC_LEN 6
#define CXEBOOT_LOAD_SIZE 1024

/* ════════════════════════════════════════════════════════════════
   TIMER STATE
════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t period_ms;
    uint32_t isr_addr;
    uint64_t next_fire_ns;
    int      active;
} TimerEntry;

#define MAX_TIMERS 8

/* ════════════════════════════════════════════════════════════════
   CPU STATE
════════════════════════════════════════════════════════════════ */

typedef struct {
    /* register file */
    int32_t  i[32];
    int64_t  l[32];
    float    f[32];
    double   d[32];
    int32_t  c[32];
    int32_t  s[32];
    uint64_t a[10];
    uint32_t sp, sf, bp, bf;
    uint32_t pc;
    uint8_t  carry;
    uint8_t  intf;
    uint8_t  halted;
    uint8_t  running;

    /* machine RAM */
    uint8_t *ram;
    uint32_t ram_size;

    /* ABIOS state */
    uint64_t boot_ns;          /* boot timestamp */
    Disk     disks[DISK_MAX_DRIVES];
    int      disk_count;
    TimerEntry timers[MAX_TIMERS];
    int      trace;
    long long steps;
} CPU;

/* ════════════════════════════════════════════════════════════════
   MEMORY HELPERS
════════════════════════════════════════════════════════════════ */

static void mem_fault(CPU *cpu, uint32_t addr, const char *op) {
    fprintf(stderr, "cxemu: %s fault at 0x%08X (pc=0x%08X)\n", op, addr, cpu->pc);
    cpu->running = 0;
}

static uint8_t  mr8 (CPU *c, uint32_t a)          { if (a>=c->ram_size){mem_fault(c,a,"read8");return 0;}  return c->ram[a]; }
static uint16_t mr16(CPU *c, uint32_t a)          { uint16_t v; if(a+1>=c->ram_size){mem_fault(c,a,"read16");return 0;} memcpy(&v,c->ram+a,2); return v; }
static uint32_t mr32(CPU *c, uint32_t a)          { uint32_t v; if(a+3>=c->ram_size){mem_fault(c,a,"read32");return 0;} memcpy(&v,c->ram+a,4); return v; }
static uint64_t mr64(CPU *c, uint32_t a)          { uint64_t v; if(a+7>=c->ram_size){mem_fault(c,a,"read64");return 0;} memcpy(&v,c->ram+a,8); return v; }
static float    mrf (CPU *c, uint32_t a)          { float    v; if(a+3>=c->ram_size){mem_fault(c,a,"readf");return 0;}  memcpy(&v,c->ram+a,4); return v; }
static double   mrd (CPU *c, uint32_t a)          { double   v; if(a+7>=c->ram_size){mem_fault(c,a,"readd");return 0;}  memcpy(&v,c->ram+a,8); return v; }
static void     mw8 (CPU *c, uint32_t a, uint8_t  v) { if(a>=c->ram_size){mem_fault(c,a,"write8");return;}  c->ram[a]=v; }
static void     mw32(CPU *c, uint32_t a, uint32_t v) { if(a+3>=c->ram_size){mem_fault(c,a,"write32");return;} memcpy(c->ram+a,&v,4); }
static void     mw64(CPU *c, uint32_t a, uint64_t v) { if(a+7>=c->ram_size){mem_fault(c,a,"write64");return;} memcpy(c->ram+a,&v,8); }
static void     mwf (CPU *c, uint32_t a, float    v) { if(a+3>=c->ram_size){mem_fault(c,a,"writef");return;} memcpy(c->ram+a,&v,4); }
static void     mwd (CPU *c, uint32_t a, double   v) { if(a+7>=c->ram_size){mem_fault(c,a,"writed");return;} memcpy(c->ram+a,&v,8); }

static uint8_t  f8 (CPU *c) { uint8_t  v=mr8 (c,c->pc); c->pc+=1; return v; }
static uint16_t f16(CPU *c) { uint16_t v=mr16(c,c->pc); c->pc+=2; return v; }
static uint32_t f32(CPU *c) { uint32_t v=mr32(c,c->pc); c->pc+=4; return v; }
static uint64_t f64(CPU *c) { uint64_t v=mr64(c,c->pc); c->pc+=8; return v; }
static float    ff (CPU *c) { float    v=mrf (c,c->pc); c->pc+=4; return v; }
static double   fd (CPU *c) { double   v=mrd (c,c->pc); c->pc+=8; return v; }

static void     push32(CPU *c, uint32_t v) { c->sp-=4; mw32(c,c->sp,v); }
static uint32_t pop32 (CPU *c)             { uint32_t v=mr32(c,c->sp); c->sp+=4; return v; }

/* ════════════════════════════════════════════════════════════════
   REGISTER ACCESS
════════════════════════════════════════════════════════════════ */

static int32_t  get_i(CPU *c,uint8_t id){return c->i[id];}
static int64_t  get_l(CPU *c,uint8_t id){return c->l[id-32];}
static float    get_f(CPU *c,uint8_t id){return c->f[id-64];}
static double   get_d(CPU *c,uint8_t id){return c->d[id-96];}
static int32_t  get_c(CPU *c,uint8_t id){return c->c[id-128];}
static void set_i(CPU *c,uint8_t id,int32_t  v){c->i[id]=v;}
static void set_l(CPU *c,uint8_t id,int64_t  v){c->l[id-32]=v;}
static void set_f(CPU *c,uint8_t id,float    v){c->f[id-64]=v;}
static void set_d(CPU *c,uint8_t id,double   v){c->d[id-96]=v;}
static void set_c(CPU *c,uint8_t id,int32_t  v){c->c[id-128]=v;}

static int32_t getreg32(CPU *c, uint8_t id) {
    if (id<32)  return c->i[id];
    if (id<64)  return (int32_t)c->l[id-32];
    if (id<96)  return (int32_t)c->f[id-64];
    if (id<128) return (int32_t)c->d[id-96];
    if (id<160) return c->c[id-128];
    if (id<192) return c->s[id-160];
    if (id<202) return (int32_t)c->a[id-192];
    if (id==202) return (int32_t)c->sp;
    if (id==203) return (int32_t)c->sf;
    if (id==204) return (int32_t)c->bp;
    if (id==205) return (int32_t)c->bf;
    return 0;
}
static void setreg32(CPU *c, uint8_t id, int32_t v) {
    if (id<32)  { c->i[id]=v; return; }
    if (id<64)  { c->l[id-32]=(int64_t)v; return; }
    if (id<96)  { c->f[id-64]=(float)v; return; }
    if (id<128) { c->d[id-96]=(double)v; return; }
    if (id<160) { c->c[id-128]=v; return; }
    if (id<192) { c->s[id-160]=v; return; }
    if (id<202) { c->a[id-192]=(uint64_t)(int64_t)v; return; }
    if (id==202) { c->sp=(uint32_t)v; return; }
    if (id==203) { c->sf=(uint32_t)v; return; }
    if (id==204) { c->bp=(uint32_t)v; return; }
    if (id==205) { c->bf=(uint32_t)v; return; }
}

/* ════════════════════════════════════════════════════════════════
   MEMORY OPERAND DECODE
════════════════════════════════════════════════════════════════ */

static uint32_t decode_mem_addr(CPU *c) {
    uint8_t  mf   = f8(c);
    uint32_t addr = 0;
    if (mf & 0x01) { uint8_t base=f8(c); addr+=(uint32_t)getreg32(c,base); }
    if (mf & 0x02) { uint8_t idx=f8(c); uint8_t sc=f8(c); addr+=(uint32_t)getreg32(c,idx)*sc; }
    addr += f32(c);
    return addr;
}

/* ════════════════════════════════════════════════════════════════
   TIMER HELPERS
════════════════════════════════════════════════════════════════ */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t elapsed_ns(CPU *c) { return now_ns() - c->boot_ns; }
static uint64_t elapsed_ms(CPU *c) { return elapsed_ns(c) / 1000000ULL; }

static void check_timers(CPU *c) {
    if (!c->intf) return;
    uint64_t t = elapsed_ns(c);
    for (int i = 0; i < MAX_TIMERS; i++) {
        TimerEntry *te = &c->timers[i];
        if (!te->active) continue;
        if (t >= te->next_fire_ns) {
            te->next_fire_ns += (uint64_t)te->period_ms * 1000000ULL;
            /* call ISR: push return address, jump */
            push32(c, c->pc);
            c->pc = te->isr_addr;
        }
    }
}

/* ════════════════════════════════════════════════════════════════
   ABIOS VIDEO — writes to VRAM in RAM, mirrors to terminal
════════════════════════════════════════════════════════════════ */

/* ANSI color map: 8 standard CGA colors → ANSI */
static const char *ansi_fg[16] = {
    "30","34","32","36","31","35","33","37",
    "90","94","92","96","91","95","93","97"
};
static const char *ansi_bg[8] = {
    "40","44","42","46","41","45","43","47"
};

static void vid_update_cell(CPU *c, int col, int row) {
    uint32_t off = MEM_VRAM_BASE + (uint32_t)(row * VID_COLS + col) * 2;
    uint8_t  ch  = c->ram[off];
    uint8_t  at  = c->ram[off+1];
    int fg = at & 0x0F;
    int bg = (at >> 4) & 0x07;
    /* move cursor to position, set colors, print char */
    printf("\033[%d;%dH\033[%s;%sm%c",
           row+1, col+1,
           ansi_fg[fg], ansi_bg[bg],
           ch ? ch : ' ');
    fflush(stdout);
}

static void vid_clear(CPU *c, uint8_t attr) {
    for (int row = 0; row < VID_ROWS; row++)
        for (int col = 0; col < VID_COLS; col++) {
            uint32_t off = MEM_VRAM_BASE + (uint32_t)(row*VID_COLS+col)*2;
            c->ram[off]   = ' ';
            c->ram[off+1] = attr;
        }
    printf("\033[2J\033[H\033[0m");
    fflush(stdout);
}

static void vid_putchar_at(CPU *c, uint8_t ch, int col, int row, uint8_t attr) {
    if (col < 0 || col >= VID_COLS || row < 0 || row >= VID_ROWS) return;
    uint32_t off = MEM_VRAM_BASE + (uint32_t)(row*VID_COLS+col)*2;
    c->ram[off]   = ch;
    c->ram[off+1] = attr;
    vid_update_cell(c, col, row);
}

static void vid_scroll_up(CPU *c, int lines, uint8_t attr) {
    if (lines <= 0) return;
    if (lines >= VID_ROWS) { vid_clear(c, attr); return; }
    /* shift VRAM up */
    memmove(c->ram + MEM_VRAM_BASE,
            c->ram + MEM_VRAM_BASE + (uint32_t)lines*VID_COLS*2,
            (uint32_t)(VID_ROWS-lines)*VID_COLS*2);
    /* blank bottom lines */
    for (int r = VID_ROWS-lines; r < VID_ROWS; r++)
        for (int col = 0; col < VID_COLS; col++) {
            uint32_t off = MEM_VRAM_BASE + (uint32_t)(r*VID_COLS+col)*2;
            c->ram[off]   = ' ';
            c->ram[off+1] = attr;
        }
    printf("\033[%dS", lines);   /* ANSI scroll up */
    fflush(stdout);
}

/* ── ABIOS data area helpers ── */
static uint8_t  ada_r8 (CPU *c, int off) { return c->ram[MEM_ABIOS_DATA + off]; }
static uint32_t ada_r32(CPU *c, int off) { return mr32(c, MEM_ABIOS_DATA + off); }
static void     ada_w8 (CPU *c, int off, uint8_t  v) { c->ram[MEM_ABIOS_DATA+off]=v; }
static void     ada_w32(CPU *c, int off, uint32_t v) { mw32(c, MEM_ABIOS_DATA+off, v); }

static int  cur_x(CPU *c) { return ada_r8(c, ADA_CUR_X); }
static int  cur_y(CPU *c) { return ada_r8(c, ADA_CUR_Y); }
static uint8_t def_attr(CPU *c) { return ada_r8(c, ADA_ATTR); }

/* ── console print through video ── */
static void con_emit(CPU *c, uint8_t ch) {
    uint8_t attr = def_attr(c);
    int x = cur_x(c), y = cur_y(c);

    if (ch == '\n') {
        x = 0; y++;
    } else if (ch == '\r') {
        x = 0;
    } else if (ch == '\t') {
        int nx = (x + 8) & ~7;
        for (; x < nx && x < VID_COLS; x++)
            vid_putchar_at(c, ' ', x, y, attr);
    } else if (ch == '\b') {
        if (x > 0) { x--; vid_putchar_at(c, ' ', x, y, attr); }
    } else {
        vid_putchar_at(c, ch, x, y, attr);
        x++;
        if (x >= VID_COLS) { x = 0; y++; }
    }

    if (y >= VID_ROWS) {
        vid_scroll_up(c, y - VID_ROWS + 1, attr);
        y = VID_ROWS - 1;
    }

    ada_w8(c, ADA_CUR_X, (uint8_t)x);
    ada_w8(c, ADA_CUR_Y, (uint8_t)y);
    /* reposition terminal cursor */
    printf("\033[%d;%dH", y+1, x+1);
    fflush(stdout);
}

static void con_puts_raw(CPU *c, uint32_t ptr, uint32_t len) {
    for (uint32_t i = 0; i < len && ptr+i < c->ram_size; i++)
        con_emit(c, c->ram[ptr+i]);
}

static void con_puts_cstr(CPU *c, uint32_t ptr) {
    while (ptr < c->ram_size && c->ram[ptr])
        con_emit(c, c->ram[ptr++]);
}

static void con_print_int(CPU *c, int32_t val) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", val);
    for (int i = 0; buf[i]; i++) con_emit(c, (uint8_t)buf[i]);
}

static void con_print_hex(CPU *c, uint32_t val) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%08X", val);
    for (int i = 0; buf[i]; i++) con_emit(c, (uint8_t)buf[i]);
}

/* ════════════════════════════════════════════════════════════════
   ABIOS DISK
════════════════════════════════════════════════════════════════ */

static int disk_read_sectors(CPU *c, int id, uint32_t lba, uint32_t buf, uint32_t count) {
    if (id < 0 || id >= c->disk_count || !c->disks[id].fp) return 1;
    if (buf + count*DISK_SECTOR_SIZE > c->ram_size) return 1;
    fseek(c->disks[id].fp, (long)lba * DISK_SECTOR_SIZE, SEEK_SET);
    size_t got = fread(c->ram + buf, DISK_SECTOR_SIZE, count, c->disks[id].fp);
    return (got == count) ? 0 : 1;
}

static int disk_write_sectors(CPU *c, int id, uint32_t lba, uint32_t buf, uint32_t count) {
    if (id < 0 || id >= c->disk_count || !c->disks[id].fp) return 1;
    if (buf + count*DISK_SECTOR_SIZE > c->ram_size) return 1;
    fseek(c->disks[id].fp, (long)lba * DISK_SECTOR_SIZE, SEEK_SET);
    size_t wrote = fwrite(c->ram + buf, DISK_SECTOR_SIZE, count, c->disks[id].fp);
    fflush(c->disks[id].fp);
    return (wrote == count) ? 0 : 1;
}

/* ════════════════════════════════════════════════════════════════
   ABIOS MEMORY MAP (written into RAM at MEM_ABIOS_STR area)
════════════════════════════════════════════════════════════════ */

#define MEM_MAP_ADDR  (MEM_ABIOS_STR + 0x100)
#define MEM_MAP_ENTRIES 7

static void abios_write_memmap(CPU *c) {
    /* write AbiosMemEntry array into RAM */
    typedef struct { uint32_t base; uint32_t size; uint8_t type; uint8_t pad[3]; } E;
    E map[MEM_MAP_ENTRIES] = {
        {MEM_IVT_BASE,        MEM_IVT_SIZE,         ABIOS_MEM_RESERVED, {0,0,0}},
        {MEM_ABIOS_DATA,      0x00BFF000,            ABIOS_MEM_FIRMWARE, {0,0,0}},
        {MEM_VRAM_BASE,       MEM_VRAM_SIZE,         ABIOS_MEM_VRAM,     {0,0,0}},
        {MEM_KBUF_BASE,       MEM_KBUF_SIZE,         ABIOS_MEM_FIRMWARE, {0,0,0}},
        {MEM_DISK_CACHE_BASE, MEM_DISK_CACHE_SIZE,   ABIOS_MEM_FIRMWARE, {0,0,0}},
        {MEM_KERNEL_BASE,     0x01000000,            ABIOS_MEM_KERNEL,   {0,0,0}},
        {MEM_HEAP_BASE,       MEM_STACK_TOP - MEM_HEAP_BASE, ABIOS_MEM_FREE, {0,0,0}},
    };
    memcpy(c->ram + MEM_MAP_ADDR, map, sizeof(map));
}

/* ════════════════════════════════════════════════════════════════
   ABIOS INTERRUPT HANDLER
════════════════════════════════════════════════════════════════ */

/* ── legacy cxvm BIOS vectors (for old .cxe binaries) ──────────
   int 0x01 = PRINT_STR  a0=ptr a1=len
   int 0x02 = PRINT_CHAR a0=char
   int 0x03 = READ_CHAR  → a0
   int 0x04 = EXIT       a0=code
   int 0x05 = MEM_ALLOC  a0=size → a0=ptr
   int 0x06 = MEM_FREE
   int 0x07 = TIME       → l0=ns
   These shadow the lower ABIOS vectors.  OS code that wants the
   full ABIOS must call int 0x10–0x18 (remapped) OR set a0 to a
   sub-function before calling int 0x01–0x09 with the high bit of
   a0 set (0x80000000 flag).  For simplicity we detect legacy by
   checking if a0 looks like a small sub-fn id vs a pointer/code. */

#define LEGACY_THRESHOLD 0x10   /* if a0 < 16 assume sub-fn (ABIOS), else legacy? */
/* Actually simpler: keep legacy on 0x01-0x07, add new ABIOS on 0x10-0x18 */

static void abios_handle(CPU *c, uint8_t vector) {
    uint32_t fn   = (uint32_t)c->a[0];
    uint32_t arg1 = (uint32_t)c->a[1];
    uint32_t arg2 = (uint32_t)c->a[2];
    uint32_t arg3 = (uint32_t)c->a[3];
    uint32_t arg4 = (uint32_t)c->a[4];

    /* ── legacy ABI shim for vectors 0x01–0x03 ──────────────────────────
       ABIOS calls these with the old cxvm convention:
         INT 0x01: a0=ptr  a1=len   (print string)
         INT 0x02: a0=subfn         (keyboard: 0=read 1=poll 2=flush)
         INT 0x03: —                (read char → a0)
       Vector 0x04 (EXIT in the old shim) is now VEC_DISK — do NOT intercept.
    ── */
    switch (vector) {
    case 0x01: /* PRINT_STR: a0=ptr a1=len */
        con_puts_raw(c, fn, arg1); return;
    case 0x02: /* KEYBOARD sub-functions */
        switch (fn) {
        case 0: { int ch = getchar(); c->a[0]=(ch==EOF)?0u:(uint64_t)(unsigned char)ch; } return;
        case 1: c->a[0] = 0; return;   /* KB_POLL: always no key */
        case 2: while(getchar()!='\n' && !feof(stdin)); return; /* KB_FLUSH */
        default: c->a[0] = 0; return;
        }
    case 0x03: /* READ_CHAR → a0 */
        { int ch = getchar(); c->a[0]=(ch==EOF)?0xFFFFFFFFu:(uint64_t)(unsigned char)ch; return; }
    }

    /* ── remap new OS vectors 0x10–0x18 → 0x01–0x09 ── */
    if (vector >= 0x10 && vector <= 0x18) vector = (uint8_t)(vector - 0x0F);

    switch (vector) {

    /* ── CONSOLE ── */
    case VEC_CONSOLE:
        switch (fn) {
        case CON_PUTCHAR:  con_emit(c, (uint8_t)arg1); break;
        case CON_WRITE:    con_puts_raw(c, arg1, arg2); break;
        case CON_PUTS:     con_puts_cstr(c, arg1); break;
        case CON_GETCHAR: {
            int ch = getchar();
            c->a[0] = (ch == EOF) ? 0xFFFFFFFFu : (uint64_t)(unsigned char)ch;
            break;
        }
        case CON_READLINE: {
            uint32_t buf = arg1, maxlen = arg2, got = 0;
            int ch;
            while (got < maxlen-1) {
                ch = getchar();
                if (ch == EOF || ch == '\n') break;
                c->ram[buf + got++] = (uint8_t)ch;
                con_emit(c, (uint8_t)ch);
            }
            c->ram[buf + got] = 0;
            c->a[0] = got;
            con_emit(c, '\n');
            break;
        }
        case CON_PRINT_INT: con_print_int(c, (int32_t)arg1); break;
        case CON_PRINT_HEX: con_print_hex(c, arg1); break;
        case CON_EXIT:      exit((int)arg1); break;
        default:
            fprintf(stderr, "abios: unknown console fn 0x%02X\n", fn);
        }
        break;

    /* ── KEYBOARD ── */
    case VEC_KEYBOARD:
        switch (fn) {
        case KB_READ: {
            int ch = getchar();
            c->a[0] = (ch == EOF) ? 0u : (uint64_t)(unsigned char)ch;
            break;
        }
        case KB_POLL:
            /* non-blocking not easily portable without termios; return 0 */
            c->a[0] = 0;
            break;
        case KB_FLUSH:
            /* flush stdin */
            while ((getchar()) != '\n' && !feof(stdin));
            break;
        case KB_AVAILABLE:
            c->a[0] = 0;
            break;
        }
        break;

    /* ── VIDEO ── */
    case VEC_VIDEO: {
        uint8_t attr = (uint8_t)arg4;
        switch (fn) {
        case VID_CLEAR:
            vid_clear(c, (uint8_t)arg1);
            ada_w8(c, ADA_ATTR, (uint8_t)arg1);
            break;
        case VID_PUTCHAR:
            vid_putchar_at(c, (uint8_t)arg1, (int)arg2, (int)arg3, attr);
            break;
        case VID_PUTS: {
            uint32_t ptr = arg1;
            int col = (int)arg2, row = (int)arg3;
            while (ptr < c->ram_size && c->ram[ptr] && col < VID_COLS) {
                vid_putchar_at(c, c->ram[ptr++], col++, row, attr);
            }
            break;
        }
        case VID_SCROLL_UP:
            vid_scroll_up(c, (int)arg1, (uint8_t)arg2);
            break;
        case VID_SET_CURSOR:
            ada_w8(c, ADA_CUR_X, (uint8_t)arg1);
            ada_w8(c, ADA_CUR_Y, (uint8_t)arg2);
            printf("\033[%d;%dH", (int)arg2+1, (int)arg1+1);
            fflush(stdout);
            break;
        case VID_GET_CURSOR:
            c->a[0] = cur_x(c);
            c->a[1] = cur_y(c);
            break;
        case VID_SET_ATTR:
            ada_w8(c, ADA_ATTR, (uint8_t)arg1);
            break;
        case VID_GET_CHAR: {
            int col=(int)arg1, row=(int)arg2;
            if (col>=0&&col<VID_COLS&&row>=0&&row<VID_ROWS) {
                uint32_t off = MEM_VRAM_BASE + (uint32_t)(row*VID_COLS+col)*2;
                c->a[0] = c->ram[off];
                c->a[1] = c->ram[off+1];
            } else { c->a[0]=0; c->a[1]=0; }
            break;
        }
        case VID_VRAM_ADDR:
            c->a[0] = MEM_VRAM_BASE;
            break;
        case VID_DIMENSIONS:
            c->a[0] = VID_COLS;
            c->a[1] = VID_ROWS;
            break;
        case VID_WRITE_RAW:
            vid_putchar_at(c, (uint8_t)arg3, (int)arg1, (int)arg2, (uint8_t)arg4);
            break;
        default:
            fprintf(stderr, "abios: unknown video fn 0x%02X\n", fn);
        }
        break;
    }

    /* ── DISK ── */
    case VEC_DISK:
        switch (fn) {
        case DISK_READ:
            c->a[0] = disk_read_sectors(c,(int)arg1,arg2,arg3,arg4);
            break;
        case DISK_WRITE:
            c->a[0] = disk_write_sectors(c,(int)arg1,arg2,arg3,arg4);
            break;
        case DISK_SECTORS:
            c->a[0] = (arg1<(uint32_t)c->disk_count) ? c->disks[arg1].sector_count : 0;
            break;
        case DISK_COUNT:
            c->a[0] = (uint32_t)c->disk_count;
            break;
        case DISK_SECTOR_SZ:
            c->a[0] = DISK_SECTOR_SIZE;
            break;
        case DISK_FLUSH:
            for (int d=0;d<c->disk_count;d++)
                if (c->disks[d].fp) fflush(c->disks[d].fp);
            break;
        default:
            fprintf(stderr,"abios: unknown disk fn 0x%02X\n",fn);
        }
        break;

    /* ── MEMORY ── */
    case VEC_MEMORY:
        switch (fn) {
        case MEM_ALLOC: {
            uint32_t sz  = (arg1 + 7) & ~7u;
            uint32_t ptr = ada_r32(c, ADA_HEAP_PTR);
            if (ptr + sz < MEM_STACK_TOP - 0x10000) {
                c->a[0] = ptr;
                ada_w32(c, ADA_HEAP_PTR, ptr + sz);
            } else { c->a[0] = 0; }
            break;
        }
        case MEM_FREE: break;   /* bump allocator */
        case MEM_TOTAL_Q:
            c->a[0] = c->ram_size;
            break;
        case MEM_FREE_Q: {
            uint32_t used = ada_r32(c, ADA_HEAP_PTR) - MEM_HEAP_BASE;
            uint32_t total = MEM_STACK_TOP - 0x10000 - MEM_HEAP_BASE;
            c->a[0] = (used < total) ? total - used : 0;
            break;
        }
        case MEM_MAP_Q:
            c->a[0] = MEM_MAP_ADDR;
            c->a[1] = MEM_MAP_ENTRIES;
            break;
        case MEM_COPY:
            if (arg1 + arg3 <= c->ram_size && arg2 + arg3 <= c->ram_size)
                memmove(c->ram + arg1, c->ram + arg2, arg3);
            break;
        case MEM_SET:
            if (arg1 + arg3 <= c->ram_size)
                memset(c->ram + arg1, (int)arg2, arg3);
            break;
        case MEM_COMPARE:
            if (arg1 + arg3 <= c->ram_size && arg2 + arg3 <= c->ram_size)
                c->a[0] = (uint64_t)(int64_t)memcmp(c->ram+arg1, c->ram+arg2, arg3);
            break;
        default:
            fprintf(stderr,"abios: unknown memory fn 0x%02X\n",fn);
        }
        break;

    /* ── TIMER ── */
    case VEC_TIMER:
        switch (fn) {
        case TIMER_TICKS_NS:
            c->l[0] = (int64_t)elapsed_ns(c);
            break;
        case TIMER_TICKS_MS:
            c->a[0] = (uint32_t)elapsed_ms(c);
            break;
        case TIMER_SLEEP_MS: {
            uint64_t end = elapsed_ms(c) + arg1;
            while (elapsed_ms(c) < end);
            break;
        }
        case TIMER_UPTIME:
            c->a[0] = (uint32_t)(elapsed_ns(c) / 1000000000ULL);
            break;
        case TIMER_INSTALL:
            for (int t = 0; t < MAX_TIMERS; t++) {
                if (!c->timers[t].active) {
                    c->timers[t].period_ms   = arg1;
                    c->timers[t].isr_addr    = arg2;
                    c->timers[t].next_fire_ns= elapsed_ns(c)+(uint64_t)arg1*1000000ULL;
                    c->timers[t].active      = 1;
                    c->a[0] = (uint32_t)t;
                    break;
                }
            }
            break;
        case TIMER_REMOVE:
            if (arg1 < MAX_TIMERS) c->timers[arg1].active = 0;
            break;
        default:
            fprintf(stderr,"abios: unknown timer fn 0x%02X\n",fn);
        }
        break;

    /* ── POWER ── */
    case VEC_POWER:
        switch (fn) {
        case PWR_SHUTDOWN:
            fprintf(stderr,"\ncxemu: shutdown (code=%u, steps=%lld)\n",
                    arg1, c->steps);
            exit((int)arg1);
        case PWR_REBOOT:
            fprintf(stderr,"\ncxemu: reboot requested (not implemented — halt)\n");
            c->halted = 1; c->running = 0;
            break;
        case PWR_HALT:
            c->halted = 1; c->running = 0;
            break;
        }
        break;

    /* ── SYSINFO ── */
    case VEC_SYSINFO:
        switch (fn) {
        case SI_CPUID:
            c->a[0] = CXIS_SIG;
            c->a[1] = CXIS_VER;
            c->a[2] = g_machine.cpu->feature_bits;
            /* write cpu name string into ABIOS string area */
            strncpy((char*)(c->ram + MEM_ABIOS_STR + 64),
                    g_machine.cpu->name, 32);
            strncpy((char*)(c->ram + MEM_ABIOS_STR + 96),
                    g_machine.cpu->feature_str, 64);
            c->a[3] = MEM_ABIOS_STR + 64;   /* ptr to name   */
            c->a[4] = MEM_ABIOS_STR + 96;   /* ptr to feats  */
            break;
        case SI_MEMSIZE:
            c->a[0] = (uint32_t)g_machine.ram_mb * 1024 * 1024;
            break;
        case SI_CPU_FREQ:
            c->a[0] = g_machine.cpu->freq_mhz;
            break;
        case SI_CPU_CORES:
            c->a[0] = (uint32_t)g_machine.cores;
            break;
        case SI_GPU_ID: {
            strncpy((char*)(c->ram + MEM_ABIOS_STR + 160),
                    g_machine.gpu->name, 32);
            strncpy((char*)(c->ram + MEM_ABIOS_STR + 192),
                    g_machine.gpu->features, 64);
            c->a[0] = MEM_ABIOS_STR + 160;  /* ptr to gpu name  */
            c->a[1] = MEM_ABIOS_STR + 192;  /* ptr to gpu feats */
            c->a[2] = g_machine.gpu->vram_mb;
            break;
        }
        case SI_DISK_COUNT:
            c->a[0] = (uint32_t)c->disk_count;
            break;
        case SI_BIOS_VER: {
            const char *ver = "ABIOS v1.0 (cxemu)";
            uint32_t dst = MEM_ABIOS_STR;
            strncpy((char*)(c->ram + dst), ver, 64);
            c->a[0] = dst;
            break;
        }
        case SI_BOOT_DISK:
            c->a[0] = ada_r8(c, ADA_BOOT_DISK);
            break;
        case SI_MEM_MAP:
            c->a[0] = MEM_MAP_ADDR;
            c->a[1] = MEM_MAP_ENTRIES;
            break;
        default:
            fprintf(stderr,"abios: unknown sysinfo fn 0x%02X\n",fn);
        }
        break;

    /* ── IRQ ── */
    case VEC_IRQ:
        switch (fn) {
        case IRQ_INSTALL:
            if (arg1 < 256) mw32(c, IVT_ENTRY(arg1), arg2);
            break;
        case IRQ_REMOVE:
            if (arg1 < 256) mw32(c, IVT_ENTRY(arg1), 0);
            break;
        case IRQ_ENABLE:  c->intf = 1; break;
        case IRQ_DISABLE: c->intf = 0; break;
        case IRQ_FIRE: {
            uint32_t isr = mr32(c, IVT_ENTRY(arg1 & 0xFF));
            if (isr) { push32(c, c->pc); c->pc = isr; }
            break;
        }
        }
        break;

    default:
        fprintf(stderr,"abios: unhandled vector 0x%02X at pc=0x%08X\n",
                vector, c->pc);
    }
}

/* ════════════════════════════════════════════════════════════════
   INSTRUCTION DECODE & EXECUTE  (mirrors cxvm.c + fixes)
════════════════════════════════════════════════════════════════ */

static void step(CPU *c) {
    c->steps++;

    uint8_t opcode = f8(c);
    uint8_t mod    = f8(c);

    int nops    = (mod >> 6) & 0x3;
    int has_imm = (mod >> 5) & 0x1;
    int has_mem = (mod >> 4) & 0x1;
    int has_ext = (mod >> 3) & 0x1;

    /* Current assembler emits three extension type bytes. */
    uint8_t dst_class = 0, s1_class = 0, s2_class = 0;
    if (has_ext) {
        dst_class = f8(c);
        s1_class  = f8(c);
        s2_class  = f8(c);
    }

    uint8_t rv[4] = {0,0,0,0};
    int     nr    = 0;
    int64_t imm   = 0;
    uint64_t immu = 0;
    uint32_t label_addr = 0;

    int nregs = nops - (has_imm ? 1 : 0) - (has_mem ? 1 : 0);
    if (nregs < 0) nregs = 0;
    for (int k = 0; k < nregs && k < 4; k++) rv[nr++] = f8(c);
    if (has_imm) {
        int imm_is_wide = 0;
        if (has_ext) {
            /* For immediate forms, wide scalar immediates are used by long/double ops. */
            if (dst_class == RC_L || dst_class == RC_D ||
                s1_class == RC_L || s1_class == RC_D ||
                s2_class == RC_L || s2_class == RC_D)
                imm_is_wide = 1;
        }
        if (imm_is_wide) {
            immu = f64(c);
            imm  = (int64_t)immu;
        } else {
            imm = (int64_t)(int32_t)f32(c);
            immu = (uint64_t)(uint32_t)(int32_t)imm;
        }
        /* Toolchain encodes immediate jump targets as absolute addresses. */
        label_addr = (uint32_t)(int32_t)imm;
    }

#define GR(id)    getreg32(c,id)
#define SR(id,v)  setreg32(c,id,(int32_t)(v))
#define RV0 rv[0]
#define RV1 rv[1]
#define RV2 rv[2]

    switch (opcode) {

    case OP_NOP:  break;
    case OP_HALT: c->halted=1; c->running=0; break;

    /* ── DATA MOVEMENT ── */
    case OP_MOV: {
        if (has_imm && !has_mem) { SR(RV0,(int32_t)imm); }
        else if (has_mem) {
            uint32_t addr = decode_mem_addr(c);
            if (mod & 0x04) mw32(c,addr,(uint32_t)GR(RV0));
            else            SR(RV0,(int32_t)mr32(c,addr));
        } else { SR(RV1,GR(RV0)); }
        break; }
    case OP_MOVB: {
        if (has_mem) {
            uint32_t addr = decode_mem_addr(c);
            if (mod & 0x04) mw8(c,addr,(uint8_t)GR(RV0));
            else            SR(RV0,(int32_t)(uint32_t)mr8(c,addr));
        } else { SR(RV1,GR(RV0)&0xFF); }
        break; }
    case OP_MOVW: {
        if (has_mem) {
            uint32_t addr = decode_mem_addr(c);
            if (mod & 0x04) { uint16_t v=(uint16_t)GR(RV0); memcpy(c->ram+addr,&v,2); }
            else { uint16_t v; memcpy(&v,c->ram+addr,2); SR(RV0,(int32_t)(uint32_t)v); }
        } else { SR(RV1,GR(RV0)&0xFFFF); }
        break; }
    case OP_FMOV: {
        if (has_imm) { float fv; memcpy(&fv,&imm,4); set_f(c,RV0,fv); }
        else         set_f(c,RV1,get_f(c,RV0));
        break; }
    case OP_DMOV: {
        if (has_imm) { double dv; memcpy(&dv, &immu, sizeof(dv)); set_d(c,RV0,dv); }
        else         set_d(c,RV1,get_d(c,RV0));
        break; }
    case OP_LMOV: {
        if (has_imm) set_l(c,RV0,(int64_t)immu);
        else         set_l(c,RV1,get_l(c,RV0));
        break; }
    case OP_MOVSX:  SR(RV1,(int32_t)(int16_t)(GR(RV0)&0xFFFF)); break;
    case OP_MOVZX:  SR(RV1,(int32_t)(uint16_t)(uint32_t)GR(RV0)); break;
    case OP_MOVSXD: set_l(c,RV1,(int64_t)(int32_t)GR(RV0)); break;
    case OP_LEA: {
        uint32_t addr = decode_mem_addr(c);
        uint8_t  dst  = f8(c);
        SR(dst,(int32_t)addr); break; }
    case OP_CMOV:   if(get_c(c,RV0)) SR(RV2,GR(RV1)); break;
    case OP_PUSH:   push32(c,(uint32_t)(has_imm?(int32_t)imm:GR(RV0))); break;
    case OP_POP:    SR(RV0,(int32_t)pop32(c)); break;
    case OP_PUSHA:  for(int k=31;k>=0;k--) push32(c,(uint32_t)c->i[k]); break;
    case OP_POPA:   for(int k=0;k<32;k++) c->i[k]=(int32_t)pop32(c); break;

    /* ── ARITHMETIC i32 ── */
    case OP_ADD: { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; int64_t r=(int64_t)a+b; c->carry=(r>(int64_t)0x7FFFFFFF||r<(int64_t)(int32_t)0x80000000)?1:0; SR(d,(int32_t)r); break; }
    case OP_ADDC:{ int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; int64_t r=(int64_t)a+b+c->carry; c->carry=(uint32_t)(r>>32)?1:0; SR(d,(int32_t)r); break; }
    case OP_SUB: { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; int64_t r=(int64_t)a-b; c->carry=(r<0)?1:0; SR(d,(int32_t)r); break; }
    case OP_SUBB:{ int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; int64_t r=(int64_t)a-b-c->carry; c->carry=(r<0)?1:0; SR(d,(int32_t)r); break; }
    case OP_MUL: { uint32_t a=(uint32_t)GR(RV0),b=has_imm?(uint32_t)imm:(uint32_t)GR(RV1); uint8_t d=has_imm?RV1:RV2; SR(d,(int32_t)(a*b)); break; }
    case OP_IMUL:{ int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; SR(d,a*b); break; }
    case OP_DIV: { uint32_t a=(uint32_t)GR(RV0),b=has_imm?(uint32_t)imm:(uint32_t)GR(RV1); uint8_t d=has_imm?RV1:RV2; if(!b){fprintf(stderr,"cxemu: div/0\n");c->running=0;break;} SR(d,(int32_t)(a/b)); break; }
    case OP_IDIV:{ int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; if(!b){fprintf(stderr,"cxemu: div/0\n");c->running=0;break;} SR(d,a/b); break; }
    case OP_INC: SR(RV0,GR(RV0)+1); break;
    case OP_DEC: SR(RV0,GR(RV0)-1); break;
    case OP_NEG: SR(RV1,-GR(RV0)); break;

    /* ── ARITHMETIC i64 ── */
    case OP_LNEG: set_l(c,RV1,-get_l(c,RV0)); break;
    case OP_LADD: set_l(c,RV2,get_l(c,RV0)+get_l(c,RV1)); break;
    case OP_LSUB: set_l(c,RV2,get_l(c,RV0)-get_l(c,RV1)); break;
    case OP_LMUL: set_l(c,RV2,get_l(c,RV0)*get_l(c,RV1)); break;
    case OP_LDIV: { if(!get_l(c,RV1)){fprintf(stderr,"cxemu: div/0\n");c->running=0;break;} set_l(c,RV2,get_l(c,RV0)/get_l(c,RV1)); break; }

    /* ── ARITHMETIC float ── */
    case OP_FADD: set_f(c,RV2,get_f(c,RV0)+get_f(c,RV1)); break;
    case OP_FSUB: set_f(c,RV2,get_f(c,RV0)-get_f(c,RV1)); break;
    case OP_FMUL: set_f(c,RV2,get_f(c,RV0)*get_f(c,RV1)); break;
    case OP_FDIV: set_f(c,RV2,get_f(c,RV0)/get_f(c,RV1)); break;
    case OP_FNEG: set_f(c,RV1,-get_f(c,RV0)); break;

    /* ── ARITHMETIC double ── */
    case OP_DADD: set_d(c,RV2,get_d(c,RV0)+get_d(c,RV1)); break;
    case OP_DSUB: set_d(c,RV2,get_d(c,RV0)-get_d(c,RV1)); break;
    case OP_DMUL: set_d(c,RV2,get_d(c,RV0)*get_d(c,RV1)); break;
    case OP_DDIV: set_d(c,RV2,get_d(c,RV0)/get_d(c,RV1)); break;
    case OP_DNEG: set_d(c,RV1,-get_d(c,RV0)); break;

    /* ── BITWISE ── */
    case OP_AND: { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; SR(d,a&b); break; }
    case OP_OR:  { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; SR(d,a|b); break; }
    case OP_XOR: { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; SR(d,a^b); break; }
    case OP_NOT:    SR(RV1,~GR(RV0)); break;
    case OP_SHL: { int32_t sv=GR(RV0); uint8_t n=has_imm?(uint8_t)(imm&31):(uint8_t)(GR(RV1)&31); uint8_t d=has_imm?RV1:RV2; SR(d,sv<<n); break; }
    case OP_SHR: { uint32_t sv=(uint32_t)GR(RV0); uint8_t n=has_imm?(uint8_t)(imm&31):(uint8_t)(GR(RV1)&31); uint8_t d=has_imm?RV1:RV2; SR(d,(int32_t)(sv>>n)); break; }
    case OP_SAR: { int32_t sv=GR(RV0); uint8_t n=has_imm?(uint8_t)(imm&31):(uint8_t)(GR(RV1)&31); uint8_t d=has_imm?RV1:RV2; SR(d,sv>>n); break; }
    case OP_ROL: { uint32_t sv=(uint32_t)GR(RV0); uint8_t n=has_imm?(uint8_t)(imm&31):(uint8_t)(GR(RV1)&31); uint8_t d=has_imm?RV1:RV2; SR(d, n ? (int32_t)((sv<<n)|(sv>>(32-n))) : (int32_t)sv); break; }
    case OP_ROR: { uint32_t sv=(uint32_t)GR(RV0); uint8_t n=has_imm?(uint8_t)(imm&31):(uint8_t)(GR(RV1)&31); uint8_t d=has_imm?RV1:RV2; SR(d, n ? (int32_t)((sv>>n)|(sv<<(32-n))) : (int32_t)sv); break; }
    case OP_BSF: { uint32_t v=(uint32_t)GR(RV0); int idx=0; while(idx<32&&!((v>>idx)&1))idx++; SR(RV1,idx); break; }
    case OP_BSR: { uint32_t v=(uint32_t)GR(RV0); int idx=31; while(idx>=0&&!((v>>idx)&1))idx--; SR(RV1,idx); break; }
    case OP_POPCNT:{ uint32_t v=(uint32_t)GR(RV0); int cnt=0; while(v){cnt+=v&1;v>>=1;} SR(RV1,cnt); break; }
    case OP_LZCNT: { uint32_t v=(uint32_t)GR(RV0); int cnt=0; for(int k=31;k>=0;k--){if(!((v>>k)&1))cnt++;else break;} SR(RV1,cnt); break; }
    case OP_TZCNT: { uint32_t v=(uint32_t)GR(RV0); int cnt=0; while(cnt<32&&!((v>>cnt)&1))cnt++; SR(RV1,cnt); break; }
    case OP_TEST:   set_c(c,RV2,(GR(RV0)&GR(RV1))?1:0); break;
    case OP_XCHG: { int32_t t=GR(RV0); SR(RV0,GR(RV1)); SR(RV1,t); break; }
    case OP_BT:   set_c(c,RV2,((uint32_t)GR(RV0)>>(imm&31))&1); break;
    case OP_BTS:  SR(RV2,GR(RV0)|(1<<(imm&31))); break;
    case OP_BTR:  SR(RV2,GR(RV0)&~(1<<(imm&31))); break;
    case OP_BTC:  SR(RV2,GR(RV0)^(1<<(imm&31))); break;

    /* ── COMPARE ── */
    case OP_CMP:  { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,(a<b)?-1:(a>b)?1:0); break; }
    case OP_LCMP: { int64_t a=get_l(c,RV0),b=get_l(c,RV1); set_c(c,RV2,(a<b)?-1:(a>b)?1:0); break; }
    case OP_FCMP: { float a=get_f(c,RV0),b=get_f(c,RV1); set_c(c,RV2,(a<b)?-1:(a>b)?1:0); break; }
    case OP_DCMP: { double a=get_d(c,RV0),b=get_d(c,RV1); set_c(c,RV2,(a<b)?-1:(a>b)?1:0); break; }
    case OP_EQ:  { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,a==b?1:0); break; }
    case OP_NE:  { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,a!=b?1:0); break; }
    case OP_GT:  { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,a>b?1:0);  break; }
    case OP_LT:  { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,a<b?1:0);  break; }
    case OP_GTE: { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,a>=b?1:0); break; }
    case OP_LTE: { int32_t a=GR(RV0),b=has_imm?(int32_t)imm:GR(RV1); uint8_t d=has_imm?RV1:RV2; set_c(c,d,a<=b?1:0); break; }

    /* ── CONTROL FLOW ── */
    case OP_JMP:
    case OP_GOTO:
        /* has_imm=1: relative jump by signed offset; has_imm=0: indirect via register */
        if (has_imm) c->pc = label_addr;
        else         c->pc = (uint32_t)GR(RV0);
        break;
    case OP_JCC:
    case OP_JE:    if ( get_c(c,RV0))    c->pc=label_addr; break;
    case OP_JNE:   if (!get_c(c,RV0))    c->pc=label_addr; break;
    case OP_JG:    if ( get_c(c,RV0)>0)  c->pc=label_addr; break;
    case OP_JGE:   if ( get_c(c,RV0)>=0) c->pc=label_addr; break;
    case OP_JL:    if ( get_c(c,RV0)<0)  c->pc=label_addr; break;
    case OP_JLE:   if ( get_c(c,RV0)<=0) c->pc=label_addr; break;
    case OP_JA:    if ((uint32_t)get_c(c,RV0)>0)  c->pc=label_addr; break;
    case OP_JB:    if ((uint32_t)get_c(c,RV0)==0)  c->pc=label_addr; break;
    case OP_LOOP:  c->i[0]--; if(c->i[0]!=0) c->pc=label_addr; break;
    case OP_CALL:  push32(c,c->pc); c->pc=label_addr; break;
    case OP_RET:   c->pc=pop32(c); break;
    case OP_RETN:  c->pc=pop32(c); c->sp+=(uint32_t)imm; break;
    case OP_EXIT:  exit((int)(has_imm ? (int32_t)imm : GR(RV0))); break;

    /* ── SYSTEM ── */
    case OP_INT:   abios_handle(c,(uint8_t)imm); break;
    case OP_IRET:  c->pc=pop32(c); c->intf=1; break;
    case OP_CLI:   c->intf=0; break;
    case OP_STI:   c->intf=1; break;
    case OP_CPUID: c->a[0]=CXIS_SIG; c->a[1]=CXIS_VER; c->a[2]=CXIS_FEAT_FLOAT|CXIS_FEAT_DOUBLE|CXIS_FEAT_ABIOS; c->a[3]=0; break;
    case OP_RDTSC: c->l[0]=(int64_t)elapsed_ns(c); break;
    case OP_WAIT:  break;
    case OP_PAUSE: break;
    case OP_UD:
        fprintf(stderr,"cxemu: #UD at pc=0x%08X\n", c->pc-2);
        c->running=0; break;
    case OP_IN:    SR(RV1,0); break;
    case OP_OUT:   break;

    /* ── TYPE CONVERSIONS ── */
    case OP_ITOF: set_f(c,RV1,(float)get_i(c,RV0)); break;
    case OP_ITOD: set_d(c,RV1,(double)get_i(c,RV0)); break;
    case OP_ITOL: set_l(c,RV1,(int64_t)get_i(c,RV0)); break;
    case OP_LTOF: set_f(c,RV1,(float)get_l(c,RV0)); break;
    case OP_LTOD: set_d(c,RV1,(double)get_l(c,RV0)); break;
    case OP_FTOI: set_i(c,RV1,(int32_t)get_f(c,RV0)); break;
    case OP_FTOD: set_d(c,RV1,(double)get_f(c,RV0)); break;
    case OP_FTOL: set_l(c,RV1,(int64_t)get_f(c,RV0)); break;
    case OP_DTOI: set_i(c,RV1,(int32_t)get_d(c,RV0)); break;
    case OP_DTOF: set_f(c,RV1,(float)get_d(c,RV0)); break;
    case OP_DTOL: set_l(c,RV1,(int64_t)get_d(c,RV0)); break;
    case OP_LTOI: set_i(c,RV1,(int32_t)get_l(c,RV0)); break;

    default:
        fprintf(stderr,"cxemu: unknown opcode 0x%02X at pc=0x%08X\n",
                opcode, c->pc-2);
        c->running=0; break;
    }

#undef GR
#undef SR
#undef RV0
#undef RV1
#undef RV2
}

/* ════════════════════════════════════════════════════════════════
   CXE LOADER
════════════════════════════════════════════════════════════════ */

/* load a .cxe file; load_base=0 uses vaddrs from file, else overrides to load_base */
static int load_cxe_at(CPU *c, const char *path, uint32_t load_base) {
    FILE *f = fopen(path,"rb");
    if (!f) { fprintf(stderr,"cxemu: cannot open '%s': %s\n",path,strerror(errno)); return 0; }

    CxeHeader hdr;
    if (fread(&hdr,sizeof(hdr),1,f) != 1 || hdr.magic != CXE_MAGIC) {
        fprintf(stderr,"cxemu: '%s' is not a valid .cxe file\n",path);
        fclose(f); return 0;
    }

    /* when load_base is set, find the lowest vaddr in the file and use
       it as the origin — all sections are rebased relative to it */
    uint32_t origin = 0xFFFFFFFFu;
    if (load_base) {
        long hdr_end = ftell(f);
        for (int i = 0; i < hdr.section_count; i++) {
            CxeSection sec;
            fread(&sec, sizeof(sec), 1, f);
            if (!(sec.flags & CXE_SEC_ZERO) && sec.vaddr < origin)
                origin = sec.vaddr;
        }
        fseek(f, hdr_end, SEEK_SET);
        if (origin == 0xFFFFFFFFu) origin = hdr.entry_point;
    }

    for (int i = 0; i < hdr.section_count; i++) {
        CxeSection sec;
        fread(&sec,sizeof(sec),1,f);
        long saved = ftell(f);

        uint32_t dest = load_base ? (load_base + (sec.vaddr - origin)) : sec.vaddr;

        if (dest + sec.mem_size > c->ram_size) {
            fprintf(stderr,"cxemu: section %d out of RAM\n",i);
            fclose(f); return 0;
        }
        if (sec.flags & CXE_SEC_ZERO) {
            memset(c->ram + dest, 0, sec.mem_size);
        } else {
            fseek(f, sec.offset, SEEK_SET);
            fread(c->ram + dest, 1, sec.file_size, f);
            if (sec.mem_size > sec.file_size)
                memset(c->ram + dest + sec.file_size, 0, sec.mem_size - sec.file_size);
        }
        fseek(f, saved, SEEK_SET);
    }

    fclose(f);
    fprintf(stderr,"cxemu: loaded '%s' at 0x%08X (cxe-entry=0x%08X)\n",
            path, load_base ? load_base : origin, hdr.entry_point);
    /* return the actual RAM address of the entry point */
    if (load_base && origin != 0xFFFFFFFFu)
        return (int)(load_base + (hdr.entry_point - origin));
    return (int)hdr.entry_point;
}

/* convenience wrappers */
static int load_bootloader(CPU *c, const char *path) {
    return load_cxe_at(c, path, MEM_BOOT_BASE) > 0 ? 1 : 0;
}
/* loads BIOS and returns the actual RAM entry address, 0 on failure */
static uint32_t load_abios_rom_entry(CPU *c, const char *path) {
    int r = load_cxe_at(c, path, MEM_BIOS_ROM);
    return (r > 1) ? (uint32_t)r : (r == 1 ? MEM_BIOS_ROM : 0);
}
static int load_abios_rom(CPU *c, const char *path) {
    return load_abios_rom_entry(c, path) ? 1 : 0;
}

/* ════════════════════════════════════════════════════════════════
   HW INIT — zero RAM regions, set up IVT, VRAM, data area
   Called before any code runs. ABIOS ROM then runs as real CXIS.
════════════════════════════════════════════════════════════════ */

static void hw_init(CPU *c, uint32_t bios_entry) {
    /* ── zero low RAM ── */
    memset(c->ram, 0, MEM_ABIOS_DATA_SIZE * 2);

    /* ── reset stub at 0x0000: GOTO bios_entry ── */
    if (bios_entry) {
        uint8_t reset_stub[6] = {
            0x61, 0x60,
            (uint8_t)(bios_entry),
            (uint8_t)(bios_entry >> 8),
            (uint8_t)(bios_entry >> 16),
            (uint8_t)(bios_entry >> 24),
        };
        memcpy(c->ram, reset_stub, sizeof(reset_stub));
    }

    /* ── ABIOS data area: zero + set defaults ── */
    memset(c->ram + MEM_ABIOS_DATA, 0, MEM_ABIOS_DATA_SIZE);
    ada_w32(c, ADA_HEAP_PTR,  MEM_HEAP_BASE);
    ada_w8 (c, ADA_ATTR,      VATTR(COL_LGRAY, COL_BLACK));
    ada_w8 (c, ADA_DISK_CNT,  (uint8_t)c->disk_count);
    ada_w8 (c, ADA_BOOT_DISK, c->disk_count > 0 ? 0 : 0xFF);
    /* BIOS_FLAG written by ABIOS ROM at runtime */

    /* ── blank VRAM ── */
    uint8_t attr = VATTR(COL_LGRAY, COL_BLACK);
    for (uint32_t i = 0; i < (uint32_t)(VID_COLS * VID_ROWS); i++) {
        c->ram[MEM_VRAM_BASE + i*2]   = ' ';
        c->ram[MEM_VRAM_BASE + i*2+1] = attr;
    }

    /* ── write memory map ── */
    abios_write_memmap(c);

    /* ── CPU reset: pc = 0x0000 — executes reset stub ── */
    c->pc = 0x00000000;
}

/* ════════════════════════════════════════════════════════════════
   MAIN
════════════════════════════════════════════════════════════════ */

static void usage(void) {
    fprintf(stderr,
        "cxemu — CXIS Machine Emulator + ABIOS\n"
        "Usage:  cxemu [options]\n"
        "Options:\n"
        "  --floppy <file>  attach floppy disk image (highest boot priority)\n"
        "  --hdd    <file>  attach hard disk image\n"
        "  --ssd    <file>  attach SSD disk image\n"
        "  --cdrom  <file>  attach CD-ROM image (lowest boot priority)\n"
        "  --bios   <file>  load BIOS from .bios or .cxe file\n"
        "                   (.bios files have CXEBIOS header, .cxe loaded directly)\n"
        "  --trace          print pc/regs every instruction\n"
        "  --help           this message\n"
        "\n"
        "Boot order: floppy -> hdd -> ssd -> cdrom\n"
        "  Reads 1024 bytes from sector 0, checks for 'CXEBOOT' magic at offset 0,\n"
        "  then loads 1MB from disk into RAM at 0x00001C00 and jumps to it.\n"
        "\n"
        "ABIOS interrupt vectors:\n"
        "  int 0x01  console   int 0x02  keyboard  int 0x03  video\n"
        "  int 0x04  disk      int 0x05  memory    int 0x06  timer\n"
        "  int 0x07  power     int 0x08  sysinfo   int 0x09  IRQ\n"
        "\n"
        "Memory map (64MB):\n"
        "  0x00000000  IVT (1KB)          0x00001000  ABIOS data\n"
        "  0x00001C00  boot load area     0x00C00000  VRAM 80x25\n"
        "  0x01000000  kernel area        0x02000000  heap\n"
        "  0x03F00000  stack top\n"
    );
}

/* ════════════════════════════════════════════════════════════════
   .bios PACKAGE LOADER
   Reads a CXEBIOS-headered file, extracts the embedded .cxe,
   and loads it at the specified load_addr in RAM.
════════════════════════════════════════════════════════════════ */

#define BIOS_PKG_MAGIC   "CXEBIOS\0"
#define BIOS_PKG_HDRSZ   32

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t flags;
    uint32_t cxe_size;
    uint32_t entry_hint;
    uint32_t load_addr;
    uint32_t reserved;
} BiosPkgHdr;

static uint32_t load_bios_pkg(CPU *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cxemu: bios: cannot open '%s': %s\n", path, strerror(errno));
        return 0;
    }

    /* peek at magic to decide .bios vs plain .cxe */
    char magic[8] = {0};
    fread(magic, 1, 8, f);
    rewind(f);

    if (memcmp(magic, BIOS_PKG_MAGIC, 8) == 0) {
        /* ── .bios package ── */
        BiosPkgHdr hdr;
        fread(&hdr, sizeof(hdr), 1, f);

        fprintf(stderr, "cxemu: bios pkg '%s'\n", path);
        fprintf(stderr, "  version:   %u.%u\n",
                (hdr.version >> 16) & 0xFFFF, hdr.version & 0xFFFF);
        fprintf(stderr, "  load addr: 0x%08X\n", hdr.load_addr);
        fprintf(stderr, "  cxe size:  %u bytes\n", hdr.cxe_size);

        /* write embedded .cxe to a temp file */
        char tmp[] = "/tmp/cxemu_bios_XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) { fprintf(stderr,"cxemu: bios: mkstemp failed\n"); fclose(f); return 0; }
        FILE *tf = fdopen(fd, "wb");
        uint8_t buf[4096];
        uint32_t rem = hdr.cxe_size;
        while (rem > 0) {
            uint32_t n = rem < sizeof(buf) ? rem : sizeof(buf);
            size_t got = fread(buf, 1, n, f);
            if (got == 0) break;
            fwrite(buf, 1, got, tf);
            rem -= (uint32_t)got;
        }
        fclose(tf);
        fclose(f);

        /* load extracted .cxe at hdr.load_addr, return entry */
        uint32_t entry = load_abios_rom_entry(c, tmp);
        remove(tmp);
        return entry;

    } else {
        /* ── plain .cxe — load directly ── */
        fclose(f);
        fprintf(stderr, "cxemu: bios (plain .cxe) '%s'\n", path);
        return load_abios_rom_entry(c, path);
    }
}


/* ════════════════════════════════════════════════════════════════
   DISK BOOT — MBR-style: read 1024 bytes, check CXEBOOT magic,
   load 1MB into MEM_BOOT_BASE, jump to it.
════════════════════════════════════════════════════════════════ */

#define CXEBOOT_LOAD_1MB  (1024 * 1024)

/* Try to boot from disk slot id. Returns 1 if booted (never returns),
   0 if disk has no valid CXEBOOT signature. */
static int disk_try_boot(CPU *c, int id) {
    if (id < 0 || id >= c->disk_count || !c->disks[id].fp) return 0;

    /* read first 1024 bytes to check signature */
    uint8_t probe[CXEBOOT_LOAD_SIZE];
    fseek(c->disks[id].fp, 0, SEEK_SET);
    size_t got = fread(probe, 1, CXEBOOT_LOAD_SIZE, c->disks[id].fp);
    if (got < CXEBOOT_LOAD_SIZE) return 0;

    /* check magic at offset 0 */
    if (memcmp(probe, CXEBOOT_MAGIC, 7) != 0) {
        fprintf(stderr, "cxemu: %s '%s': no CXEBOOT signature\n",
                disk_type_name[c->disks[id].type], c->disks[id].path);
        return 0;
    }

    fprintf(stderr, "cxemu: booting from %s '%s'\n",
            disk_type_name[c->disks[id].type], c->disks[id].path);

    /* load up to 1MB from disk into MEM_BOOT_BASE */
    uint32_t load_max = CXEBOOT_LOAD_1MB;
    if (MEM_BOOT_BASE + load_max > c->ram_size)
        load_max = c->ram_size - MEM_BOOT_BASE;

    fseek(c->disks[id].fp, 0, SEEK_SET);
    size_t loaded = fread(c->ram + MEM_BOOT_BASE, 1, load_max, c->disks[id].fp);
    fprintf(stderr, "cxemu: loaded %zu bytes from disk to 0x%08X\n",
            loaded, MEM_BOOT_BASE);

    /* record boot disk index in ABIOS data area */
    ada_w8(c, ADA_BOOT_DISK, (uint8_t)id);

    /* Write a GOTO trampoline that skips the 8-byte header. */
    {
        uint32_t boot_code = MEM_BOOT_BASE + 8;
        uint8_t tramp[6] = {
            0x61, 0x60,
            (uint8_t)(boot_code),
            (uint8_t)(boot_code >> 8),
            (uint8_t)(boot_code >> 16),
            (uint8_t)(boot_code >> 24),
        };
        memcpy(c->ram + MEM_BOOT_BASE, tramp, 6);
    }

    /* jump to boot area */
    c->pc = MEM_BOOT_BASE;
    return 1;
}

/* Try each device type in priority order: floppy -> hdd -> ssd -> cdrom */
static int disk_boot(CPU *c) {
    static const DiskType order[] = {
        DISK_TYPE_FLOPPY, DISK_TYPE_HDD, DISK_TYPE_SSD, DISK_TYPE_CDROM
    };
    for (int o = 0; o < 4; o++) {
        for (int i = 0; i < c->disk_count; i++) {
            if (c->disks[i].type == order[o]) {
                if (disk_try_boot(c, i)) return 1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    int trace = 0;
    const char *bios_path = NULL;

    CPU *cpu = calloc(1, sizeof(CPU));
    cpu->ram_size = g_machine.ram_mb * 1024 * 1024;
    if (cpu->ram_size < MEM_BOOT_BASE + 1024*1024)
        cpu->ram_size = MEM_BOOT_BASE + 1024*1024;  /* minimum to fit bootloader */
    cpu->ram      = calloc(1, cpu->ram_size);
    cpu->sp       = MEM_STACK_TOP;
    cpu->sf       = MEM_STACK_TOP;
    cpu->bp       = MEM_TEXT_BASE;
    cpu->bf       = 0;
    cpu->intf     = 1;
    cpu->boot_ns  = now_ns();

    /* helper: attach a disk image with given type */
    #define ATTACH_DISK(devtype) do { \
        if (i + 1 >= argc) { fprintf(stderr,"cxemu: %s requires a file\n", argv[i]); return 1; } \
        const char *_path = argv[++i]; \
        if (cpu->disk_count >= DISK_MAX_DRIVES) { fprintf(stderr,"cxemu: max %d disks\n", DISK_MAX_DRIVES); break; } \
        int _id = cpu->disk_count++; \
        cpu->disks[_id].fp = fopen(_path, "r+b"); \
        if (!cpu->disks[_id].fp) { fprintf(stderr,"cxemu: %s '%s': %s\n", disk_type_name[devtype], _path, strerror(errno)); cpu->disk_count--; break; } \
        fseek(cpu->disks[_id].fp, 0, SEEK_END); \
        long _sz = ftell(cpu->disks[_id].fp); \
        cpu->disks[_id].sector_count = (uint32_t)(_sz / DISK_SECTOR_SIZE); \
        cpu->disks[_id].type = (devtype); \
        strncpy(cpu->disks[_id].path, _path, 255); \
        fprintf(stderr,"cxemu: %s '%s' (%u sectors)\n", disk_type_name[devtype], _path, cpu->disks[_id].sector_count); \
    } while(0)

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace")  == 0) { trace = 1; continue; }
        if (strcmp(argv[i], "--help")   == 0) { usage(); return 0; }
        if (strcmp(argv[i], "--bios")   == 0) {
            if (i + 1 >= argc) { fprintf(stderr,"cxemu: --bios requires a file\n"); return 1; }
            bios_path = argv[++i]; continue;
        }
        if (strcmp(argv[i], "--floppy") == 0) { ATTACH_DISK(DISK_TYPE_FLOPPY); continue; }
        if (strcmp(argv[i], "--hdd")    == 0) { ATTACH_DISK(DISK_TYPE_HDD);    continue; }
        if (strcmp(argv[i], "--ssd")    == 0) { ATTACH_DISK(DISK_TYPE_SSD);    continue; }
        if (strcmp(argv[i], "--cdrom")  == 0) { ATTACH_DISK(DISK_TYPE_CDROM);  continue; }
        if (strcmp(argv[i], "--cpu") == 0) {
            if (i + 1 >= argc) { fprintf(stderr,"cxemu: --cpu requires a model name\n"); return 1; }
            const char *model = argv[++i];
            int found = 0;
            for (int m = 0; m < CPU_MODEL_COUNT; m++) {
                if (strcmp(cpu_models[m].name, model) == 0) {
                    g_machine.cpu = &cpu_models[m]; found = 1; break;
                }
            }
            if (!found) {
                fprintf(stderr, "cxemu: unknown CPU model '%s'\n  available:", model);
                for (int m = 0; m < CPU_MODEL_COUNT; m++)
                    fprintf(stderr, " %s", cpu_models[m].name);
                fprintf(stderr, "\n"); return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--gpu") == 0) {
            if (i + 1 >= argc) { fprintf(stderr,"cxemu: --gpu requires a model name\n"); return 1; }
            const char *model = argv[++i];
            int found = 0;
            for (int m = 0; m < GPU_MODEL_COUNT; m++) {
                if (strcmp(gpu_models[m].name, model) == 0) {
                    g_machine.gpu = &gpu_models[m]; found = 1; break;
                }
            }
            if (!found) {
                fprintf(stderr, "cxemu: unknown GPU model '%s'\n  available:", model);
                for (int m = 0; m < GPU_MODEL_COUNT; m++)
                    fprintf(stderr, " %s", gpu_models[m].name);
                fprintf(stderr, "\n"); return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--ram") == 0) {
            if (i + 1 >= argc) { fprintf(stderr,"cxemu: --ram requires a size e.g. 128M\n"); return 1; }
            const char *sz = argv[++i];
            char *end;
            uint32_t val = (uint32_t)strtoul(sz, &end, 10);
            if      (*end == 'M' || *end == 'm') g_machine.ram_mb = val;
            else if (*end == 'G' || *end == 'g') g_machine.ram_mb = val * 1024;
            else { fprintf(stderr,"cxemu: --ram format: 64M / 256M / 1G\n"); return 1; }
            continue;
        }
        if (strcmp(argv[i], "--cores") == 0) {
            if (i + 1 >= argc) { fprintf(stderr,"cxemu: --cores requires a number\n"); return 1; }
            g_machine.cores = atoi(argv[++i]);
            if (g_machine.cores < 1 || g_machine.cores > 64) {
                fprintf(stderr,"cxemu: --cores must be 1-64\n"); return 1;
            }
            continue;
        }
        fprintf(stderr, "cxemu: unknown argument '%s'\n", argv[i]);
        usage(); return 1;
    }

    #undef ATTACH_DISK

    /* ── boot sequence ─────────────────────────────────────────────
       1. hw_init: zero RAM, blank VRAM, init data area
       2. load BIOS into RAM, get entry point
       3. write reset stub at 0x0000 → bios entry
       4. pc = 0x0000 → reset stub → BIOS POST
       5. BIOS calls disk_boot() equivalent:
          - iterates floppy → hdd → ssd → cdrom
          - reads 1024 bytes, checks CXEBOOT magic at offset 0
          - loads 1MB from disk into MEM_BOOT_BASE
          - jumps to MEM_BOOT_BASE
    ────────────────────────────────────────────────────────────── */

    /* step 1: hw_init */
    hw_init(cpu, 0);

    /* step 1b: print machine configuration */
    fprintf(stderr, "cxemu: machine config\n");
    fprintf(stderr, "  cpu:   %s @ %s  cores: %d  features: %s\n",
            g_machine.cpu->name, g_machine.cpu->freq_str,
            g_machine.cores, g_machine.cpu->feature_str);
    fprintf(stderr, "  gpu:   %s  vram: %uMB  features: %s\n",
            g_machine.gpu->name, g_machine.gpu->vram_mb,
            g_machine.gpu->features);
    fprintf(stderr, "  ram:   %uMB\n", g_machine.ram_mb);

    /* step 2: load BIOS */
    uint32_t bios_entry = 0;
    if (bios_path) {
        bios_entry = (uint32_t)load_bios_pkg(cpu, bios_path);
        if (!bios_entry) { fprintf(stderr,"cxemu: fatal: could not load bios '%s'\n", bios_path); return 1; }
    } else {
        bios_entry = load_abios_rom_entry(cpu, "bios/abios.cxe");
        if (!bios_entry) fprintf(stderr,"cxemu: WARNING: bios not found\n");
    }

    /* step 3: write reset stub at 0x0000 → bios entry */
    {
        uint32_t e = bios_entry ? bios_entry : MEM_BIOS_ROM;
        uint8_t stub[6] = {
            0x61, 0x60,
            (uint8_t)(e), (uint8_t)(e>>8), (uint8_t)(e>>16), (uint8_t)(e>>24),
        };
        memcpy(cpu->ram, stub, sizeof(stub));
    }

    /* step 4: pre-load bootloader from disk into MEM_BOOT_BASE so it's
       ready when ABIOS jumps to 0x1C00 after POST.  We do NOT set pc here —
       the CPU still starts at 0x0000 → reset stub → ABIOS POST → goto 0x1C00. */
    {
        static const DiskType order[] = {
            DISK_TYPE_FLOPPY, DISK_TYPE_HDD, DISK_TYPE_SSD, DISK_TYPE_CDROM
        };
        int preloaded = 0;
        for (int o = 0; o < 4 && !preloaded; o++) {
            for (int i = 0; i < cpu->disk_count && !preloaded; i++) {
                if (cpu->disks[i].type != order[o]) continue;
                if (!cpu->disks[i].fp) continue;
                uint8_t probe[CXEBOOT_LOAD_SIZE];
                fseek(cpu->disks[i].fp, 0, SEEK_SET);
                if (fread(probe, 1, CXEBOOT_LOAD_SIZE, cpu->disks[i].fp) < CXEBOOT_LOAD_SIZE) continue;
                if (memcmp(probe, CXEBOOT_MAGIC, 7) != 0) {
                    fprintf(stderr, "cxemu: %s '%s': no CXEBOOT signature\n",
                            disk_type_name[cpu->disks[i].type], cpu->disks[i].path);
                    continue;
                }
                uint32_t load_max = (uint32_t)(1024*1024);
                if (MEM_BOOT_BASE + load_max > cpu->ram_size)
                    load_max = cpu->ram_size - MEM_BOOT_BASE;
                fseek(cpu->disks[i].fp, 0, SEEK_SET);
                size_t loaded = fread(cpu->ram + MEM_BOOT_BASE, 1, load_max, cpu->disks[i].fp);
                fprintf(stderr, "cxemu: pre-loaded %zu bytes from disk to 0x%08X\n",
                        loaded, MEM_BOOT_BASE);
                /* patch CXEBOOT header with trampoline to +8 (skip magic) */
                {
                    uint32_t boot_code = MEM_BOOT_BASE + 8;
                    uint8_t tramp[6] = {
                        0x61, 0x60,
                        (uint8_t)(boot_code), (uint8_t)(boot_code>>8),
                        (uint8_t)(boot_code>>16), (uint8_t)(boot_code>>24),
                    };
                    memcpy(cpu->ram + MEM_BOOT_BASE, tramp, 6);
                }
                ada_w8(cpu, ADA_BOOT_DISK, (uint8_t)i);
                preloaded = 1;
            }
        }
        if (!preloaded) {
            fprintf(stderr, "cxemu: fatal: no bootable disk found (missing CXEBOOT signature)\n");
            fprintf(stderr, "cxemu: attach a disk with --floppy/--hdd/--ssd/--cdrom\n");
            return 1;
        }
    }

    /* step 5: CPU reset — pc=0x0000 executes reset stub → ABIOS POST → goto 0x1C00 */
    fflush(stderr);
    cpu->pc      = 0x00000000;
    cpu->trace   = trace;
    cpu->running = 1;

    while (cpu->running && !cpu->halted) {
        if (trace)
            fprintf(stderr, "  pc=%08X sp=%08X i0=%d i1=%d a0=%u\n",
                    cpu->pc, cpu->sp, cpu->i[0], cpu->i[1], (uint32_t)cpu->a[0]);
        step(cpu);
        check_timers(cpu);
    }

    fprintf(stderr,"\ncxemu: halted  steps=%lld  uptime=%llus\n",
            cpu->steps, (unsigned long long)(elapsed_ns(cpu)/1000000000ULL));

    for (int i = 0; i < cpu->disk_count; i++)
        if (cpu->disks[i].fp) fclose(cpu->disks[i].fp);

    free(cpu->ram);
    free(cpu);
    return 0;
}
