#ifdef _WIN32

#include <windows.h>

#include <setupapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bi2x_log.h"
#include "bi2x_port.h"

/* GUID_DEVINTERFACE_USB_DEVICE: what libaio opens (aioSciComm "USB CDC") */
static const GUID bi2x_guid_usb_device = {
    0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

/* GUID_DEVINTERFACE_COMPORT: fallback */
static const GUID bi2x_guid_comport = {
    0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};

struct bi2x_serial {
    HANDLE fd;
};

struct bi2x_mutex {
    CRITICAL_SECTION cs;
};

struct bi2x_thread {
    const struct bi2x_thread_api *api;
    int ext_id;
    HANDLE handle;
    int (*proc)(void *);
    void *ctx;
};

static int find_device_path(char *out, size_t cap)
{
    const GUID *guids[2] = {&bi2x_guid_usb_device, &bi2x_guid_comport};
    char want_vid[16];
    char want_pid[16];
    HDEVINFO devs;
    SP_DEVINFO_DATA info;
    DWORD i;
    int found = 0;

    snprintf(want_vid, sizeof(want_vid), "VID_%04X", BI2X_USB_VID);
    snprintf(want_pid, sizeof(want_pid), "PID_%04X", BI2X_USB_PID);

    devs = SetupDiGetClassDevsA(
        NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE);

    if (devs == INVALID_HANDLE_VALUE) {
        return -1;
    }

    for (i = 0; !found; i++) {
        char inst[512];
        size_t k;
        int g;

        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);

        if (!SetupDiEnumDeviceInfo(devs, i, &info)) {
            break;
        }

        if (!SetupDiGetDeviceInstanceIdA(
                devs, &info, inst, sizeof(inst), NULL)) {
            continue;
        }

        for (k = 0; inst[k]; k++) {
            if (inst[k] >= 'a' && inst[k] <= 'z') {
                inst[k] = (char) (inst[k] - 'a' + 'A');
            }
        }

        if (strstr(inst, want_vid) == NULL || strstr(inst, want_pid) == NULL) {
            continue;
        }

        for (g = 0; g < 2 && !found; g++) {
            SP_DEVICE_INTERFACE_DATA ifd;
            SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail;
            DWORD needed = 0;

            memset(&ifd, 0, sizeof(ifd));
            ifd.cbSize = sizeof(ifd);

            if (!SetupDiEnumDeviceInterfaces(devs, &info, guids[g], 0, &ifd)) {
                continue;
            }

            SetupDiGetDeviceInterfaceDetailA(devs, &ifd, NULL, 0, &needed, NULL);

            if (needed == 0) {
                continue;
            }

            detail = malloc(needed);

            if (detail == NULL) {
                continue;
            }

            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

            if (SetupDiGetDeviceInterfaceDetailA(
                    devs, &ifd, detail, needed, NULL, NULL)) {
                bi2x_misc("found %s -> %s", inst, detail->DevicePath);
                strncpy(out, detail->DevicePath, cap - 1);
                out[cap - 1] = '\0';
                found = 1;
            }

            free(detail);
        }
    }

    SetupDiDestroyDeviceInfoList(devs);

    return found ? 0 : -1;
}

static int configure(HANDLE fd)
{
    DCB dcb;
    COMMTIMEOUTS to;

    PurgeComm(
        fd, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(fd, &dcb)) {
        return -1;
    }

    /* same line settings AIO_SCI_COMM applies for the BI2X */
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = 1;
    dcb.fParity = 0;
    dcb.fOutxCtsFlow = 0;
    dcb.fOutxDsrFlow = 0;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = 0;
    dcb.fTXContinueOnXoff = 1;
    dcb.fOutX = 0;
    dcb.fInX = 0;
    dcb.fErrorChar = 0;
    dcb.fNull = 0;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.XonLim = 100;
    dcb.XoffLim = 100;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;

    if (!SetCommState(fd, &dcb)) {
        return -1;
    }

    /* ReadFile returns as soon as at least one byte is available, or after
       2 ms without data. */
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 2;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 500;

    if (!SetCommTimeouts(fd, &to)) {
        return -1;
    }

    EscapeCommFunction(fd, SETDTR);

    return 0;
}

