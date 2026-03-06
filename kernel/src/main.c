#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "font8x8/font8x8_basic.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request ka_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *d = dest; const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}
void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

/* ── serial ─────────────────────────────────────────────────────────────── */
#define COM1 0x3F8
static void serial_init(void) {
    asm volatile("outb %0,%1"::"a"((uint8_t)0x00),"Nd"((uint16_t)(COM1+1)));
    asm volatile("outb %0,%1"::"a"((uint8_t)0x80),"Nd"((uint16_t)(COM1+3)));
    asm volatile("outb %0,%1"::"a"((uint8_t)0x01),"Nd"((uint16_t)(COM1+0)));
    asm volatile("outb %0,%1"::"a"((uint8_t)0x00),"Nd"((uint16_t)(COM1+1)));
    asm volatile("outb %0,%1"::"a"((uint8_t)0x03),"Nd"((uint16_t)(COM1+3)));
    asm volatile("outb %0,%1"::"a"((uint8_t)0xC7),"Nd"((uint16_t)(COM1+2)));
    asm volatile("outb %0,%1"::"a"((uint8_t)0x0B),"Nd"((uint16_t)(COM1+4)));
}
static void serial_putc(char c) {
    uint8_t lsr;
    do { asm volatile("inb %1,%0":"=a"(lsr):"Nd"((uint16_t)(COM1+5))); } while (!(lsr&0x20));
    asm volatile("outb %0,%1"::"a"((uint8_t)c),"Nd"((uint16_t)COM1));
}
static void serial_puts(const char *s) {
    while (*s) { if (*s=='\n') serial_putc('\r'); serial_putc(*s++); }
}
static void serial_puthex64(uint64_t v) {
    serial_puts("0x");
    for (int i=60;i>=0;i-=4) { uint8_t n=(v>>i)&0xF; serial_putc(n<10?'0'+n:'a'+(n-10)); }
}
static void serial_putdec(uint64_t v) {
    if (!v) { serial_putc('0'); return; }
    char buf[20]; int n=0;
    while (v) { buf[n++]='0'+(v%10); v/=10; }
    for (int i=n-1;i>=0;i--) serial_putc(buf[i]);
}
#define LOG(s)    serial_puts("[DBG] " s "\n")
#define LOGV(s,v) do { serial_puts("[DBG] " s); serial_puthex64(v); serial_putc('\n'); } while(0)
#define LOGD(s,v) do { serial_puts("[DBG] " s); serial_putdec(v);   serial_putc('\n'); } while(0)

static void hcf(void) { LOG("HCF"); asm volatile("cli"); for (;;) asm volatile("hlt"); }
static int atoim(const char *s) {
    int v=0; while (*s>='0'&&*s<='9') { v=v*10+(*s-'0'); s++; } return v;
}

/* ── allocator (forward decl needed by map_page) ────────────────────────── */
#define HEAP_SIZE 0x200000
static uint8_t heap_mem[HEAP_SIZE] __attribute__((aligned(4096)));
static size_t  heap_off = 0;
void *redrobin_malloc(size_t sz) {
    sz = (sz+15)&~(size_t)15;
    if (heap_off+sz > HEAP_SIZE) hcf();
    void *p = &heap_mem[heap_off]; heap_off += sz; return p;
}

/* ── page table mapper ──────────────────────────────────────────────────── */
#define PTE_P   (1ULL<<0)
#define PTE_W   (1ULL<<1)
#define PTE_PWT (1ULL<<3)
#define PTE_PCD (1ULL<<4)

static uint64_t hhdm_off      = 0;
static uint64_t kern_phys_off = 0;   /* ka_virt - ka_phys */

static uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(phys + hhdm_off);
}

static uint64_t alloc_pt_page(void) {
    /* Allocate from the bump allocator (virtual), convert to physical. */
    void *vp = redrobin_malloc(4096 + 4096);
    uint64_t vaddr = ((uint64_t)vp + 0xFFFULL) & ~0xFFFULL; /* 4K align */
    uint64_t phys  = vaddr - kern_phys_off;
    memset((void *)vaddr, 0, 4096);
    return phys;
}

