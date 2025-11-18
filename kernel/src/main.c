add a function where the kernel will run forever without having to embed a while(1); in a task so processes can run but the os runs something when it has nothing to run to keep it from crashing

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
struct kursor {
    uint32_t *fb;
    size_t pitch;
    size_t width;
    size_t height;
    size_t x, y;
    uint32_t fg, bg;
};
static struct kursor console;
static void fbinit(struct limine_framebuffer *info, uint32_t fg, uint32_t bg) {
    console.fb = info->address;
    console.pitch = info->pitch;
    console.width = info->width;
    console.height = info->height;
    console.x = 0;
    console.y = 0;
    console.fg = fg;
    console.bg = bg;
    memset(console.fb, 0, console.width * console.height * 4);
}
static void fbputc(char c) {
    if (c == '\n') {
        console.x = 0;
        console.y += 8;
        if (console.y + 8 > console.height) console.y = 0;
        return;
    }
    if ((unsigned char)c < 32 || (unsigned char)c >= 128) return;
    if (console.x + 8 > console.width) {
        console.x = 0;
        console.y += 8;
        if (console.y + 8 > console.height) console.y = 0;
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
static void fbputs(const char *s) {
    while (*s) fbputc(*s++);
}
#define INPUT_BUFFER_SIZE 128
static char input_buffer[INPUT_BUFFER_SIZE];
static size_t input_len = 0;
static bool left_shift = false;
static bool right_shift = false;
static bool caps_lock = false;
static uint8_t last_scancode = 0;
static const char scancode_ascii_normal[256] = {
    [0x01] = 0x1B,[0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',
    [0x0B]='0',[0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',[0x1E]='a',[0x1F]='s',
    [0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '
};
static const char scancode_ascii_shift[256] = {
    [0x01]=0x1B,[0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',
    [0x0B]=')',[0x0C]='_',[0x0D]='+',[0x0E]='\b',[0x0F]='\t',[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',[0x1E]='A',[0x1F]='S',
    [0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' '
};
static char poll_keyboard_single(void) {
    uint8_t scancode;
    asm volatile("inb %1, %0" : "=a"(scancode) : "Nd"(0x60));
    if (scancode & 0x80) {
        uint8_t make = scancode & 0x7F;
        if (make == 0x2A) left_shift = false;
        if (make == 0x36) right_shift = false;
        if (make == last_scancode) last_scancode = 0;
        return 0;
    }
    if (scancode == last_scancode) return 0;
    last_scancode = scancode;
    if (scancode == 0x2A) { left_shift = true; return 0; }
    if (scancode == 0x36) { right_shift = true; return 0; }
    if (scancode == 0x3A) { caps_lock = !caps_lock; return 0; }
    bool shift = left_shift || right_shift;
    char c = shift ? scancode_ascii_shift[scancode] : scancode_ascii_normal[scancode];
    if (!shift && caps_lock && c >= 'a' && c <= 'z') c = c - ('a'-'A');
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
    while(1){
        blink_counter++;
        if(blink_counter>500000){ blink_counter=0; cursor_state=!cursor_state; }
        draw_cursor(cursor_state);
        char c=poll_keyboard_single();
        if(!c) continue;
        if(cursor_state) draw_cursor(false);
        if(c=='\n'){ fbputc('\n'); input_buffer[input_len]='\0'; last_scancode=0; return input_buffer; }
        else if(c=='\b'){
            if(input_len>0){ input_len--;
                if(console.x>=8) console.x-=8;
                else{ console.x=console.width-8; if(console.y>=8) console.y-=8; }
                for(size_t row=0;row<8;row++)
                    for(size_t col=0;col<8;col++){
                        size_t px2=console.x+col, py2=console.y+row;
                        console.fb[py2*(console.pitch/4)+px2]=console.bg;
                    }
            }
        } else { if(input_len<INPUT_BUFFER_SIZE-1){ input_buffer[input_len++]=c; fbputc(c); } }
        draw_cursor(true);
    }
}
#define REDROBIN_HEAP_SIZE 0x10000
static uint8_t redrobin_heap[REDROBIN_HEAP_SIZE];
static size_t redrobin_heap_offset=0;
void *redrobin_malloc(size_t size){ if(redrobin_heap_offset+size>REDROBIN_HEAP_SIZE)return NULL; void*ptr=&redrobin_heap[redrobin_heap_offset]; redrobin_heap_offset+=size; return ptr; }
typedef void (*redrobin_fn)(void);
struct redrobin_task{ redrobin_fn func; struct redrobin_task *next; bool finished; };
static struct redrobin_task *redrobin_head=NULL;
static struct redrobin_task *redrobin_current=NULL;
void redrobin_add(redrobin_fn fn){
    struct redrobin_task *t=(struct redrobin_task*)redrobin_malloc(sizeof(struct redrobin_task));
    if(!t) return;
    t->func=fn;
    t->finished=false;
    if(!redrobin_head){ t->next=t; redrobin_head=t; } 
    else{ struct redrobin_task *last=redrobin_head; while(last->next!=redrobin_head) last=last->next; last->next=t; t->next=redrobin_head; }
}
void redrobin_schedule(void){
    if(!redrobin_head) return;
    if(!redrobin_current) redrobin_current=redrobin_head;
    struct redrobin_task *start=redrobin_current;
    do {
        if(!redrobin_current->finished && redrobin_current->func) redrobin_current->func();
        redrobin_current=redrobin_current->next;
    } while(redrobin_current!=start);
}
void task1(void){
    fbputs("Welcome to 0.0 Kudos!\nType something and press Enter:\n> ");
    char *input=fbgets();
    fbputs("You typed: ");
    fbputs(input);
    while(1);
}
void kmain(void){
    if(!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) hcf();
    if(!framebuffer_request.response || framebuffer_request.response->framebuffer_count<1) hcf();
    struct limine_framebuffer *info=framebuffer_request.response->framebuffers[0];
    fbinit(info,0xFFFFFF,0x000000);
    redrobin_add(task1);
    redrobin_schedule();
}
