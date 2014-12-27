#ifndef __elog__
#define __elog__
#ifdef  __cplusplus
extern "C" {
#endif
void elog_log( int flag, const char *format, ... );
void elog_notify( int flag, const char *format, ... );
void elog_complain( int flag, const char *format, ... );
void elog_die( int flag, const char *format, ... );
#ifdef  __cplusplus
}
#endif
#endif
