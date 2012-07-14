/* Copyright (c) 2012 Martin Ridgers
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "clink_pch.h"
#include "clink.h"

//------------------------------------------------------------------------------
void                    get_config_dir(char*, int);
void                    get_dll_dir(char*, int);
void                    str_cat(char*, const char*, int);
void                    save_history();
void                    shutdown_lua();
void                    log_line(const char*, ...);
static const wchar_t*   g_last_write_buffer = NULL;

//------------------------------------------------------------------------------
LONG WINAPI exception_filter(EXCEPTION_POINTERS* info)
{
#if defined(_MSC_VER)
    MINIDUMP_EXCEPTION_INFORMATION mdei = { GetCurrentThreadId(), info, FALSE };
    DWORD pid;
    HANDLE process;
    HANDLE file;
    char file_name[1024];

    get_config_dir(file_name, sizeof(file_name));
    str_cat(file_name, "/mini_dump.dmp", sizeof(file_name));

    fputs("\n!!! CLINK'S CRASHED!", stderr);
    fputs("\n!!! Something went wrong.", stderr);
    fputs("\n!!! Writing mini dump file to: ", stderr);
    fputs(file_name, stderr);
    fputs("\n", stderr);

    file = CreateFile(file_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        pid = GetCurrentProcessId();
        process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (process != NULL)
        {
            MiniDumpWriteDump(process, pid, file, MiniDumpNormal, &mdei, NULL, NULL);
        }
        CloseHandle(process);
    }
    CloseHandle(file);
#endif // _MSC_VER

    // Would be awesome if we could unhook ourself, unload, and allow cmd.exe
    // to continue!

    return EXCEPTION_EXECUTE_HANDLER;
}

//------------------------------------------------------------------------------
void emulate_doskey(wchar_t* buffer, DWORD max_size)
{
    // ReadConsoleW() will postprocess what the user enters, resolving any
    // aliases that may be registered (aka doskey macros). As we've skipped this
    // step, we have to resolve aliases ourselves.

    wchar_t exe_buf[MAX_PATH];
    wchar_t* exe;
    wchar_t* slash;

    GetModuleFileNameW(NULL, exe_buf, sizeof(exe_buf));
    slash = wcsrchr(exe_buf, L'\\');
    exe = (slash != NULL) ? (slash + 1) : exe_buf;

    GetConsoleAliasW(buffer, buffer, max_size, exe);
}

//------------------------------------------------------------------------------
void append_crlf(wchar_t* buffer, DWORD max_size)
{
    // Cmd.exe expects a CRLF combo at the end of the string, otherwise it
    // thinks the line is part of a multi-line command.

    size_t len;

    len = max_size - wcslen(buffer);
    wcsncat(buffer, L"\x0d\x0a", len);
    buffer[max_size - 1] = L'\0';
}

//------------------------------------------------------------------------------
static BOOL WINAPI hooked_read_console(
    HANDLE input,
    wchar_t* buffer,
    DWORD buffer_size,
    LPDWORD read_in,
    PCONSOLE_READCONSOLE_CONTROL control
)
{
    LPTOP_LEVEL_EXCEPTION_FILTER old_seh;

    // If cmd.exe is asking for one character at a time, use the original path
    // It does this to handle y/n/all prompts which isn't an compatible use-
    // case for readline. This saves hacking around it...
    if (buffer_size == 1)
    {
        return ReadConsoleW(input, buffer, buffer_size, read_in, control);
    }

    old_seh = SetUnhandledExceptionFilter(exception_filter);
    call_readline(g_last_write_buffer, buffer, buffer_size);
    g_last_write_buffer = L"";
    SetUnhandledExceptionFilter(old_seh);

    // Check for control codes and convert them.
    if (buffer[0] == L'\x03')
    {
        // Fire a Ctrl-C exception. Cmd.exe sets a global variable (CtrlCSeen)
        // and ReadConsole() would normally set error code 0x3e3. Sleep() is to
        // yield the thread so the global gets set (guess work...).
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        SetLastError(0x3e3);
        Sleep(0);

        buffer[0] = '\0';
    }
    else
    {
        emulate_doskey(buffer, buffer_size);
        append_crlf(buffer, buffer_size);
    }

    *read_in = (unsigned)wcslen(buffer);
    return TRUE;
}

//------------------------------------------------------------------------------
static BOOL WINAPI hooked_write_console(
    HANDLE output,
    const wchar_t* buffer,
    DWORD buffer_size,
    LPDWORD written,
    void* unused
)
{
    g_last_write_buffer = buffer;
    return WriteConsoleW(output, buffer, buffer_size, written, unused);
}

//------------------------------------------------------------------------------
static void* rva_to_addr(void* base_addr, unsigned rva)
{
    return (char*)rva + (uintptr_t)base_addr;
}

//------------------------------------------------------------------------------
static DWORD make_page_writable(void* addr, unsigned size)
{
    DWORD old_state;
    VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old_state);
    return old_state;
}

//------------------------------------------------------------------------------
static void restore_page_state(void* addr, unsigned size, unsigned state)
{
    VirtualProtect(addr, size, state, NULL);
}

//------------------------------------------------------------------------------
static void prepare_env_for_inputrc(HINSTANCE instance)
{
    static const char inputrc_eq[] = "INPUTRC=";
    char* slash;
    char buffer[1024];

    // Unless set to something else, set the environment variable "HOME"
    // to the user's profile folder.
    if (getenv("HOME") == NULL)
    {
        const char* user_profile = getenv("APPDATA");
        if (user_profile != NULL)
        {
            strcpy(buffer, "HOME=");
            str_cat(buffer, user_profile, sizeof(buffer));
        }

        putenv(buffer);
    }

    // Set INPUTRC variable to be a .inputrc file that's alongside this DLL.
    strcpy(buffer, inputrc_eq);
    get_dll_dir(
        buffer + sizeof(inputrc_eq) - 1,
        sizeof(buffer) - sizeof(inputrc_eq)
    );

    str_cat(buffer, "/clink_inputrc", sizeof(buffer));

    putenv(buffer);
}

//------------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID unused)
{
    void* base_addr;
    HANDLE kernel32 = LoadLibraryA("kernel32.dll");

    struct hook_t
    {
        void* to_hook;
        void* the_hook;
    };

    struct hook_t kernel32_hooks[] =
    {
        {
            GetProcAddress(kernel32, "ReadConsoleW"),
            hooked_read_console
        },
        {
            GetProcAddress(kernel32, "WriteConsoleW"),
            hooked_write_console
        }
    };

    // We're only interested in when the dll is attached.
    if (reason != DLL_PROCESS_ATTACH)
    {
        if (reason == DLL_PROCESS_DETACH)
        {
            save_history();
            shutdown_lua();
        }

        return TRUE;
    }

    // Give readline a chance to find the inputrc by modifying the
    // environment slightly.
    prepare_env_for_inputrc(instance);
     
    base_addr = (void*)GetModuleHandle("cmd.exe");
    if (base_addr)
    {
        IMAGE_DOS_HEADER* dos_header;
        IMAGE_NT_HEADERS* nt_header;
        IMAGE_DATA_DIRECTORY* data_dir;
        unsigned i, j;
        IMAGE_IMPORT_DESCRIPTOR* iid;

        dos_header = (IMAGE_DOS_HEADER*)base_addr;
        nt_header = (IMAGE_NT_HEADERS*)((char*)base_addr + dos_header->e_lfanew);
        data_dir = nt_header->OptionalHeader.DataDirectory + 1;
        if (data_dir == NULL)
        {
            log_line("Failed to find import table.");
            return TRUE;
        }

        iid = (IMAGE_IMPORT_DESCRIPTOR*)(rva_to_addr(base_addr, data_dir->VirtualAddress));
        for (i = 0; i < data_dir->Size / sizeof(*iid); ++i)
        {
            IMAGE_THUNK_DATA* itd;
            char* name = (char*)rva_to_addr(base_addr, iid->Name);
            if (stricmp(name, "kernel32.dll") != 0)
            {
                ++iid;
                continue;
            }

            for (j = 0; j < sizeof_array(kernel32_hooks); ++j)
            {
                itd = rva_to_addr(base_addr, iid->FirstThunk);
                while (itd->u1.Function > 0)
                {
                    BOOL ok;
                    DWORD page_state;
                    void* addr = &itd->u1.Function;

                    if (itd->u1.Function != (uintptr_t)kernel32_hooks[j].to_hook)
                    {
                        ++itd;
                        continue;
                    }

                    log_line("Hooking: %p", itd);

                    page_state = make_page_writable(addr, sizeof(uintptr_t));
                    ok = WriteProcessMemory(
                        GetCurrentProcess(),
                        addr,
                        &kernel32_hooks[j].the_hook,
                        sizeof(uintptr_t),
                        NULL
                    );
                    log_line("  GetLastError = %d", GetLastError());
                    log_line("  ...%s", (ok != FALSE) ? "ok" : "failed - PANIC!");

                    restore_page_state(addr, sizeof(uintptr_t), page_state);
                    break;
                }
            }

            break;
        }
    }
    else
    {
        log_line("Failed to find base address for 'cmd.exe'.");
    }

    return TRUE;
}
