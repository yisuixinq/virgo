#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define sb_free(a) ((a) ? HeapFree(GetProcessHeap(), 0, stb__sbraw(a)), 0 : 0)
#define sb_push(a, v) (stb__sbmaybegrow(a, 1), (a)[stb__sbn(a)++] = (v))
#define sb_count(a) ((a) ? stb__sbn(a) : 0)

#define stb__sbraw(a) ((int *)(a)-2)
#define stb__sbm(a) stb__sbraw(a)[0]
#define stb__sbn(a) stb__sbraw(a)[1]

#define stb__sbneedgrow(a, n) ((a) == 0 || stb__sbn(a) + (n) >= stb__sbm(a))
#define stb__sbmaybegrow(a, n) (stb__sbneedgrow(a, (n)) ? stb__sbgrow(a, n) : 0)
#define stb__sbgrow(a, n) ((a) = stb__sbgrowf((a), (n), sizeof(*(a))))

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define NUM_DESKTOPS 8

typedef struct {
	HWND *windows;
	unsigned count;
} Windows;

typedef struct {
	NOTIFYICONDATA nid;
	HBITMAP hBitmap;
	HFONT hFont;
	HWND hwnd;
	HDC mdc;
	unsigned bitmapWidth;
} Trayicon;

typedef struct {
	unsigned current;
	unsigned handle_hotkeys;
	Windows desktops[NUM_DESKTOPS];
	Trayicon trayicon;
} Virgo;

static char save_path[PATH_MAX]; // 或者足够大的缓冲区


static void virgo_save_state(const Virgo* v, const char* filename);
static int virgo_load_state(Virgo* v, const char* filename);
char *get_exe_dir(char *argv0);

static void *stb__sbgrowf(void *arr, unsigned increment, unsigned itemsize)
{
	unsigned dbl_cur = arr ? 2 * stb__sbm(arr) : 0;
	unsigned min_needed = sb_count(arr) + increment;
	unsigned m = dbl_cur > min_needed ? dbl_cur : min_needed;
	unsigned *p;
	if (arr) {
		p = HeapReAlloc(GetProcessHeap(), 0, stb__sbraw(arr),
						itemsize * m + sizeof(unsigned) * 2);
	} else {
		p = HeapAlloc(GetProcessHeap(), 0, itemsize * m + sizeof(unsigned) * 2);
	}
	if (p) {
		if (!arr) {
			p[1] = 0;
		}
		p[0] = m;
		return p + 2;
	} else {
		ExitProcess(1);
		return (void *)(2 * sizeof(unsigned));
	}
}

static HICON trayicon_draw(Trayicon *t, char *text, unsigned len)
{
	ICONINFO iconInfo;
	HBITMAP hOldBitmap;
	HFONT hOldFont;
	hOldBitmap = (HBITMAP)SelectObject(t->mdc, t->hBitmap);
	hOldFont = (HFONT)SelectObject(t->mdc, t->hFont);
	TextOut(t->mdc, t->bitmapWidth / 4, 0, text, len);
	SelectObject(t->mdc, hOldBitmap);
	SelectObject(t->mdc, hOldFont);
	iconInfo.fIcon = TRUE;
	iconInfo.xHotspot = iconInfo.yHotspot = 0;
	iconInfo.hbmMask = iconInfo.hbmColor = t->hBitmap;
	return CreateIconIndirect(&iconInfo);
}

