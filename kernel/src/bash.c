#include "common.h"

static void clear(void) {
    memset(console.fb, 0, console.width * console.height * 4);
    console.x = 0;
    console.y = 0;
}

int bash(uint32_t uid) {
    (void)uid;
    static bool first = true;
    if (first) {
        fbputs("0.0 KUDOS BASH, MAINTAINED BY THE KATOOLS PROJECT.\n\n");
        first = false;
    }
    while (1) {
        fbputs("root@kudos> ");
        char *line = fbgets();
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (cmd[0] == '\0') {
            fbputs("\n");
            continue;
        }
        char *arg = cmd;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        if (*arg) {
            *arg = '\0';
            arg++;
            while (*arg == ' ' || *arg == '\t') arg++;
        } else {
            arg = NULL;
        }
        if (strcmp(cmd, "clear") == 0) {
            clear();
        } else if (strcmp(cmd, "echo") == 0) {
            if (arg) {
                while (*arg) {
                    if (*arg == '\\' && *(arg + 1) == 'n') {
                        fbputs("\n");
                        arg += 2;
                    } else {
                        char tmp[2] = {*arg, '\0'};
                        fbputs(tmp);
                        arg++;
                    }
                }
            }
            fbputs("\n");
        } else if (strcmp(cmd, "help") == 0) {
            fbputs("Available commands:\n");
            fbputs(" clear - clear the screen\n");
            fbputs(" echo - print text (supports \\n for newline)\n");
            fbputs(" help - show this help\n");
            fbputs("\n");
        } else {
            fbputs(cmd);
            fbputs(": command not found\n");
        }
    }
    return 1;
}
