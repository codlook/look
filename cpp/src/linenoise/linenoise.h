/* linenoise.h — minimal cross-platform line editing with history
 * Vendored for LOOK language. Supports Windows (Console API) and POSIX (termios).
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

char* linenoise(const char* prompt);
void  linenoise_free(char* line);
int   linenoiseHistoryAdd(const char* line);
void  linenoiseHistorySetMaxLen(int len);
void  linenoiseHistoryLoad(const char* filename);
void  linenoiseHistorySave(const char* filename);
void  linenoiseClearScreen(void);

#ifdef __cplusplus
}
#endif