static void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t cr3;
    asm volatile("mov %%cr3,%0":"=r"(cr3));
    cr3 &= ~(uint64_t)0xFFF;

    uint64_t idx[4] = {
        (virt>>39)&0x1FF,
        (virt>>30)&0x1FF,
        (virt>>21)&0x1FF,
        (virt>>12)&0x1FF
    };

    uint64_t cur_phys = cr3;
    uint64_t *tbl = NULL;
    for (int level = 0; level < 3; level++) {
        tbl = phys_to_virt(cur_phys);
        if (!(tbl[idx[level]] & PTE_P)) {
            uint64_t np = alloc_pt_page();
            tbl[idx[level]] = np | PTE_P | PTE_W;
        }
        cur_phys = tbl[idx[level]] & ~(uint64_t)0xFFF;
    }
    tbl = phys_to_virt(cur_phys);
    tbl[idx[3]] = phys | flags;
    asm volatile("invlpg (%0)"::"r"(virt):"memory");
}

/* ── framebuffer ────────────────────────────────────────────────────────── */
struct kursor { uint32_t *fb; size_t pitch,width,height,x,y; uint32_t fg,bg; };
static const uint32_t ansi_fg_colors[8] = {
    0x000000,0xAA0000,0x00AA00,0xAA5500,0x0000AA,0xAA00AA,0x00AAAA,0xAAAAAA };
static const uint32_t ansi_bg_colors[8] = {
    0x000000,0xAA0000,0x00AA00,0xAA5500,0x0000AA,0xAA00AA,0x00AAAA,0xAAAAAA };
struct kursor console;

void fbinit(struct limine_framebuffer *info, uint32_t fg, uint32_t bg) {
    console.fb=info->address; console.pitch=info->pitch;
    console.width=info->width; console.height=info->height;
    console.x=console.y=0; console.fg=fg; console.bg=bg;
    memset(console.fb,0,console.width*console.height*4);
}
static void clear(void) {
    memset(console.fb,0,console.width*console.height*4); console.x=console.y=0;
}
static void fb_sgr_code(int code) {
    if (code==0) { console.fg=0xFFFFFF; console.bg=0x000000; return; }
    if (code>=30&&code<=37) { console.fg=ansi_fg_colors[code-30]; return; }
    if (code>=40&&code<=47) { console.bg=ansi_bg_colors[code-40]; return; }
}
static void fbputc(char c) {
    if (c=='\n') { console.x=0; console.y+=8; if (console.y+8>console.height) clear(); return; }
    if ((unsigned char)c<32||(unsigned char)c>=128) return;
    if (console.x+8>console.width) { console.x=0; console.y+=8; if (console.y+8>console.height) clear(); }
    const unsigned char *glyph=(const unsigned char *)font8x8_basic[(unsigned char)c];
    for (size_t row=0;row<8;row++) {
        unsigned char bits=glyph[row];
        for (size_t col=0;col<8;col++) {
            size_t px=console.x+col, py=console.y+row;
            console.fb[py*(console.pitch/4)+px]=(bits&(1<<col))?console.fg:console.bg;
        }
    }
    console.x+=8;
}
void fbputs(const char *s) {
    static enum { NORMAL,ESC,CSI } state = NORMAL;
    static char csi_buf[16]; static int csi_len=0;
    while (*s) {
        char c = *s++;
        switch (state) {
        case NORMAL:
            if (c==0x1B) state=ESC;
            else fbputc(c);
            break;
        case ESC:
            if (c=='[') { state=CSI; csi_len=0; }
            else state=NORMAL;
            break;
        case CSI:
            if (c>='0'&&c<='9') {
                if (csi_len<(int)(sizeof(csi_buf)-1)) csi_buf[csi_len++]=c;
            } else if (c==';') {
                csi_buf[csi_len]=0; fb_sgr_code(atoim(csi_buf)); csi_len=0;
            } else if (c=='m') {
                csi_buf[csi_len]=0; if (csi_len>0) fb_sgr_code(atoim(csi_buf)); state=NORMAL;
            } else {
                state=NORMAL;
            }
            break;
        }
    }
}

