#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

struct kursor {
    uint32_t *fb;
    size_t pitch;
    size_t width;
    size_t height;
    size_t x, y;
    uint32_t fg, bg;
};

extern struct kursor console;

void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);
int memcmp(const void *s1, const void *s2, size_t n);

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

void fbinit(struct limine_framebuffer *fb, uint32_t fg, uint32_t bg);
void fbputc(char c);
void fbputs(const char *s);
char *fbgets(void);

typedef int (*rr_task_fn)(uint32_t uid);

__attribute__((noreturn)) void panic(const char *msg);

#define hcf() for(;;) __asm__ __volatile__("hlt")

#define COLOR_WHITE    0xFFFFFF
#define COLOR_BLACK    0x000000
#define COLOR_RED      0xFF0000
#define COLOR_GREEN    0x00FF00
#define COLOR_BLUE     0x0000FF
#define COLOR_YELLOW   0xFFFF00
#define COLOR_CYAN     0x00FFFF
#define COLOR_MAGENTA  0xFF00FF
#define COLOR_GRAY     0x888888

#define STATIC_ASSERT(c,n) _Static_assert(c, "assert "#n)
