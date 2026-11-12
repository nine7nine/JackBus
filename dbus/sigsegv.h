#ifndef __SIGSEGV_H__
#define __SIGSEGV_H__

#ifdef __cplusplus
extern "C"
#endif
int setup_sigsegv();

/* Also write crash reports to this (already open, O_APPEND) file descriptor. */
#ifdef __cplusplus
extern "C"
#endif
void sigsegv_set_log_fd(int fd);

#endif
