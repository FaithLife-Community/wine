/* Build with x86_64-w64-mingw32-gcc -Wall touch-probe.c -o touch-probe.exe -luser32 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <stdio.h>

static BOOL (WINAPI *get_cursor_id)(UINT32, UINT32 *);

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    POINTER_INFO info;
    UINT32 id, cursor;
    const char *event;

    switch (message)
    {
    case WM_POINTERDOWN:   event = "down"; break;
    case WM_POINTERUPDATE: event = "move"; break;
    case WM_POINTERUP:     event = "up"; break;
    case WM_LBUTTONDOWN:   puts("mouse down"); return 0;
    case WM_LBUTTONUP:     puts("mouse up"); return 0;
    case WM_DESTROY:       PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }

    id = GET_POINTERID_WPARAM(wparam);
    printf("%s id=%u message-contact=%d", event, id, !!IS_POINTER_INCONTACT_WPARAM(wparam));
    if (GetPointerInfo(id, &info))
        printf(" type=%u source=%p x=%ld y=%ld flags=%#x", (unsigned)info.pointerType,
               info.sourceDevice, info.ptPixelLocation.x, info.ptPixelLocation.y, info.pointerFlags);
    else printf(" GetPointerInfo error=%lu", GetLastError());
    if (get_cursor_id && get_cursor_id(id, &cursor)) printf(" cursor=%u", cursor);
    puts("");
    return 0;
}

int main(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    HWND window;
    MSG message;
    UINT32 count = 0;
    int status;

    setvbuf(stdout, NULL, _IONBF, 0);
    get_cursor_id = (BOOL (WINAPI *)(UINT32, UINT32 *))GetProcAddress(GetModuleHandleW(L"user32"), "GetPointerCursorId");
    if (GetPointerDevices(&count, NULL)) printf("pointer devices=%u\n", count);
    else printf("GetPointerDevices error=%lu\n", GetLastError());
    printf("GetPointerCursorId export=%s\n", get_cursor_id ? "present" : "missing");
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"OinosTouchProbe";
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 1;
    window = CreateWindowW(wc.lpszClassName, L"Touch here with two fingers, then lift each finger",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 700, 480,
                           NULL, NULL, instance, NULL);
    if (!window) return 1;
    while ((status = GetMessageW(&message, NULL, 0, 0)) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return status == -1;
}
