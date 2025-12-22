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
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;
void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}
void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}
struct limine_framebuffer *framebuffer_info = NULL;
static void hcf(void) {
    for (;;) {
#if defined(__x86_64__)
        asm("hlt");
#elif defined(__aarch64__) || defined(__riscv)
        asm("wfi");
#elif defined(__loongarch64)
        asm("idle 0");
#endif
    }
}
static int atoim(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}
struct kursor {
    uint32_t *fb;
    size_t pitch;
    size_t width;
    size_t height;
    size_t x, y;
    uint32_t fg, bg;
};
static const uint32_t ansi_fg_colors[8] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA
};
static const uint32_t ansi_bg_colors[8] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA
};
struct kursor console;
void fbinit(struct limine_framebuffer *info, uint32_t fg, uint32_t bg) {
    console.fb = info->address;
    console.pitch = info->pitch;
    console.width = info->width;
    console.height = info->height;
    console.x = console.y = 0;
    console.fg = fg;
    console.bg = bg;
    memset(console.fb, 0, console.width * console.height * 4);
}
static void clear(void) {
    memset(console.fb, 0, console.width * console.height * 4);
    console.x = 0;
    console.y = 0;
}
static void fb_sgr_code(int code) {
    if (code == 0) {
        console.fg = 0xFFFFFF;
        console.bg = 0x000000;
        return;
    }
    if (code >= 30 && code <= 37) {
        console.fg = ansi_fg_colors[code - 30];
        return;
    }
    if (code >= 40 && code <= 47) {
        console.bg = ansi_bg_colors[code - 40];
        return;
    }
}
static void fbputc(char c) {
    if (c == '\n') {
        console.x = 0;
        console.y += 8;
        if (console.y + 8 > console.height) {
            clear();
        }
        return;
    }
    if ((unsigned char)c < 32 || (unsigned char)c >= 128) return;
    if (console.x + 8 > console.width) {
        console.x = 0;
        console.y += 8;
        if (console.y + 8 > console.height) {
            clear();
        }
    }
    const unsigned char *glyph = font8x8_basic[(unsigned char)c];
    for (size_t row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (size_t col = 0; col < 8; col++) {
            size_t px = console.x + col;
            size_t py = console.y + row;
            console.fb[py * (console.pitch / 4) + px] = (bits & (1 << col)) ? console.fg : console.bg;
        }
    }
    console.x += 8;
}
void fbputs(const char *s) {
    static enum { NORMAL, ESC, CSI } state = NORMAL;
    static char csi_buf[16];
    static int csi_len = 0;
    while (*s) {
        char c = *s++;
        switch (state) {
        case NORMAL:
            if (c == 0x1B) state = ESC;
            else fbputc(c);
            break;
        case ESC:
            if (c == '[') {
                state = CSI;
                csi_len = 0;
            } else state = NORMAL;
            break;
        case CSI:
            if (c >= '0' && c <= '9') {
                if (csi_len < (int)(sizeof(csi_buf) - 1))
                    csi_buf[csi_len++] = c;
            } else if (c == ';') {
                csi_buf[csi_len] = 0;
                fb_sgr_code(atoim(csi_buf));
                csi_len = 0;
            } else if (c == 'm') {
                csi_buf[csi_len] = 0;
                if (csi_len > 0) fb_sgr_code(atoim(csi_buf));
                state = NORMAL;
            } else {
                state = NORMAL;
            }
            break;
        }
    }
}
#define INPUT_BUFFER_SIZE 128
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_len = 0;
static bool left_shift = false;
static bool right_shift = false;
static bool caps_lock = false;
static bool key_pressed[128] = {0};
static const char scancode_ascii_normal[256] = {
    [0x01] = 0x1B,[0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',
    [0x0B]='0',[0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',[0x1E]='a',[0x1F]='s',
    [0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '
};
static const char scancode_ascii_shift[256] = {
    [0x01]=0x1B,[0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',
    [0x0B]=')',[0x0C]='_',[0x0D]='+',[0x0E]='\b',[0x0F]='\t',[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',[0x1E]='A',[0x1F]='S',
    [0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' '
};
static char poll_keyboard_single(void) {
    uint8_t scancode;
    asm volatile("inb %1, %0" : "=a"(scancode) : "Nd"(0x60));
    bool is_break = scancode & 0x80;
    uint8_t code = scancode & 0x7F;
    static uint64_t key_repeat_timer[128] = {0};
    static bool key_repeating[128] = {false};
    static uint64_t global_timer = 0;
    global_timer++;
    if (code == 0x2A) { left_shift = !is_break; return 0; }
    if (code == 0x36) { right_shift = !is_break; return 0; }
    if (code == 0x3A) {
        if (is_break) caps_lock = !caps_lock;
        return 0;
    }
    if (is_break) {
        key_pressed[code] = false;
        key_repeating[code] = false;
        key_repeat_timer[code] = 0;
        return 0;
    }
    if (!key_pressed[code]) {
        key_pressed[code] = true;
        key_repeating[code] = false;
        key_repeat_timer[code] = global_timer + 1000000;
        bool shift = left_shift || right_shift;
        char c = shift ? scancode_ascii_shift[code] : scancode_ascii_normal[code];
        if (!shift && caps_lock && c >= 'a' && c <= 'z') {
            c -= ('a' - 'A');
        }
        return c;
    }
    if (global_timer < key_repeat_timer[code]) return 0;
    if (!key_repeating[code]) {
        key_repeating[code] = true;
        key_repeat_timer[code] = global_timer + 800000;
    } else {
        key_repeat_timer[code] = global_timer + 800000;
    }
    bool shift = left_shift || right_shift;
    char c = shift ? scancode_ascii_shift[code] : scancode_ascii_normal[code];
    if (!shift && caps_lock && c >= 'a' && c <= 'z') {
        c -= ('a' - 'A');
    }
    return c;
}
static void draw_cursor(bool visible) {
    if (console.x+8>console.width||console.y+8>console.height) return;
    uint32_t fg = visible?0xFFFFFF:0x000000;
    uint32_t bg = fg;
    const unsigned char *glyph = font8x8_basic['k'];
    for(size_t row=0; row<8; row++)
        for(size_t col=0; col<8; col++) {
            size_t px=console.x+col, py=console.y+row;
            bool bit = glyph[row] & (1<<col);
            console.fb[py*(console.pitch/4)+px]=bit?fg:bg;
        }
}
char *fbgets(void) {
    input_len=0;
    memset(input_buffer,0,INPUT_BUFFER_SIZE);
    bool cursor_state=true;
    uint64_t blink_counter=0;
    static uint64_t enter_repeat_delay = 0;
    static uint64_t enter_repeat_timer = 0;
    static uint64_t global_timer = 0;
    global_timer++;
    while(1) {
        blink_counter++;
        if(blink_counter>500000){ blink_counter=0; cursor_state=!cursor_state; }
        draw_cursor(cursor_state);
        char c = poll_keyboard_single();
        bool enter_pressed = key_pressed[0x1C];
        if (c) {
            if (cursor_state) draw_cursor(false);
            static enum {NG_NORMAL, NG_ESC, NG_CSI} ng = NG_NORMAL;
            static char ng_buf[16];
            static int ng_len = 0;
            switch(ng) {
                case NG_NORMAL:
                    if (c == 0x1B) { ng = NG_ESC; continue; }
                    break;
                case NG_ESC:
                    if (c == '[') { ng = NG_CSI; ng_len = 0; continue; }
                    ng = NG_NORMAL;
                    break;
                case NG_CSI:
                    if (c >= '0' && c <= '9') {
                        if (ng_len < (int)(sizeof(ng_buf)-1)) ng_buf[ng_len++] = c;
                        continue;
                    } else if (c == ';') {
                        ng_buf[ng_len] = 0;
                        fb_sgr_code(atoim(ng_buf));
                        ng_len = 0;
                        continue;
                    } else if (c == 'm') {
                        ng_buf[ng_len] = 0;
                        if (ng_len > 0) fb_sgr_code(atoim(ng_buf));
                        ng = NG_NORMAL;
                        continue;
                    }
                    ng = NG_NORMAL;
                    break;
            }
            if(c=='\n') {
                fbputc('\n');
                input_buffer[input_len]='\0';
                enter_repeat_delay = 0;
                enter_repeat_timer = 0;
                return input_buffer;
            }
            else if(c=='\b') {
                if(input_len>0){ input_len--;
                    if(console.x>=8) console.x-=8;
                    else{ console.x=console.width-8; if(console.y>=8) console.y-=8;
                        if (console.y + 8 > console.height) clear();
                    }
                    for(size_t row=0;row<8;row++)
                        for(size_t col=0;col<8;col++){
                            size_t px2=console.x+col, py2=console.y+row;
                            console.fb[py2*(console.pitch/4)+px2]=console.bg;
                        }
                }
            } else {
                if(input_len<INPUT_BUFFER_SIZE-1){
                    input_buffer[input_len++]=c;
                    fbputc(c);
                }
            }
            draw_cursor(true);
            if (c == '\n') {
                enter_repeat_delay = global_timer + 1000000;
                enter_repeat_timer = 0;
            }
        }
        if (enter_pressed && c == 0) {
            if (enter_repeat_delay != 0 && global_timer >= enter_repeat_delay && enter_repeat_timer == 0) {
                enter_repeat_timer = global_timer + 800000;
                fbputc('\n');
                input_buffer[input_len]='\0';
                return input_buffer;
            }
            if (enter_repeat_timer != 0 && global_timer >= enter_repeat_timer) {
                enter_repeat_timer = global_timer + 800000;
                fbputc('\n');
                input_buffer[input_len]='\0';
                return input_buffer;
            }
        }
    }
}
#define REDROBIN_HEAP_SIZE 0x10000
static uint8_t redrobin_heap[REDROBIN_HEAP_SIZE];
static size_t redrobin_heap_offset=0;
void *redrobin_malloc(size_t size){
    if(redrobin_heap_offset+size>REDROBIN_HEAP_SIZE) return NULL;
    void*ptr=&redrobin_heap[redrobin_heap_offset];
    redrobin_heap_offset+=size;
    return ptr;
}
typedef int (*redrobin_fn)(uint32_t uid);
struct redrobin_task{
    redrobin_fn func;
    uint32_t uid;
    bool finished;
    struct redrobin_task *next;
};
static struct redrobin_task *redrobin_head=NULL;
static uint32_t next_uid=1;
uint32_t rradd(redrobin_fn fn){
    struct redrobin_task *t=(struct redrobin_task*)redrobin_malloc(sizeof(struct redrobin_task));
    if(!t) return 0;
    t->func=fn;
    t->uid=next_uid++;
    t->finished=false;
    t->next=NULL;
    if(!redrobin_head){
        redrobin_head=t;
        t->next=t;
    }else{
        struct redrobin_task *last=redrobin_head;
        while(last->next!=redrobin_head) last=last->next;
        last->next=t;
        t->next=redrobin_head;
    }
    return t->uid;
}
void rrsch(void) {
    if (!redrobin_head) return;
    struct redrobin_task *cur = redrobin_head;
    do {
        if (!cur->finished) {
            int res = cur->func(cur->uid);
            if (res != 0)
                cur->finished = true;
        }
        if (cur->finished) {
            struct redrobin_task *next = cur->next;
            if (cur->next == cur) {
                redrobin_head = NULL;
                return;
            }
            struct redrobin_task *prev = redrobin_head;
            while (prev->next != cur) prev = prev->next;
            prev->next = next;
            if (cur == redrobin_head)
                redrobin_head = next;
            cur = next;
            continue;
        }
        cur = cur->next;
    } while (cur != redrobin_head);
}
int bash(uint32_t uid);
void kmain(void) {
    if(!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) hcf();
    if(!framebuffer_request.response || framebuffer_request.response->framebuffer_count<1) hcf();
    struct limine_framebuffer *info=framebuffer_request.response->framebuffers[0];
    fbinit(info,0xFFFFFF,0x000000);
    rradd(bash);
    for(;;){
        rrsch();
    }
}
