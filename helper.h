#ifndef GBN_HELPER_H
#define GBN_HELPER_H

#include <errno.h>
#include <stdlib.h>

#define STR_ERROR() \
    (errno == 0 ? "None" : strerror(errno))
#define ERR_CHECK(A, M, ...) \
    if (!(A)) {DBG_ERROR(M, ##__VA_ARGS__); errno = 0; goto error_exit;}
#define DBG_ERROR(M, ...) \
    dbg_log_print(__FILE__, __LINE__, "[ERROR]: %s. " M, STR_ERROR(), ##__VA_ARGS__)
#define DBG_PRINT(...) \
    /*dbg_log_print(__FILE__, __LINE__, ##__VA_ARGS__)*/

extern char module_name[200];

void get_current_time(char (*time_str)[], size_t maxsize);

int itoa(char* buf, int number);

void dbg_log_print(char* fname, int lnum, char* fmt, ...);

#endif
