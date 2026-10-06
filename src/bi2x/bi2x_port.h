#ifndef BI2X_PORT_H
#define BI2X_PORT_H

#include <stddef.h>
#include <stdint.h>

/* Platform abstraction: serial port, time, threads, locks. Implemented by
   bi2x_port_win32.c (the real thing) and bi2x_port_posix.c (for testing
   against a simulated board on a pty). */

#define BI2X_USB_VID 0x1CCF
#define BI2X_USB_PID 0x8050 /* BIO2 running BI2X firmware */

struct bi2x_serial;

/* port == NULL or "" -> search for the USB device VID_1CCF&PID_8050 the same
   way libaio does. Otherwise a device path such as "COM3" / "\\\\.\\COM3"
   (win32) or "/dev/pts/5" (posix). Returns 0 on success. */
int bi2x_serial_open(struct bi2x_serial **out, const char *port);
void bi2x_serial_close(struct bi2x_serial *s);

/* Returns 0 on success, -1 on an I/O error (device probably gone). */
int bi2x_serial_write(struct bi2x_serial *s, const void *buf, size_t len);

/* Wait up to timeout_ms for data. Returns number of bytes read (0 on
   timeout), -1 on an I/O error. */
int bi2x_serial_read(
    struct bi2x_serial *s, void *buf, size_t cap, unsigned int timeout_ms);

/* Assert / release a line break. -1 on error. */
int bi2x_serial_set_break(struct bi2x_serial *s, int on);

/* Drop anything sitting in the OS buffers. */
void bi2x_serial_purge(struct bi2x_serial *s);

uint64_t bi2x_time_ms(void);
void bi2x_sleep_ms(unsigned int ms);

struct bi2x_mutex;
struct bi2x_mutex *bi2x_mutex_create(void);
void bi2x_mutex_lock(struct bi2x_mutex *m);
void bi2x_mutex_unlock(struct bi2x_mutex *m);
void bi2x_mutex_destroy(struct bi2x_mutex *m);

/* Optional external thread API (bemanitools passes these to iidx_io_init). */
typedef int (*bi2x_thread_create_t)(
    int (*proc)(void *), void *ctx, uint32_t stack_sz, unsigned int priority);
typedef void (*bi2x_thread_join_t)(int thread_id, int *result);
typedef void (*bi2x_thread_destroy_t)(int thread_id);

struct bi2x_thread_api {
    bi2x_thread_create_t create;
    bi2x_thread_join_t join;
    bi2x_thread_destroy_t destroy;
};

struct bi2x_thread;
struct bi2x_thread *bi2x_thread_start(
    const struct bi2x_thread_api *api, int (*proc)(void *), void *ctx);
void bi2x_thread_join(struct bi2x_thread *t);

/* Directory containing the running module (our DLL / EXE), with trailing
   separator. Empty string if unknown. */
void bi2x_self_dir(char *out, size_t cap);

#endif