/* ── keyboard ───────────────────────────────────────────────────────────── */
#define INPUT_BUFFER_SIZE 128
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_len=0;
static bool left_shift=false, right_shift=false, caps_lock=false;
static bool key_pressed[128]={0};

static const char scancode_ascii_normal[256]={
    [0x01]=0x1B,[0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',
    [0x0B]='0',[0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',[0x1E]='a',[0x1F]='s',
    [0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '};
static const char scancode_ascii_shift[256]={
    [0x01]=0x1B,[0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',
    [0x0B]=')',[0x0C]='_',[0x0D]='+',[0x0E]='\b',[0x0F]='\t',[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',[0x1E]='A',[0x1F]='S',
    [0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' '};

static char poll_keyboard_single(void) {
    uint8_t scancode;
    asm volatile("inb %1,%0":"=a"(scancode):"Nd"(0x60));
    bool is_break=scancode&0x80; uint8_t code=scancode&0x7F;
    static uint64_t key_repeat_timer[128]={0};
    static bool key_repeating[128]={false};
    static uint64_t global_timer=0;
    global_timer++;
    if (code==0x2A) { left_shift=!is_break; return 0; }
    if (code==0x36) { right_shift=!is_break; return 0; }
    if (code==0x3A) { if (is_break) caps_lock=!caps_lock; return 0; }
    if (is_break) { key_pressed[code]=false; key_repeating[code]=false; key_repeat_timer[code]=0; return 0; }
    bool shift=left_shift||right_shift;
    if (!key_pressed[code]) {
        key_pressed[code]=true; key_repeating[code]=false;
        key_repeat_timer[code]=global_timer+1000000;
        char c=shift?scancode_ascii_shift[code]:scancode_ascii_normal[code];
        if (!shift&&caps_lock&&c>='a'&&c<='z') c-=('a'-'A');
        return c;
    }
    if (global_timer<key_repeat_timer[code]) return 0;
    if (!key_repeating[code]) { key_repeating[code]=true; key_repeat_timer[code]=global_timer+800000; }
    else key_repeat_timer[code]=global_timer+800000;
    char c=shift?scancode_ascii_shift[code]:scancode_ascii_normal[code];
    if (!shift&&caps_lock&&c>='a'&&c<='z') c-=('a'-'A');
    return c;
}

static void draw_cursor(bool visible) {
    if (console.x+8>console.width||console.y+8>console.height) return;
    uint32_t color=visible?0xFFFFFF:0x000000;
    for (size_t row=0;row<8;row++)
        for (size_t col=0;col<8;col++)
            console.fb[(console.y+row)*(console.pitch/4)+(console.x+col)]=color;
}

char *fbgets(void) {
    input_len=0; memset(input_buffer,0,INPUT_BUFFER_SIZE);
    bool cursor_state=true; uint64_t blink_counter=0;
    static uint64_t enter_repeat_delay=0, enter_repeat_timer=0, global_timer=0;
    global_timer++;
    while (1) {
        blink_counter++;
        if (blink_counter>500000) { blink_counter=0; cursor_state=!cursor_state; }
        draw_cursor(cursor_state);
        char c=poll_keyboard_single();
        bool enter_pressed=key_pressed[0x1C];
        if (c) {
            if (cursor_state) draw_cursor(false);
            static enum { NG_NORMAL,NG_ESC,NG_CSI } ng=NG_NORMAL;
            static char ng_buf[16]; static int ng_len=0;
            switch (ng) {
            case NG_NORMAL:
                if (c==0x1B) { ng=NG_ESC; continue; }
                break;
            case NG_ESC:
                if (c=='[') { ng=NG_CSI; ng_len=0; continue; }
                ng=NG_NORMAL; break;
            case NG_CSI:
                if (c>='0'&&c<='9') { if (ng_len<(int)(sizeof(ng_buf)-1)) ng_buf[ng_len++]=c; continue; }
                else if (c==';') { ng_buf[ng_len]=0; fb_sgr_code(atoim(ng_buf)); ng_len=0; continue; }
                else if (c=='m') { ng_buf[ng_len]=0; if (ng_len>0) fb_sgr_code(atoim(ng_buf)); ng=NG_NORMAL; continue; }
                ng=NG_NORMAL; break;
            }
            if (c=='\n') {
                fbputc('\n'); input_buffer[input_len]='\0';
                enter_repeat_delay=0; enter_repeat_timer=0;
                return input_buffer;
            } else if (c=='\b') {
                if (input_len>0) {
                    input_len--;
                    if (console.x>=8) console.x-=8;
                    else { console.x=console.width-8; if (console.y>=8) console.y-=8; if (console.y+8>console.height) clear(); }
                    for (size_t row=0;row<8;row++)
                        for (size_t col=0;col<8;col++)
                            console.fb[(console.y+row)*(console.pitch/4)+(console.x+col)]=console.bg;
                }
            } else {
                if (input_len<INPUT_BUFFER_SIZE-1) { input_buffer[input_len++]=c; fbputc(c); }
            }
            draw_cursor(true);
        }
        if (enter_pressed&&c==0) {
            if (enter_repeat_delay!=0&&global_timer>=enter_repeat_delay&&enter_repeat_timer==0) {
                enter_repeat_timer=global_timer+800000;
                fbputc('\n'); input_buffer[input_len]='\0'; return input_buffer;
            }
            if (enter_repeat_timer!=0&&global_timer>=enter_repeat_timer) {
                enter_repeat_timer=global_timer+800000;
                fbputc('\n'); input_buffer[input_len]='\0'; return input_buffer;
            }
        }
    }
}

/* ── task table ─────────────────────────────────────────────────────────── */
#define TASK_STACK_SZ  16384
#define SCHED_STACK_SZ 4096
#define MAX_TASKS      16
#define TIMER_VECTOR   0x20

struct task { uint64_t rsp_save; uint32_t uid; bool active; int(*func)(uint32_t); };
static struct task tasks[MAX_TASKS];
static int         cur  = 0;
static uint32_t    nuid = 1;
static uint16_t    boot_cs=0, boot_ss=0;
static uint8_t     sched_stack_mem[SCHED_STACK_SZ] __attribute__((aligned(16)));
uint64_t           sched_stack_top = 0;
static volatile uint64_t tick_count = 0;

/* ── LAPIC (xAPIC MMIO via mapped page) ─────────────────────────────────── */
#define LAPIC_PHYS  0xFEE00000ULL
#define LAPIC_VIRT  0xFFFFFFFF80F00000ULL

static volatile uint32_t *lapic = (volatile uint32_t *)LAPIC_VIRT;

static void lapic_init(void) {
    lapic[0x0F0/4] = lapic[0x0F0/4] | 0x1FF;
    lapic[0x3E0/4] = 0x3;
    lapic[0x320/4] = TIMER_VECTOR | (1u<<17);
    lapic[0x380/4] = 0x200000;
}

/* ── scheduler ──────────────────────────────────────────────────────────── */
__attribute__((used))
uint64_t do_schedule(uint64_t old_rsp) {
    tasks[cur].rsp_save = old_rsp;
    tick_count++;
    if (tick_count<=5) {
        serial_puts("[ISR] tick "); serial_putdec(tick_count);
        serial_puts(" cur="); serial_putdec((uint64_t)cur);
        serial_puts(" old_rsp="); serial_puthex64(old_rsp); serial_putc('\n');
    }
    int next=cur;
    for (int i=1;i<=MAX_TASKS;i++) { int c=(cur+i)%MAX_TASKS; if (tasks[c].active) { next=c; break; } }
    if (tick_count<=5) {
        serial_puts("[ISR] -> next="); serial_putdec((uint64_t)next);
        serial_puts(" rsp="); serial_puthex64(tasks[next].rsp_save); serial_putc('\n');
    }
    cur=next;
    return tasks[next].rsp_save;
}

/* ── timer ISR ──────────────────────────────────────────────────────────── */
__attribute__((used, naked))
void timer_isr(void) {
    asm volatile(
        "push %%rax\n" "push %%rbx\n" "push %%rcx\n" "push %%rdx\n"
        "push %%rsi\n" "push %%rdi\n" "push %%rbp\n"
        "push %%r8\n"  "push %%r9\n"  "push %%r10\n" "push %%r11\n"
        "push %%r12\n" "push %%r13\n" "push %%r14\n" "push %%r15\n"
        "mov  %%rsp, %%rdi\n"
        "mov  sched_stack_top(%%rip), %%rsp\n"
        "call do_schedule\n"
        "movabsq %[eoi], %%rbx\n"
        "movl $0, (%%rbx)\n"
        "mov  %%rax, %%rsp\n"
        "pop %%r15\n" "pop %%r14\n" "pop %%r13\n" "pop %%r12\n"
        "pop %%r11\n" "pop %%r10\n" "pop %%r9\n"  "pop %%r8\n"
        "pop %%rbp\n" "pop %%rdi\n" "pop %%rsi\n"
        "pop %%rdx\n" "pop %%rcx\n" "pop %%rbx\n" "pop %%rax\n"
        "iretq\n"
        :: [eoi] "i"(LAPIC_VIRT + 0xB0)
        : "memory"
    );
}

/* ── task bootstrap ─────────────────────────────────────────────────────── */
__attribute__((used))
void task_body(uint64_t idx) {
    serial_puts("[TASK] entered idx="); serial_putdec(idx);
    serial_puts(" uid="); serial_putdec(tasks[idx].uid); serial_putc('\n');
    tasks[idx].func(tasks[idx].uid);
    serial_puts("[TASK] returned idx="); serial_putdec(idx); serial_putc('\n');
    tasks[idx].active=false;
    for (;;) asm volatile("hlt");
}

__attribute__((used, naked))
void task_stub(void) {
    asm volatile("mov %%rbx,%%rdi\ncall task_body\nud2\n":::"memory");
}

uint32_t rradd(int (*fn)(uint32_t)) {
    for (int i=0;i<MAX_TASKS;i++) {
        if (tasks[i].active) continue;
        uint8_t *stk=(uint8_t*)redrobin_malloc(TASK_STACK_SZ);
        if (!stk) return 0;
        uint64_t *sp=(uint64_t*)(stk+TASK_STACK_SZ);
        *--sp=(uint64_t)boot_ss;
        *--sp=(uint64_t)(stk+TASK_STACK_SZ);
        *--sp=0x202;
        *--sp=(uint64_t)boot_cs;
        *--sp=(uint64_t)task_stub;
        /* GPR frame must match ISR pop order (r15 popped first = lowest addr).
           Stack slots low->high: r15,r14,r13,r12,r11,r10,r9,r8,rbp,rdi,rsi,rdx,rcx,rbx,rax
           We build high->low (each *--sp decrements), so push rax first: */
        *--sp=0;            /* rax  [14] */
        *--sp=(uint64_t)i;  /* rbx  [13] - read by task_stub via mov rbx,rdi */
        *--sp=0;            /* rcx  [12] */
        *--sp=0;            /* rdx  [11] */
        *--sp=0;            /* rsi  [10] */
        *--sp=0;            /* rdi  [ 9] */
        *--sp=0;            /* rbp  [ 8] */
        *--sp=0;            /* r8   [ 7] */
        *--sp=0;            /* r9   [ 6] */
        *--sp=0;            /* r10  [ 5] */
        *--sp=0;            /* r11  [ 4] */
        *--sp=0;            /* r12  [ 3] */
        *--sp=0;            /* r13  [ 2] */
        *--sp=0;            /* r14  [ 1] */
        *--sp=0;            /* r15  [ 0] <- rsp_save points here */
        tasks[i].rsp_save=(uint64_t)sp;
        tasks[i].active=true; tasks[i].uid=nuid++; tasks[i].func=fn;
        serial_puts("[INIT] slot="); serial_putdec((uint64_t)i);
        serial_puts(" rsp_save="); serial_puthex64(tasks[i].rsp_save);
        serial_puts(" stub="); serial_puthex64((uint64_t)task_stub); serial_putc('\n');
        return tasks[i].uid;
    }
    return 0;
}

/* ── IDT ────────────────────────────────────────────────────────────────── */
struct idt_entry { uint16_t off0,sel; uint8_t ist,attr; uint16_t off1; uint32_t off2,_zero; } __attribute__((packed));
struct idtr { uint16_t lim; uint64_t base; } __attribute__((packed));
static struct idt_entry idt[256];
static struct idtr idtr_val;
static void idt_set(int v, void(*fn)(void), uint16_t cs) {
    uint64_t a=(uint64_t)fn;
    idt[v].off0=a&0xFFFF; idt[v].sel=cs; idt[v].ist=0; idt[v].attr=0x8E;
    idt[v].off1=(a>>16)&0xFFFF; idt[v].off2=(a>>32)&0xFFFFFFFF; idt[v]._zero=0;
}

static void pic_disable(void) {
    asm volatile("outb %0,%1"::"a"((uint8_t)0xFF),"Nd"((uint16_t)0xA1));
    asm volatile("outb %0,%1"::"a"((uint8_t)0xFF),"Nd"((uint16_t)0x21));
}

int entry(uint32_t uid);

void kmain(void) {
    serial_init();
    LOG("kmain entered");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) { LOG("bad revision"); hcf(); }
    if (!framebuffer_request.response||framebuffer_request.response->framebuffer_count<1) { LOG("no fb"); hcf(); }
    if (!hhdm_request.response) { LOG("no hhdm"); hcf(); }
    if (!ka_request.response)   { LOG("no ka");   hcf(); }

    hhdm_off = hhdm_request.response->offset;
    LOGV("hhdm_off=", hhdm_off);

    uint64_t ka_phys = ka_request.response->physical_base;
    uint64_t ka_virt = ka_request.response->virtual_base;
    kern_phys_off = ka_virt - ka_phys;
    LOGV("ka_phys=", ka_phys);
    LOGV("ka_virt=", ka_virt);
    LOGV("kern_phys_off=", kern_phys_off);

    fbinit(framebuffer_request.response->framebuffers[0], 0xFFFFFF, 0x000000);
    LOG("fbinit done");

    asm volatile("mov %%cs,%0":"=r"(boot_cs));
    asm volatile("mov %%ss,%0":"=r"(boot_ss));
    LOGV("boot_cs=", (uint64_t)boot_cs);
    LOGV("boot_ss=", (uint64_t)boot_ss);

    LOG("mapping LAPIC...");
    map_page(LAPIC_VIRT, LAPIC_PHYS, PTE_P|PTE_W|PTE_PWT|PTE_PCD);
    LOG("LAPIC mapped");

    memset(idt,0,sizeof(idt));
    idt_set(TIMER_VECTOR, timer_isr, boot_cs);
    idtr_val.lim=sizeof(idt)-1; idtr_val.base=(uint64_t)idt;
    asm volatile("lidt %0"::"m"(idtr_val));
    LOGV("IDT base=", idtr_val.base);

    pic_disable();
    LOG("PIC disabled");

    LOG("LAPIC init...");
    lapic_init();
    LOG("LAPIC init done");

    sched_stack_top=(uint64_t)(sched_stack_mem+SCHED_STACK_SZ);
    LOGV("sched_stack_top=", sched_stack_top);

    tasks[0].active=true; tasks[0].uid=nuid++; tasks[0].func=NULL; cur=0;
    rradd(entry);
    LOG("tasks ready");

    asm volatile("sti");
    LOG("hlt loop");
    for (;;) asm volatile("hlt");
}