static void trayicon_init(Trayicon *t)
{
	HDC hdc;
	t->hwnd =
		CreateWindowA("STATIC", "virgo", 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
	t->bitmapWidth = GetSystemMetrics(SM_CXSMICON);
	t->nid.cbSize = sizeof(t->nid);
	t->nid.hWnd = t->hwnd;
	t->nid.uID = 100;
	t->nid.uFlags = NIF_ICON;
	hdc = GetDC(t->hwnd);
	t->hBitmap = CreateCompatibleBitmap(hdc, t->bitmapWidth, t->bitmapWidth);
	t->mdc = CreateCompatibleDC(hdc);
	ReleaseDC(t->hwnd, hdc);
	SetBkColor(t->mdc, RGB(0x00, 0x00, 0x00));
	SetTextColor(t->mdc, RGB(0x00, 0xFF, 0x00));
	t->hFont = CreateFont(-MulDiv(11, GetDeviceCaps(t->mdc, LOGPIXELSY), 72), 0,
						  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, TEXT("Arial"));
	t->nid.hIcon = trayicon_draw(t, "1", 1);
	Shell_NotifyIcon(NIM_ADD, &t->nid);
}

static void trayicon_set(Trayicon *t, unsigned number)
{
	char snumber[2];
	if (number > 9) {
		return;
	}
	snumber[0] = number + '0';
	snumber[1] = 0;
	DestroyIcon(t->nid.hIcon);
	t->nid.hIcon = trayicon_draw(t, snumber, 1);
	Shell_NotifyIcon(NIM_MODIFY, &t->nid);
}

static void trayicon_deinit(Trayicon *t)
{
	Shell_NotifyIcon(NIM_DELETE, &t->nid);
	DestroyIcon(t->nid.hIcon);
	DeleteObject(t->hBitmap);
	DeleteObject(t->hFont);
	DeleteDC(t->mdc);
	DestroyWindow(t->hwnd);
}

static void windows_mod(Windows *wins, unsigned state)
{
	unsigned i;
	for (i = 0; i < wins->count; i++) {
		ShowWindow(wins->windows[i], state);
	}
}

static void windows_show(Windows *wins) { windows_mod(wins, SW_SHOW); }

static void windows_hide(Windows *wins) { windows_mod(wins, SW_HIDE); }

static void windows_add(Windows *wins, HWND hwnd)
{
	if (wins->count >= sb_count(wins->windows)) {
		sb_push(wins->windows, hwnd);
	} else {
		wins->windows[wins->count] = hwnd;
	}
	wins->count++;
}

static void windows_del(Windows *wins, HWND hwnd)
{
	unsigned i, e;
	for (i = 0; i < wins->count; i++) {
		if (wins->windows[i] != hwnd) {
			continue;
		}
		if (i != wins->count - 1) {
			for (e = i; e < wins->count - 1; e++) {
				wins->windows[e] = wins->windows[e + 1];
			}
		}
		wins->count--;
		break;
	}
}

static unsigned is_valid_window(HWND hwnd)
{
	WINDOWINFO wi;
	wi.cbSize = sizeof(wi);
	GetWindowInfo(hwnd, &wi);
	return (wi.dwStyle & WS_VISIBLE) && !(wi.dwExStyle & WS_EX_TOOLWINDOW);
}

static void register_hotkey(unsigned id, unsigned mod, unsigned vk)
{
	// if (!RegisterHotKey(NULL, id, mod, vk)) {
	// 	MessageBox(NULL, "could not register hotkey", "error",
	// 			   MB_ICONEXCLAMATION);
	// 	ExitProcess(1);
	// }
	RegisterHotKey(NULL, id, mod, vk);  // 忽略报错
}


static BOOL enum_func(HWND hwnd, LPARAM lParam)
{
	unsigned i, e;
	Virgo *v;
	Windows *desk;
	v = (Virgo *)lParam;
	if (!is_valid_window(hwnd)) {
		return 1;
	}
	for (i = 0; i < NUM_DESKTOPS; i++) {
		desk = &(v->desktops[i]);
		for (e = 0; e < desk->count; e++) {
			if (desk->windows[e] == hwnd) {
				return 1;
			}
		}
	}
	windows_add(&(v->desktops[v->current]), hwnd);
	return 1;
}

static void virgo_update(Virgo *v)
{
	unsigned i, e;
	Windows *desk;
	HWND hwnd;
	for (i = 0; i < NUM_DESKTOPS; i++) {
		desk = &(v->desktops[i]);
		for (e = 0; e < desk->count; e++) {
			hwnd = desk->windows[e];
			if (!GetWindowThreadProcessId(desk->windows[e], NULL)) {
				windows_del(desk, hwnd);
			}
		}
	}
	desk = &v->desktops[v->current];
	for (i = 0; i < desk->count; i++) {
		hwnd = desk->windows[i];
		if (!IsWindowVisible(hwnd)) {
			windows_del(desk, hwnd);
		}
	}
	EnumWindows((WNDENUMPROC)&enum_func, (LPARAM)v);

	 // 保存当前状态
    virgo_save_state(v, save_path);
}

static void virgo_toggle_hotkeys(Virgo *v)
{
	unsigned i;
	v->handle_hotkeys = !v->handle_hotkeys;
	if (v->handle_hotkeys) {
		for (i = 0; i < NUM_DESKTOPS; i++) {
			register_hotkey(i * 2, MOD_ALT | MOD_NOREPEAT, i + 1 + '0');
			// register_hotkey(i * 2 + 1, MOD_CONTROL | MOD_NOREPEAT, i + 1 + '0');
			register_hotkey(i * 2 + 1,  MOD_CONTROL | MOD_ALT |MOD_NOREPEAT, i + 1 + '0');
		}
	} else {
		for (i = 0; i < NUM_DESKTOPS; i++) {
			UnregisterHotKey(NULL, i * 2);
			UnregisterHotKey(NULL, i * 2 + 1);
		}
	}
}

static void virgo_init(Virgo *v)
{
	unsigned i;
	v->handle_hotkeys = 1;
	for (i = 0; i < NUM_DESKTOPS; i++) {
		register_hotkey(i * 2, MOD_ALT | MOD_NOREPEAT, i + 1 + '0');
		register_hotkey(i * 2 + 1,  MOD_CONTROL | MOD_ALT |MOD_NOREPEAT, i + 1 + '0');
	}
	register_hotkey(i * 2, MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
					'Q');
	register_hotkey(i * 2 + 1, MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
					'S');
	trayicon_init(&v->trayicon);

	 // 加载初始状态
    virgo_load_state(v, save_path);
}

static void virgo_deinit(Virgo *v)
{
	unsigned i;
	for (i = 0; i < NUM_DESKTOPS; i++) {
		windows_show(&v->desktops[i]);
		sb_free(v->desktops[i].windows);
	}
	trayicon_deinit(&v->trayicon);

	// 保存最终状态
    // virgo_save_state(v, save_path);
}

static void virgo_move_to_desk(Virgo *v, unsigned desk)
{
	HWND hwnd;
	if (v->current == desk) {
		return;
	}
	virgo_update(v);
	hwnd = GetForegroundWindow();
	if (!hwnd || !is_valid_window(hwnd)) {
		return;
	}
	windows_del(&v->desktops[v->current], hwnd);
	windows_add(&v->desktops[desk], hwnd);
	ShowWindow(hwnd, SW_HIDE);

	// 保存最终状态
    virgo_save_state(v, save_path);
}

static void virgo_go_to_desk(Virgo *v, unsigned desk)
{
	if (v->current == desk) {
		return;
	}
	virgo_update(v);
	windows_hide(&v->desktops[v->current]);
	windows_show(&v->desktops[desk]);
	v->current = desk;
	trayicon_set(&v->trayicon, v->current + 1);
}

void __main(void) __asm__("__main");
void main(int argc, char *argv[])
{
	Virgo v = {0};
	MSG msg;

	char *exe_dir = get_exe_dir(argv[0]);
    snprintf(save_path, PATH_MAX, "%s%s", exe_dir, "virgo_state.bin");

    virgo_init(&v);

	while (GetMessage(&msg, NULL, 0, 0)) {
		if (msg.message != WM_HOTKEY) {
			continue;
		}
		if (msg.wParam == NUM_DESKTOPS * 2) {
			break;
		}
		if (msg.wParam == NUM_DESKTOPS * 2 + 1) {
			virgo_toggle_hotkeys(&v);
		} else if (msg.wParam % 2 == 0) {
			virgo_go_to_desk(&v, msg.wParam / 2);
		} else {
			virgo_move_to_desk(&v, (msg.wParam - 1) / 2);
		}
	}
	virgo_deinit(&v);
	ExitProcess(0);
}



// ------------------------------------------------------------------

// 保存Virgo状态到文件
static void virgo_save_state(const Virgo* v, const char* filename)
{
    FILE* file = fopen(filename, "wb"); // 使用"wb"模式以二进制方式打开文件

    if (file == NULL)
    {
        MessageBox(NULL, "Failed to open state file for saving", "Error", MB_ICONERROR);
        return;
    }

    // 写入当前桌面索引
    if (fwrite(&v->current, sizeof(v->current), 1, file) != 1)
    {
        // 出错处理（例如关闭文件并显示错误消息）
        fclose(file);
        MessageBox(NULL, "Failed to write current desktop index to state file", "Error", MB_ICONERROR);
        return;
    }

    // 写入各虚拟桌面的窗口列表
    for (unsigned i = 0; i < NUM_DESKTOPS; ++i)
    {
        const Windows* desk = &v->desktops[i];

        // 写入窗口数量
        if (fwrite(&desk->count, sizeof(desk->count), 1, file) != 1)
        {
            // 出错处理（例如关闭文件并显示错误消息）
            fclose(file);
            MessageBox(NULL, "Failed to write window count to state file", "Error", MB_ICONERROR);
            return;
        }

        // 写入每个窗口句柄
        for (unsigned j = 0; j < desk->count; ++j)
        {
            if (fwrite(&desk->windows[j], sizeof(desk->windows[j]), 1, file) != 1)
            {
                // 出错处理（例如关闭文件并显示错误消息）
                fclose(file);
                MessageBox(NULL, "Failed to write window handle to state file", "Error", MB_ICONERROR);
                return;
            }
        }
    }

    // 关闭文件
    if (fclose(file) != 0)
    {
        // 出错处理（例如显示错误消息）
        MessageBox(NULL, "Failed to close state file", "Error", MB_ICONERROR);
        return;
    }
}

// 加载Virgo状态从文件
static int virgo_load_state(Virgo* v, const char* filename)
{
    FILE* file = fopen(filename, "rb"); // 使用"rb"模式以二进制方式打开文件

    if (file == NULL)
    {
        // 文件不存在或无法打开，返回false表示未成功加载状态
        return 0;
    }

    // 读取当前桌面索引
    if (fread(&v->current, sizeof(v->current), 1, file) != 1)
    {
        // 出错处理（例如关闭文件并返回false）
        fclose(file);
        return 0;
    }

    // 读取各虚拟桌面的窗口列表
    for (unsigned i = 0; i < NUM_DESKTOPS; ++i)
    {
        Windows* desk = &v->desktops[i];

        // 读取窗口数量
        if (fread(&desk->count, sizeof(desk->count), 1, file) != 1)
        {
            // 出错处理（例如关闭文件并返回false）
            fclose(file);
            return 0;
        }

        // 重新分配窗口句柄数组
        sb_free(desk->windows);
        sb_push(desk->windows, 0); // Reserve space for count

        // 读取每个窗口句柄
        for (unsigned j = 0; j < desk->count; ++j)
        {
            if (fread(&desk->windows[j], sizeof(desk->windows[j]), 1, file) != 1)
            {
                // 出错处理（例如关闭文件并返回false）
                fclose(file);
                return 0;
            }
        }
    }


	fclose(fopen(filename, "w"));

    return 1;
}

// 获取可执行文件所在目录
char *get_exe_dir(char *argv0) {
    char *last_slash = strrchr(argv0, '/');
    if (last_slash == NULL) {
        last_slash = strrchr(argv0, '\\'); // Windows环境下尝试查找反斜杠
    }
    if (last_slash != NULL) {
        size_t dir_len = last_slash - argv0 + 1; // 包含分隔符
        char *dir = malloc(dir_len + 1);
        memcpy(dir, argv0, dir_len);
        dir[dir_len] = '\0';
        return dir;
    } else {
        return "."; // 当前工作目录，如果无法从argv0获取路径
    }
}
