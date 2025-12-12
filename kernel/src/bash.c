#include "common.h"

int bash(uint32_t uid) {
	static bool first = true;
	if (first) {
		fbputs("KUDOS 0.1 BASH, MAINTAINED BY THE KATOOLS PROJECT.\n\n");
		first = false;
	}
	while(1) {
        	fbputs("root@kudos> ");
		char *line = fbgets();
		fbputs("\n");
	}
	return 1;
}