int bi2x_serial_open(struct bi2x_serial **out, const char *port)
{
    char path[1024];
    HANDLE fd;
    struct bi2x_serial *s;

    *out = NULL;

    if (port == NULL || port[0] == '\0') {
        if (find_device_path(path, sizeof(path)) != 0) {
            return -1;
        }
    } else if (strncmp(port, "\\\\", 2) == 0) {
        strncpy(path, port, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        snprintf(path, sizeof(path), "\\\\.\\%s", port);
    }

    fd = CreateFileA(
        path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

    if (fd == INVALID_HANDLE_VALUE) {
        bi2x_misc("CreateFile(%s) failed: %lu", path, GetLastError());
        return -1;
    }

    if (configure(fd) != 0) {
        bi2x_warn("failed to configure %s: %lu", path, GetLastError());
        CloseHandle(fd);
        return -1;
    }

    s = calloc(1, sizeof(*s));

    if (s == NULL) {
        CloseHandle(fd);
        return -1;
    }

    s->fd = fd;
    *out = s;

    return 0;
}

void bi2x_serial_close(struct bi2x_serial *s)
{
    if (s == NULL) {
        return;
    }

    CloseHandle(s->fd);
    free(s);
}

static void clear_errors(HANDLE fd)
{
    DWORD errors;
    COMSTAT stat;

    ClearCommError(fd, &errors, &stat);
}

int bi2x_serial_write(struct bi2x_serial *s, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len > 0) {
        DWORD written = 0;

        if (!WriteFile(s->fd, p, (DWORD) len, &written, NULL)) {
            clear_errors(s->fd);
            return -1;
        }

        if (written == 0) {
            return -1;
        }

        p += written;
        len -= written;
    }

    return 0;
}

int bi2x_serial_read(
    struct bi2x_serial *s, void *buf, size_t cap, unsigned int timeout_ms)
{
    uint64_t deadline = bi2x_time_ms() + timeout_ms;

    for (;;) {
        DWORD got = 0;

        if (!ReadFile(s->fd, buf, (DWORD) cap, &got, NULL)) {
            clear_errors(s->fd);
            return -1;
        }

        if (got > 0) {
            return (int) got;
        }

        if (bi2x_time_ms() >= deadline) {
            return 0;
        }
    }
}

int bi2x_serial_set_break(struct bi2x_serial *s, int on)
{
    BOOL ok = on ? SetCommBreak(s->fd) : ClearCommBreak(s->fd);

    return ok ? 0 : -1;
}

void bi2x_serial_purge(struct bi2x_serial *s)
{
    PurgeComm(s->fd, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

uint64_t bi2x_time_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }

    QueryPerformanceCounter(&now);

    return (uint64_t) (now.QuadPart * 1000 / freq.QuadPart);
}

void bi2x_sleep_ms(unsigned int ms)
{
    Sleep(ms);
}

struct bi2x_mutex *bi2x_mutex_create(void)
{
    struct bi2x_mutex *m = calloc(1, sizeof(*m));

    if (m != NULL) {
        InitializeCriticalSection(&m->cs);
    }

    return m;
}

void bi2x_mutex_lock(struct bi2x_mutex *m)
{
    EnterCriticalSection(&m->cs);
}

void bi2x_mutex_unlock(struct bi2x_mutex *m)
{
    LeaveCriticalSection(&m->cs);
}

void bi2x_mutex_destroy(struct bi2x_mutex *m)
{
    if (m != NULL) {
        DeleteCriticalSection(&m->cs);
        free(m);
    }
}

static DWORD WINAPI thread_trampoline(LPVOID param)
{
    struct bi2x_thread *t = param;

    return (DWORD) t->proc(t->ctx);
}

struct bi2x_thread *bi2x_thread_start(
    const struct bi2x_thread_api *api, int (*proc)(void *), void *ctx)
{
    struct bi2x_thread *t = calloc(1, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }

    t->api = api;
    t->proc = proc;
    t->ctx = ctx;

    if (api != NULL && api->create != NULL) {
        t->ext_id = api->create(proc, ctx, 0x10000, 0);
        return t;
    }

    t->handle = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);

    if (t->handle == NULL) {
        free(t);
        return NULL;
    }

    SetThreadPriority(t->handle, THREAD_PRIORITY_ABOVE_NORMAL);

    return t;
}

void bi2x_thread_join(struct bi2x_thread *t)
{
    if (t == NULL) {
        return;
    }

    if (t->api != NULL && t->api->create != NULL) {
        int result = 0;

        if (t->api->join != NULL) {
            t->api->join(t->ext_id, &result);
        }
        if (t->api->destroy != NULL) {
            t->api->destroy(t->ext_id);
        }
    } else {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
    }

    free(t);
}

void bi2x_self_dir(char *out, size_t cap)
{
    HMODULE self = NULL;
    char path[MAX_PATH];
    char *slash;
    DWORD n;

    out[0] = '\0';

    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR) (void *) bi2x_self_dir, &self)) {
        return;
    }

    n = GetModuleFileNameA(self, path, sizeof(path));

    if (n == 0 || n >= sizeof(path)) {
        return;
    }

    slash = strrchr(path, '\\');

    if (slash == NULL) {
        return;
    }

    slash[1] = '\0';
    strncpy(out, path, cap - 1);
    out[cap - 1] = '\0';
}

#endif
