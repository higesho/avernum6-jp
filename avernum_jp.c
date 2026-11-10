// Avernum 6 - Japanese text support
// (ported from the Avernum: Escape From the Pit / Avernum 2-5 JP mods)
//
// Built as a proxy dsound.dll (DirectSoundCreate is passed through to the system dsound.dll).
// Avernum 6 draws text with its own bitmap fonts through OpenGL texture objects. The DLL
//   * replaces the routine that lays out and draws text, so that EUC-JP characters are measured
//     with GDI and drawn from glyph textures of its own (ASCII stays with the game's fonts);
//   * fixes the script loader's token estimate, which Japanese literals would overflow;
//   * redirects exe strings (.rdata) and the data files (scripts, scenario, pictures)
//     to the translated versions, as in the Avernum 3 mod.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <GL/gl.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "opengl32.lib")

static BYTE *g_base;
#define VA(x) ((void *)(g_base + ((DWORD)(x) - 0x400000)))
#define GAME_CP 20932 /* EUC-JP */
static int g_eucjp = 1; // used by to_game_encoding(): game text is EUC-JP

// ---------------------------------------------------------------- logging
static FILE *g_log;
static void jp_log(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------- exe string replacement
//
// avernum_jp_exe.tsv (UTF-8, tab separated): va, sink, refs, english, japanese.
// For every row with a translation we check that the exe really has the English
// string at that address, then rewrite every relocated pointer to it (code
// immediates and data tables, found via the exe's base relocation table) so it
// points at the translated string instead. The exe file itself is untouched.

static const BYTE kSprintfPrologue[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};

typedef struct
{
    DWORD oldPtr;
    DWORD newPtr;
} StrMap;
#define STRMAP_SIZE 4096
static StrMap g_strmap[STRMAP_SIZE];
static int g_strmapCount;

static void strmap_put(DWORD oldPtr, DWORD newPtr)
{
    DWORD h = (oldPtr * 2654435761u) & (STRMAP_SIZE - 1);
    while (g_strmap[h].oldPtr && g_strmap[h].oldPtr != oldPtr)
        h = (h + 1) & (STRMAP_SIZE - 1);
    if (!g_strmap[h].oldPtr)
        g_strmapCount++;
    g_strmap[h].oldPtr = oldPtr;
    g_strmap[h].newPtr = newPtr;
}
static DWORD strmap_get(DWORD oldPtr)
{
    DWORD h = (oldPtr * 2654435761u) & (STRMAP_SIZE - 1);
    while (g_strmap[h].oldPtr)
    {
        if (g_strmap[h].oldPtr == oldPtr)
            return g_strmap[h].newPtr;
        h = (h + 1) & (STRMAP_SIZE - 1);
    }
    return 0;
}

// In-place unescape of \n \r \t \\ (as written by the exporter).
static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++)
    {
        if (*s == '\\' && s[1])
        {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == 't' ? '\t' : *s;
        }
        else
            *o++ = *s;
    }
    *o = 0;
}

// UTF-8 -> game encoding (EUC-JP or UTF-8). Returns malloc'd string.
static char *to_game_encoding(const char *utf8, int *unmappable)
{
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), i, n;
    WCHAR *w;
    char *out;
    BOOL usedDefault = FALSE;
    *unmappable = 0;
    if (!g_eucjp)
        return _strdup(utf8);
    w = (WCHAR *)malloc(wn * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
    for (i = 0; i < wn; i++) // same normalisation as tools/jpconv.py
        switch (w[i])
        {
        case 0xFF5E: w[i] = 0x301C; break;
        case 0xFF0D: w[i] = 0x2212; break;
        case 0x2225: w[i] = 0x2016; break;
        }
    n = WideCharToMultiByte(20932, 0, w, wn, NULL, 0, NULL, NULL);
    out = (char *)malloc(n + 1);
    WideCharToMultiByte(20932, 0, w, wn, out, n, "?", &usedDefault);
    out[n] = 0;
    *unmappable = usedDefault;
    free(w);
    return out;
}

static int g_needPositional;
static int g_forceScan; // [debug] force_scan=1: search everything as for another build (tests the search)

static int __cdecl my_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    if (strchr(fmt, '$'))
        r = _vsprintf_p(buf, 65536, fmt, ap); // supports %1$s style reordering
    else
        r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

static void install_sprintf_hook(void)
{
    jp_log("positional formats (%%1$s) are not supported in Avernum 4 exe strings\n");
}

static int is_positional(const char *s)
{
    for (; *s; s++)
        if (s[0] == '%' && s[1] >= '1' && s[1] <= '9' && s[2] == '$')
            return 1;
    return 0;
}

// Other builds keep the same strings at other addresses. A string is identified by its
// text and by how many relocated pointers refer to it (the "refs" column, counted in
// the Steam build), so short texts that also occur as plain data are not mistaken.
static DWORD *g_relocTargets; // values of all relocated pointers, sorted
static int g_relocCount;

static int cmp_dword(const void *a, const void *b)
{
    DWORD x = *(const DWORD *)a, y = *(const DWORD *)b;
    return x < y ? -1 : x > y;
}

static void build_reloc_targets(void)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    BYTE *rel = g_base + rd->VirtualAddress, *end = rel + rd->Size;
    int cap = rd->Size / 2 + 1;
    g_relocTargets = (DWORD *)malloc(cap * sizeof(DWORD));
    g_relocCount = 0;
    while (g_relocTargets && rd->VirtualAddress && rel < end)
    {
        IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)rel;
        WORD *e = (WORD *)(blk + 1);
        int n = (blk->SizeOfBlock - sizeof *blk) / 2, i;
        if (blk->SizeOfBlock < sizeof *blk)
            break;
        for (i = 0; i < n && g_relocCount < cap; i++)
            if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                g_relocTargets[g_relocCount++] = *(DWORD *)(g_base + blk->VirtualAddress + (e[i] & 0xfff));
        rel += blk->SizeOfBlock;
    }
    if (g_relocTargets)
        qsort(g_relocTargets, g_relocCount, sizeof(DWORD), cmp_dword);
}

static int reloc_refs(DWORD ptr)
{
    int lo = 0, hi = g_relocCount, n = 0;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (g_relocTargets[mid] < ptr)
            lo = mid + 1;
        else
            hi = mid;
    }
    while (lo + n < g_relocCount && g_relocTargets[lo + n] == ptr)
        n++;
    return n;
}

// True when some relocated pointer points inside [ptr+1, ptr+len]: the compiler inlined a copy of
// this short string as 4-byte loads (mov eax,[s]; mov eax,[s+4] ...). Redirecting only the pointer
// to the first byte would mix the first 4 bytes of the translation with the rest of the English
// ("筋力ngth"), so such strings are overwritten in place instead.
static int reloc_interior(DWORD ptr, size_t len)
{
    int lo = 0, hi = g_relocCount;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (g_relocTargets[mid] <= ptr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g_relocCount && g_relocTargets[lo] <= ptr + len;
}

// Returns the RVA of a whole NUL-terminated occurrence of s in the data sections with
// exactly `refs` references (refs < 0: any number above zero) that is not in used[].
static DWORD find_string_rva(const char *s, int refs, const DWORD *used, int nUsed)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(s);
    int i, j, r;
    if (!len)
        return 0;
    if (!g_relocTargets)
        build_reloc_targets();
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        BYTE *a, *p, *end;
        if ((sec[i].Characteristics & IMAGE_SCN_CNT_CODE) || !(sec[i].Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA))
            continue;
        a = g_base + sec[i].VirtualAddress;
        end = a + min(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        for (p = a; p + len < end; p++)
            if (*p == (BYTE)s[0] && (p == a || p[-1] == 0) && p[len] == 0 && memcmp(p, s, len) == 0)
            {
                DWORD rva = (DWORD)(p - g_base);
                r = reloc_refs((DWORD)p);
                for (j = 0; j < nUsed && used[j] != rva; j++)
                    ;
                if (j == nUsed && (refs < 0 ? r > 0 : r == refs))
                    return rva;
                p += len;
            }
    }
    return 0;
}

static void load_exe_strings(const char *dir)
{
    DWORD *used = (DWORD *)calloc(4096, sizeof(DWORD)); // RVAs of strings already taken
    int nUsed = 0, foundBySearch = 0;
    char path[MAX_PATH];
    FILE *f;
    long size;
    char *buf, *line, *next;
    int lineno = 0, ok = 0, skipped = 0, inPlace = 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;

    _snprintf(path, sizeof path, "%s\\avernum_jp_exe.tsv", dir);
    f = fopen(path, "rb");
    if (!f)
    {
        jp_log("no %s (exe strings stay English)\n", path);
        free(used);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = 0;
    fclose(f);
    if (!g_relocTargets)
        build_reloc_targets();

    for (line = buf; line && *line; line = next)
    {
        char *col[5] = {0};
        char *p;
        int c = 0, unmappable;
        DWORD va, rva;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        lineno++;
        if ((p = strchr(line, '\r')) != NULL)
            *p = 0;
        if (lineno == 1 && (strncmp(line, "va\t", 3) == 0 || strncmp(line, "\xEF\xBB\xBFva\t", 6) == 0))
            continue;
        col[0] = line;
        for (p = line; *p && c < 4; p++)
            if (*p == '\t')
            {
                *p = 0;
                col[++c] = p + 1;
            }
        if (c < 4 || !col[4][0])
            continue; // untranslated
        unescape(col[3]);
        unescape(col[4]);
        va = strtoul(col[0], NULL, 16);
        rva = va - 0x400000;
        if (g_forceScan || va < 0x400000 || rva >= imageSize || strcmp((char *)g_base + rva, col[3]) != 0)
        {
            // not at the Steam build's address: look for the same text elsewhere
            char *e;
            long refs = strtol(col[2], &e, 10);
            rva = find_string_rva(col[3], (e != col[2] && refs > 0) ? (int)refs : -1, used, nUsed);
            if (!rva)
            {
                jp_log("exe_tsv line %d: English text not found in this exe (%s), skipped\n", lineno, col[0]);
                skipped++;
                continue;
            }
            if (rva != va - 0x400000)
                foundBySearch++;
        }
        if (nUsed < 4096)
            used[nUsed++] = rva;
        {
            char *jp = to_game_encoding(col[4], &unmappable);
            if (unmappable)
                jp_log("exe_tsv line %d: characters not in EUC-JP were replaced with '?'\n", lineno);
            if (strstr(col[1], "float") && strlen(jp) > 39)
                jp_log("exe_tsv line %d: floating text is %u bytes (max 39), it will not be shown\n", lineno, (unsigned)strlen(jp));
            if (g_relocTargets && reloc_interior((DWORD)(g_base + rva), strlen((char *)g_base + rva)))
            {
                BYTE *s = g_base + rva;
                size_t elen = strlen((char *)s), jl = strlen(jp);
                if (jl <= elen)
                {
                    DWORD old;
                    VirtualProtect(s, elen + 1, PAGE_READWRITE, &old);
                    memcpy(s, jp, jl + 1);
                    VirtualProtect(s, elen + 1, old, &old);
                    inPlace++;
                    ok++;
                }
                else
                {
                    jp_log("exe_tsv line %d: %s is copied 4 bytes at a time and the translation is longer than the English (%u > %u bytes), left English\n",
                           lineno, col[0], (unsigned)jl, (unsigned)elen);
                    skipped++;
                }
                continue;
            }
            if (is_positional(jp))
                g_needPositional = 1;
            strmap_put((DWORD)(g_base + rva), (DWORD)jp);
            ok++;
        }
    }
    free(buf);
    free(used);
    free(g_relocTargets);
    g_relocTargets = NULL;
    if (foundBySearch)
        jp_log("exe strings: %d found at other addresses than in the Steam build\n", foundBySearch);

    // Walk the exe's base relocations and redirect every pointer to a translated string.
    {
        IMAGE_DATA_DIRECTORY *rd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        BYTE *rel = g_base + rd->VirtualAddress, *end = rel + rd->Size;
        int patched = 0;
        while (rd->VirtualAddress && rel < end)
        {
            IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)rel;
            WORD *e = (WORD *)(blk + 1);
            int n = (blk->SizeOfBlock - sizeof *blk) / 2, i;
            if (blk->SizeOfBlock < sizeof *blk)
                break;
            for (i = 0; i < n; i++)
            {
                if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
                {
                    DWORD *site = (DWORD *)(g_base + blk->VirtualAddress + (e[i] & 0xfff));
                    DWORD np = strmap_get(*site);
                    if (np)
                    {
                        DWORD old;
                        VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old);
                        *site = np;
                        VirtualProtect(site, 4, old, &old);
                        patched++;
                    }
                }
            }
            rel += blk->SizeOfBlock;
        }
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        jp_log("exe strings: %d translated (%d written in place), %d skipped, %d references patched\n", ok, inPlace, skipped, patched);
    }
    if (g_needPositional)
        install_sprintf_hook();
}

// ---------------------------------------------------------------- dsound passthrough
typedef HRESULT(WINAPI *DSCreateFn)(const GUID *, void **, void *);
static HMODULE g_realDsound;
static FARPROC real_dsound(const char *name)
{
    if (!g_realDsound)
    {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dsound.dll");
        g_realDsound = LoadLibraryA(path);
    }
    return g_realDsound ? GetProcAddress(g_realDsound, name) : NULL;
}
HRESULT WINAPI DirectSoundCreate(const GUID *dev, void **out, void *outer)
{
    DSCreateFn fn = (DSCreateFn)real_dsound("DirectSoundCreate");
    return fn ? fn(dev, out, outer) : E_FAIL;
}

// ---------------------------------------------------------------- translated data files (drop-in mode)
// The game's own files are never changed. avernum_jp\jp_patch.bin holds deltas against the
// original files; on start-up they are applied into avernum_jp\cache\ (only when the
// original's SHA-256 matches), and the exe's CreateFileA/W and FindFirstFileExA are
// redirected there. Removing dsound.dll restores the English game.
typedef struct
{
    DWORD h[8];
    BYTE buf[64];
    unsigned __int64 len;
    int n;
} Sha256;

static const DWORD kShaK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const BYTE *p)
{
    DWORD w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (DWORD)p[i * 4] << 24 | (DWORD)p[i * 4 + 1] << 16 | (DWORD)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++)
        w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] +
               (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (i = 0; i < 64; i++)
    {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + kShaK[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha256(const BYTE *data, DWORD size, BYTE out[32])
{
    static const DWORD init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Sha256 s;
    DWORD i;
    unsigned __int64 bits = (unsigned __int64)size * 8;
    memcpy(s.h, init, sizeof init);
    for (i = 0; i + 64 <= size; i += 64)
        sha_block(&s, data + i);
    s.n = size - i;
    memcpy(s.buf, data + i, s.n);
    s.buf[s.n++] = 0x80;
    if (s.n > 56)
    {
        memset(s.buf + s.n, 0, 64 - s.n);
        sha_block(&s, s.buf);
        s.n = 0;
    }
    memset(s.buf + s.n, 0, 56 - s.n);
    for (i = 0; i < 8; i++)
        s.buf[56 + i] = (BYTE)(bits >> (56 - 8 * i));
    sha_block(&s, s.buf);
    for (i = 0; i < 8; i++)
    {
        out[i * 4] = (BYTE)(s.h[i] >> 24);
        out[i * 4 + 1] = (BYTE)(s.h[i] >> 16);
        out[i * 4 + 2] = (BYTE)(s.h[i] >> 8);
        out[i * 4 + 3] = (BYTE)s.h[i];
    }
}

static BYTE *read_all(const char *path, DWORD *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    BYTE *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (BYTE *)malloc(n > 0 ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (DWORD)n;
    return buf;
}

static void make_parent_dirs(const char *path)
{
    char tmp[MAX_PATH];
    char *p;
    lstrcpynA(tmp, path, MAX_PATH);
    for (p = tmp + 3; *p; p++)
        if (*p == '\\')
        {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
}

#define MAX_REDIRECTS 1024
typedef struct
{
    char rel[MAX_PATH];   // lower case, '\' separated, relative to the game folder
    char cache[MAX_PATH]; // translated copy
} Redirect;
static Redirect *g_redir;
static int g_redirCount;
static char g_gameDirLower[MAX_PATH];
static int g_gameDirLen;

static void lower_path(char *s)
{
    for (; *s; s++)
        *s = *s == '/' ? '\\' : (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
}

static const char *redirect_for(const char *path)
{
    char full[MAX_PATH * 2];
    DWORD n;
    int i;
    if (!g_redirCount || !path)
        return NULL;
    n = GetFullPathNameA(path, sizeof full, full, NULL);
    if (!n || n >= sizeof full)
        return NULL;
    lower_path(full);
    if (strncmp(full, g_gameDirLower, g_gameDirLen) != 0 || full[g_gameDirLen] != '\\')
        return NULL;
    for (i = 0; i < g_redirCount; i++)
        if (strcmp(full + g_gameDirLen + 1, g_redir[i].rel) == 0)
            return g_redir[i].cache;
    return NULL;
}

typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *FindFirstFileExA_t)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
static CreateFileA_t g_realCreateFileA;
static CreateFileW_t g_realCreateFileW;
static FindFirstFileExA_t g_realFindFirstFileExA;
static int g_redirLogged;

static void log_redirect(const char *from, const char *to)
{
    if (g_redirLogged < 5) // the first few, to confirm the redirect works
    {
        g_redirLogged++;
        jp_log("redirect %s -> %s\n", from, to);
    }
}

static HANDLE WINAPI my_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    const char *to = (access & (GENERIC_WRITE | GENERIC_ALL)) ? NULL : redirect_for(name);
    if (to)
    {
        log_redirect(name, to);
        name = to;
    }
    return g_realCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI my_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    char a[MAX_PATH * 2];
    WCHAR w[MAX_PATH];
    const char *to = NULL;
    if (name && !(access & (GENERIC_WRITE | GENERIC_ALL)) && g_redirCount &&
        WideCharToMultiByte(CP_ACP, 0, name, -1, a, sizeof a, NULL, NULL))
        to = redirect_for(a);
    if (to && MultiByteToWideChar(CP_ACP, 0, to, -1, w, MAX_PATH))
    {
        log_redirect(a, to);
        name = w;
    }
    return g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI my_FindFirstFileExA(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data,
                                         FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
    const char *to = redirect_for(name); // stat() of a translated file reports the translated size
    return g_realFindFirstFileExA(to ? to : name, level, data, op, filter, flags);
}

// Replace one entry of the exe's import address table; returns the original function.
static void *patch_iat(const char *dll, const char *func, void *hook)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress);
    for (; dir.VirtualAddress && d->Name; d++)
    {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((const char *)(g_base + d->Name), dll) != 0)
            continue;
        names = (IMAGE_THUNK_DATA *)(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            void *orig;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0)
                continue;
            orig = (void *)addrs->u1.Function;
            VirtualProtect(&addrs->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old);
            addrs->u1.Function = (DWORD)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(DWORD), old, &old);
            return orig;
        }
    }
    return NULL;
}

static DWORD rd32(const BYTE *p) { return *(const DWORD *)p; }

// Apply one patch entry; returns a malloc'ed buffer of dst_len bytes or NULL.
static BYTE *apply_entry(const BYTE *ops, DWORD count, const BYTE *src, DWORD srcLen, DWORD dstLen)
{
    BYTE *o = (BYTE *)malloc(dstLen ? dstLen : 1);
    DWORD k, w = 0;
    for (k = 0; o && k < count; k++)
    {
        BYTE t = *ops++;
        DWORD off, len, i;
        if (t == 2)
        {
            len = rd32(ops), ops += 4;
            if (w + len > dstLen)
                goto bad;
            memcpy(o + w, ops, len);
            ops += len;
        }
        else
        {
            off = rd32(ops), len = rd32(ops + 4), ops += 8;
            if (w + len > dstLen || !srcLen)
                goto bad;
            if (t == 1)
            {
                if (off + len > srcLen)
                    goto bad;
                memcpy(o + w, src + off, len);
            }
            else
            {
                for (i = 0; i < len; i++)
                    o[w + i] = (BYTE)(ops[i] + src[(off + i) % srcLen]);
                ops += len;
            }
        }
        w += len;
    }
    if (o && w == dstLen)
        return o;
bad:
    free(o);
    return NULL;
}

// Skip over the ops of one entry (to reach the next entry).
static const BYTE *skip_ops(const BYTE *p, DWORD count)
{
    DWORD k;
    for (k = 0; k < count; k++)
    {
        BYTE t = *p++;
        p += t == 1 ? 8 : t == 2 ? 4 + rd32(p) : 8 + rd32(p + 4);
    }
    return p;
}

static void prepare_redirects(const char *gameDir, const char *dataDir)
{
    char path[MAX_PATH], stampPath[MAX_PATH], line[MAX_PATH + 64];
    BYTE *patch, h[32];
    const BYTE *p, *end;
    DWORD size, n, i;
    WIN32_FILE_ATTRIBUTE_DATA pa;
    char *stamps = NULL; // old stamp file contents, searched by line
    DWORD stampsLen = 0;
    FILE *out;
    int made = 0, reused = 0, already = 0, mismatch = 0;

    _snprintf(path, sizeof path, "%s\\jp_patch.bin", dataDir);
    patch = read_all(path, &size);
    if (!patch)
    {
        jp_log("no %s (data files are used as they are)\n", path);
        return;
    }
    if (size < 12 || memcmp(patch, "AJPATCH1", 8) != 0 || !GetFileAttributesExA(path, GetFileExInfoStandard, &pa))
    {
        jp_log("%s is broken\n", path);
        free(patch);
        return;
    }
    lstrcpynA(g_gameDirLower, gameDir, MAX_PATH);
    lower_path(g_gameDirLower);
    g_gameDirLen = (int)strlen(g_gameDirLower);
    g_redir = (Redirect *)calloc(MAX_REDIRECTS, sizeof(Redirect));

    // A stamp line per cached file: "rel <TAB> original size <TAB> original mtime <TAB> patch stamp".
    // A cached file is reused while the original and the patch file are unchanged.
    _snprintf(stampPath, sizeof stampPath, "%s\\cache\\stamp.txt", dataDir);
    stamps = (char *)read_all(stampPath, &stampsLen);
    if (stamps)
    {
        // keep a leading '\n' so that a search for "\n<line>" only matches whole lines
        char *s = (char *)malloc(stampsLen + 2);
        s[0] = '\n';
        memcpy(s + 1, stamps, stampsLen);
        s[stampsLen + 1] = 0;
        free(stamps);
        stamps = s;
    }
    make_parent_dirs(stampPath);
    out = fopen(stampPath, "wb");

    n = rd32(patch + 8);
    p = patch + 12;
    end = patch + size;
    for (i = 0; i < n && p < end && g_redirCount < MAX_REDIRECTS; i++)
    {
        char rel[MAX_PATH], target[MAX_PATH], cache[MAX_PATH], *q;
        WORD plen = *(const WORD *)p;
        const BYTE *srcHash, *dstHash, *ops;
        DWORD srcLen, dstLen, count;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        p += 2;
        if (plen >= MAX_PATH)
            break;
        memcpy(rel, p, plen);
        rel[plen] = 0;
        p += plen;
        srcLen = rd32(p), srcHash = p + 4, p += 36;
        dstLen = rd32(p), dstHash = p + 4, p += 36;
        count = rd32(p), p += 4;
        ops = p;
        p = skip_ops(p, count);
        for (q = rel; *q; q++)
            if (*q == '/')
                *q = '\\';
        _snprintf(target, sizeof target, "%s\\%s", gameDir, rel);
        _snprintf(cache, sizeof cache, "%s\\cache\\%s", dataDir, rel);
        if (!GetFileAttributesExA(target, GetFileExInfoStandard, &fa))
            continue;
        _snprintf(line, sizeof line, "\n%s\t%lu\t%lu.%lu\t%lu.%lu\n", rel, fa.nFileSizeLow,
                  fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime,
                  pa.ftLastWriteTime.dwHighDateTime, pa.ftLastWriteTime.dwLowDateTime);
        if (stamps && strstr(stamps, line) && GetFileAttributesExA(cache, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeLow == dstLen)
        {
            reused++;
        }
        else
        {
            DWORD olen;
            BYTE *orig = read_all(target, &olen), *res = NULL;
            if (!orig)
                continue;
            sha256(orig, olen, h);
            if (olen == dstLen && memcmp(h, dstHash, 32) == 0)
            {
                already++; // the game file is already the Japanese one
                free(orig);
                continue;
            }
            if (olen == srcLen && memcmp(h, srcHash, 32) == 0)
                res = apply_entry(ops, count, orig, olen, dstLen);
            if (res)
                sha256(res, dstLen, h);
            if (!res || memcmp(h, dstHash, 32) != 0)
            {
                if (mismatch++ < 10)
                    jp_log("not translated (the original differs): %s\n", rel);
                free(orig);
                free(res);
                continue;
            }
            {
                FILE *f;
                make_parent_dirs(cache);
                f = fopen(cache, "wb");
                if (!f || fwrite(res, 1, dstLen, f) != dstLen)
                {
                    jp_log("cannot write %s\n", cache);
                    if (f)
                        fclose(f);
                    free(orig);
                    free(res);
                    continue;
                }
                fclose(f);
            }
            made++;
            free(orig);
            free(res);
        }
        if (out)
            fputs(line + 1, out);
        lstrcpynA(g_redir[g_redirCount].rel, rel, MAX_PATH);
        lower_path(g_redir[g_redirCount].rel);
        lstrcpynA(g_redir[g_redirCount].cache, cache, MAX_PATH);
        g_redirCount++;
    }
    if (out)
        fclose(out);
    free(stamps);
    free(patch);
    jp_log("data files: %d translated (%d built, %d cached), %d already Japanese, %d left as they are\n",
           g_redirCount, made, reused, already, mismatch);
    if (!g_redirCount)
        return;
    g_realCreateFileA = (CreateFileA_t)patch_iat("kernel32.dll", "CreateFileA", (void *)my_CreateFileA);
    g_realCreateFileW = (CreateFileW_t)patch_iat("kernel32.dll", "CreateFileW", (void *)my_CreateFileW);
    g_realFindFirstFileExA = (FindFirstFileExA_t)patch_iat("kernel32.dll", "FindFirstFileExA", (void *)my_FindFirstFileExA);
    if (!g_realCreateFileA)
        g_realCreateFileA = CreateFileA;
    if (!g_realCreateFileW)
        g_realCreateFileW = CreateFileW;
    if (!g_realFindFirstFileExA)
        g_realFindFirstFileExA = FindFirstFileExA;
    jp_log("file redirect installed\n");
}


// ---------------------------------------------------------------- config
static char g_fontFace[64] = "MS UI Gothic"; // font for Japanese characters (the game's own bitmap fonts have none)
static int g_sizeAdjust = 1;                  // px added to the Japanese character height
static int g_yAdjust = 0;                     // px added to the Japanese vertical position
static int g_bold = 1;                        // 1 = bold Japanese
static int g_lineGap = 1;                     // px added to the line pitch (size + 1) of wrapped Japanese text
static double g_alphaGamma = 0.8;             // < 1 makes the anti-aliased edges a little stronger

static void load_config(const char *dir)
{
    char ini[MAX_PATH], buf[32];
    _snprintf(ini, sizeof ini, "%s\\avernum_jp.ini", dir);
    GetPrivateProfileStringA("font", "face", g_fontFace, g_fontFace, sizeof g_fontFace, ini);
    g_sizeAdjust = GetPrivateProfileIntA("font", "size_adjust", g_sizeAdjust, ini);
    g_yAdjust = GetPrivateProfileIntA("font", "y_adjust", g_yAdjust, ini);
    g_bold = GetPrivateProfileIntA("font", "bold", g_bold, ini);
    g_lineGap = GetPrivateProfileIntA("font", "line_gap", g_lineGap, ini);
    GetPrivateProfileStringA("font", "alpha_gamma", "", buf, sizeof buf, ini);
    if (buf[0])
        g_alphaGamma = atof(buf);
    if (g_alphaGamma < 0.2 || g_alphaGamma > 3.0)
        g_alphaGamma = 0.8;
}

// ---------------------------------------------------------------- script token estimate
// Before tokenizing a script, 0x46b9e0 estimates the token count to size the token array.
// It counts every byte that is not a digit, letter, '_' or blank as its own token -- also
// inside string literals -- so Japanese adds 2 per character, a long dialogue file overflows
// the count and the game fails while loading it. A literal is still one token, so classify
// bytes >= 0x80 like letters.
//   0x46ba1a: cmp al,30h / jl 46ba29 / cmp al,39h / jg 46ba29   -> jmp tok_class_stub
#define A6_TOKCLASS 0x46ba1a
#define A6_TOK_DIGIT 0x46ba22
#define A6_TOK_OTHER 0x46ba29
#define A6_TOK_LETTER 0x46ba4a
static const BYTE kTokClassOrig[8] = {0x3C, 0x30, 0x7C, 0x0B, 0x3C, 0x39, 0x7F, 0x07};
static DWORD g_tokDigit, g_tokOther, g_tokLetter;

__declspec(naked) static void tok_class_stub(void)
{
    __asm {
        test al, al
        js letter
        cmp al, 0x30
        jl other
        cmp al, 0x39
        jg other
        jmp dword ptr [g_tokDigit]
    other:
        jmp dword ptr [g_tokOther]
    letter:
        jmp dword ptr [g_tokLetter]
    }
}

static int install_token_estimate_fix(void)
{
    BYTE *p = (BYTE *)VA(A6_TOKCLASS);
    DWORD old;
    if (memcmp(p, kTokClassOrig, sizeof kTokClassOrig) != 0)
    {
        jp_log("token estimate fix: unexpected bytes at %06x (not the Steam build?)\n", A6_TOKCLASS);
        return 0;
    }
    g_tokDigit = (DWORD)VA(A6_TOK_DIGIT);
    g_tokOther = (DWORD)VA(A6_TOK_OTHER);
    g_tokLetter = (DWORD)VA(A6_TOK_LETTER);
    VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(DWORD *)(p + 1) = (DWORD)tok_class_stub - (DWORD)(p + 5);
    p[5] = p[6] = p[7] = 0x90;
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    jp_log("token estimate fix installed\n");
    return 1;
}

// ---------------------------------------------------------------- text drawing
// Avernum 6 draws all text with its own bitmap fonts (G290) through one routine:
//   0x47d130  draw_text(ecx = string; target, mode, size, r, g, b, left, top, right, bottom)  cdecl
//             mode 0 = wrap in the rect, 1 = one line centred, 2 = one line at left/top
//             r, g, b are 0..31. Bytes above 0x7f are turned into blanks.
//   0x4805d0  draw_line(ecx = string; r, g, b)  cdecl, pen at the words 0x773de8 / 0x773dec.
//             Each glyph is drawn by the font texture object [0x773ff4] (OpenGL):
//             vtbl+0x84 set_color(r, g, b, a floats), vtbl+0x48 draw(src l, t, r, b floats; x, y; 0; 0).
// draw_text is replaced: the same layout as the original, but EUC-JP characters are measured
// with GDI, rendered into textures of our own (copies of the font texture object with another
// GL texture) and drawn with the same object methods; ASCII runs still go through draw_line so
// English looks exactly the same.
#define A6_DRAW_TEXT 0x47d130
#define A6_DRAW_LINE 0x4805d0
#define A6_CUR_FONT 0x773de4
#define A6_LINE_H 0x773cd4
#define A6_PEN_X 0x773de8
#define A6_PEN_Y 0x773dec
#define A6_MAIN_TARGET 0x6bf5cc
#define A6_OFS_X 0x773dd8 /* draw_text offsets the rect on the main target */
#define A6_OFS_Y 0x773ddc
#define A6_KEEP_IN 0x481610 /* esi = RECT*, cdecl (bounds RECT by value): moves the rect inside */
#define A6_BOUNDS 0x7734e4
#define A6_CLIP 0x7734c4    /* left, top, right, bottom (dwords) used by draw_line */
#define A6_NO_CLIP 0x773af0 /* byte */
#define A6_ALPHA 0x6ac7b0   /* float */
#define A6_ZOOM_A 0x773aec  /* float, 0 unless zoomed */
#define A6_ZOOM_B 0x6ac7b4  /* float, 1 unless zoomed */
#define A6_FONT_TEX 0x773ff4
#define A6_BOUND_TEX 0x5d209c /* GL id last bound by a texture object */
#define A6_COLOR_DIV 0x5bff58 /* double (31.0) */
#define A6_CHARMAP 0x69d930
#define A6_FONT12 0x69c9c8
#define A6_FONT14 0x69cfe8
#define A6_FONT15 0x69ccd8
#define A6_FONT16 0x69d2f8
#define A6_FONT24 0x69d608
static const BYTE kDrawTextOrig[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x34, 0x05, 0x00, 0x00};
static const BYTE kDrawLineOrig[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x68};

#define TEXOBJ_SIZE 0x170
#define TEXOBJ_GLID 0x2c
#define TEXOBJ_PIXELS 0x134
#define TEXOBJ_W 0x13c
#define TEXOBJ_H 0x140
#define TEXOBJ_TARGET 0x144
#define VT_DRAW (0x48 / 4)
#define VT_COLOR (0x84 / 4)
typedef void(__fastcall *TexColorFn)(void *self, void *edx, float r, float g, float b, float a);
typedef void(__fastcall *TexDrawFn)(void *self, void *edx, float sl, float st, float sr, float sb, int x, int y, int z1, int z2);

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

typedef struct
{
    WORD ch;      // ASCII byte (ch < 0x100) or UTF-16 code of a Japanese character
    BYTE cjk;     // 1 = drawn from our glyph textures
    BYTE brk;     // 1 = a line may break before this unit
    short x;      // start position in the line
    short w;      // advance
} TUnit;

#define MAX_UNITS 512
static HFONT g_jfont[40]; // by em size
static HDC g_measureDC;

static const BYTE *font_ptr(void) { return *(const BYTE **)VA(A6_CUR_FONT); }
static short fword(const BYTE *f, int off) { return *(const short *)(f + off); }
static int glyph_index(BYTE c) { return ((const short *)VA(A6_CHARMAP))[(signed char)c]; }
static int jem(int size) { int e = size + g_sizeAdjust; return e < 8 ? 8 : e > 39 ? 39 : e; }

static HFONT jfont(int size)
{
    int em = jem(size);
    if (!g_jfont[em])
    {
        WCHAR face[64];
        MultiByteToWideChar(CP_ACP, 0, g_fontFace, -1, face, 64);
        g_jfont[em] = CreateFontW(-em, 0, 0, 0, g_bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    }
    return g_jfont[em];
}

static int jwidth(int size, WCHAR c)
{
    SIZE sz;
    if (!g_measureDC)
        g_measureDC = CreateCompatibleDC(NULL);
    SelectObject(g_measureDC, jfont(size));
    if (!GetTextExtentPoint32W(g_measureDC, &c, 1, &sz))
        return jem(size);
    return sz.cx;
}

// ---- glyph textures
typedef struct
{
    DWORD key; // (em << 16) | UTF-16 code, 0 = empty
    BYTE page;
    short x, y, w, h;
} CGlyph;

#define CACHE_SIZE 8192
#define MAX_PAGES 32
#define SHELF_H 40
#define DIB_W 64
#define DIB_H 48
static CGlyph g_cache[CACHE_SIZE];
static BYTE *g_pages[MAX_PAGES]; // copies of the font texture object
static int g_pageCount, g_texW, g_texH, g_curX, g_curY;
static GLenum g_target;
static HDC g_dc;
static DWORD *g_dibBits;
static BYTE g_alphaLut[256];
static int g_warnedZoom;

static BYTE *new_page(void)
{
    BYTE *fontObj = *(BYTE **)VA(A6_FONT_TEX);
    BYTE *clone;
    GLuint id;
    void *zeros;
    if (!fontObj || g_pageCount >= MAX_PAGES)
        return NULL;
    if (g_pageCount == 0)
    {
        g_texW = (int)*(float *)(fontObj + TEXOBJ_W);
        g_texH = (int)*(float *)(fontObj + TEXOBJ_H);
        g_target = *(GLenum *)(fontObj + TEXOBJ_TARGET);
        jp_log("font texobj=%p glid=%u size=%dx%d target=0x%x\n", fontObj, *(DWORD *)(fontObj + TEXOBJ_GLID), g_texW,
               g_texH, g_target);
        if (g_texW < 128 || g_texH < 128 || g_texW > 4096 || g_texH > 4096)
            return NULL;
    }
    clone = (BYTE *)malloc(TEXOBJ_SIZE);
    memcpy(clone, fontObj, TEXOBJ_SIZE);
    *(DWORD *)(clone + TEXOBJ_PIXELS) = 0;
    glGenTextures(1, &id);
    glBindTexture(g_target, id);
    *(DWORD *)VA(A6_BOUND_TEX) = id;
    glTexParameteri(g_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(g_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(g_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(g_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    zeros = calloc((size_t)g_texW * g_texH, 4);
    glTexImage2D(g_target, 0, GL_RGBA, g_texW, g_texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);
    free(zeros);
    *(DWORD *)(clone + TEXOBJ_GLID) = id;
    g_pages[g_pageCount++] = clone;
    g_curX = 1;
    g_curY = 1;
    jp_log("new glyph page %d glid=%u\n", g_pageCount - 1, id);
    return clone;
}

static void ensure_gdi(void)
{
    BITMAPINFO bi;
    HBITMAP dib;
    int i;
    if (g_dc)
        return;
    for (i = 0; i < 256; i++)
        g_alphaLut[i] = (BYTE)(255.0 * pow(i / 255.0, g_alphaGamma) + 0.5);
    g_dc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = DIB_W;
    bi.bmiHeader.biHeight = -DIB_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dib = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_dibBits, NULL, 0);
    SelectObject(g_dc, dib);
    SetTextColor(g_dc, RGB(255, 255, 255));
    SetBkColor(g_dc, RGB(0, 0, 0));
    SetBkMode(g_dc, OPAQUE);
}

// The glyph image is the font's cell (cellH px tall), the Japanese em box centred in it.
static CGlyph *get_glyph(int size, int cellH, WCHAR c)
{
    int em = jem(size);
    DWORD key = ((DWORD)em << 16) | ((DWORD)cellH << 24) | c;
    DWORD h = ((DWORD)c * 2654435761u ^ (DWORD)em * 40503u ^ (DWORD)cellH * 977u) & (CACHE_SIZE - 1);
    CGlyph *g;
    TEXTMETRICW tm;
    SIZE sz;
    DWORD *rgba;
    int adv, x, y, top;
    GLint oldRow, oldAlign;
    for (;;)
    {
        g = &g_cache[h];
        if (g->key == key)
            return g;
        if (g->key == 0)
            break;
        h = (h + 1) & (CACHE_SIZE - 1);
    }
    if (cellH > SHELF_H - 2)
        cellH = SHELF_H - 2;
    if (cellH > DIB_H)
        cellH = DIB_H;
    ensure_gdi();
    SelectObject(g_dc, jfont(size));
    GetTextMetricsW(g_dc, &tm);
    GetTextExtentPoint32W(g_dc, &c, 1, &sz);
    adv = sz.cx;
    if (adv > DIB_W - 2)
        adv = DIB_W - 2;
    if (adv < 1)
        adv = 1;
    // centre the em box (tmHeight - tmInternalLeading) in the cell
    top = (cellH - em) / 2 - tm.tmInternalLeading + g_yAdjust;
    memset(g_dibBits, 0, DIB_W * DIB_H * 4);
    TextOutW(g_dc, 0, top, &c, 1);
    GdiFlush();

    if (g_pageCount == 0 || g_curX + adv + 2 > g_texW)
    {
        g_curX = 1;
        g_curY += SHELF_H;
    }
    if (g_pageCount == 0 || g_curY + SHELF_H > g_texH)
        if (!new_page())
            return NULL;

    rgba = (DWORD *)malloc((size_t)adv * cellH * 4 + 4);
    for (y = 0; y < cellH; y++)
        for (x = 0; x < adv; x++)
        {
            DWORD p = g_dibBits[y * DIB_W + x];
            DWORD a = ((p & 0xff) + ((p >> 8) & 0xff) + ((p >> 16) & 0xff)) / 3;
            rgba[y * adv + x] = ((DWORD)g_alphaLut[a] << 24) | 0x00ffffff;
        }
    glBindTexture(g_target, *(DWORD *)(g_pages[g_pageCount - 1] + TEXOBJ_GLID));
    *(DWORD *)VA(A6_BOUND_TEX) = *(DWORD *)(g_pages[g_pageCount - 1] + TEXOBJ_GLID);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRow);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(g_target, 0, g_curX, g_curY, adv, cellH, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, oldRow);
    glPixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);
    free(rgba);

    g->key = key;
    g->page = (BYTE)(g_pageCount - 1);
    g->x = (short)g_curX;
    g->y = (short)g_curY;
    g->w = (short)adv;
    g->h = (short)cellH;
    g_curX += adv + 2;
    return g;
}

// Draw one glyph with its cell's top-left at (x, y), clipped like draw_line does.
static void draw_glyph(const CGlyph *gl, int x, int y, int r, int g, int b)
{
    BYTE *obj = g_pages[gl->page];
    void **vt = *(void ***)obj;
    float alpha = *(float *)VA(A6_ALPHA);
    double div = *(double *)VA(A6_COLOR_DIV);
    int sl = gl->x, st = gl->y, sr = gl->x + gl->w, sb = gl->y + gl->h;
    int dl = x, dt = y, dr = x + gl->w, db = y + gl->h;
    if (alpha == 0.0f)
        return;
    if (div <= 0)
        div = 31.0;
    if (!*(BYTE *)VA(A6_NO_CLIP))
    {
        const int *clip = (const int *)VA(A6_CLIP); // left, top, right, bottom
        if (dr <= clip[0] || dl >= clip[2] || dt >= clip[3] || db <= clip[1])
            return;
        if (dt < clip[1])
            st += clip[1] - dt, dt = clip[1];
        if (dl < clip[0])
            sl += clip[0] - dl, dl = clip[0];
        if (db > clip[3])
            sb += clip[3] - db;
        if (dr >= clip[2])
            sr += clip[2] - dr - 1;
    }
    if (!g_warnedZoom && (*(float *)VA(A6_ZOOM_A) != 0.0f || *(float *)VA(A6_ZOOM_B) != 1.0f))
    {
        g_warnedZoom = 1;
        jp_log("note: text drawn while zoomed (Japanese is drawn unscaled)\n");
    }
    ((TexColorFn)vt[VT_COLOR])(obj, NULL, (float)(1.0 + (r - 31) / div), (float)(1.0 + (g - 31) / div),
                               (float)(1.0 + (b - 31) / div), alpha);
    ((TexDrawFn)vt[VT_DRAW])(obj, NULL, (float)sl, (float)st, (float)sr, (float)sb, dl, dt, 0, 0);
    ((TexColorFn)vt[VT_COLOR])(obj, NULL, 1.0f, 1.0f, 1.0f, 1.0f);
}

static int is_kinsoku_start(WCHAR c) // may not begin a line
{
    return wcschr(L"、。，．・：；？！ー―…‥）」』】〉》〕］｝ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ々～〜", c) != NULL;
}
static int is_kinsoku_end(WCHAR c) // may not end a line
{
    return wcschr(L"（「『【〈《〔［｛", c) != NULL;
}

static unsigned char mac_fix(unsigned char c)
{
    switch (c)
    {
    case 0xc4: case 0xd0: return '-';
    case 0xd2: case 0xd3: return '"';
    case 0xd4: case 0xd5: return '\'';
    }
    return c > 0x7f ? ' ' : c;
}

// Split s into units; ASCII widths as the game measures them (spacing and kerning pairs).
static int make_units(const unsigned char *s, TUnit *u, int max, int size)
{
    const BYTE *f = font_ptr();
    int n = 0, pos = 0, prevAscii = -1;
    while (*s && n < max)
    {
        TUnit *t = &u[n];
        t->brk = 0;
        if ((s[0] >= 0xa1 && s[0] <= 0xfe && s[1] >= 0xa1 && s[1] <= 0xfe) ||
            (s[0] == 0x8e && s[1] >= 0xa1 && s[1] <= 0xdf)) // JIS X 0208 / half-width katakana
        {
            WCHAR w;
            if (MultiByteToWideChar(GAME_CP, 0, (const char *)s, 2, &w, 1) != 1)
                w = L'?';
            t->ch = w;
            t->cjk = 1;
            if (n > 0)
                pos += fword(f, 0xa);
            t->x = (short)pos;
            t->w = (short)jwidth(size, w);
            pos += t->w;
            prevAscii = -1;
            s += 2;
        }
        else
        {
            unsigned char c = mac_fix(s[0]);
            int g = glyph_index(c);
            t->ch = c;
            t->cjk = 0;
            if (n > 0)
            {
                if (prevAscii >= 0 && fword(f, 0xc + g * 8 + 6) > 0 && fword(f, 0xc + glyph_index((BYTE)prevAscii) * 8 + 4) > 0)
                    pos -= fword(f, 0xc + glyph_index((BYTE)prevAscii) * 8 + 4);
                else
                    pos += fword(f, 0xa);
            }
            t->x = (short)pos;
            t->w = (short)(fword(f, 0xc + g * 8 + 2) - fword(f, 0xc + g * 8));
            pos += t->w;
            prevAscii = c;
            s++;
        }
        n++;
    }
    // break opportunities
    {
        int i;
        for (i = 1; i < n; i++)
        {
            TUnit *a = &u[i - 1], *b = &u[i];
            if (b->ch == ' ' || b->ch == '|')
                b->brk = 1;
            else if (a->ch == ' ')
                b->brk = 1;
            else if ((a->cjk || b->cjk) && !(b->cjk && is_kinsoku_start(b->ch)) && !(a->cjk && is_kinsoku_end(a->ch)))
                b->brk = 1;
        }
    }
    return n;
}

static void call_draw_line(const char *s, int r, int g, int b)
{
    DWORD fn = (DWORD)VA(A6_DRAW_LINE);
    __asm {
        push b
        push g
        push r
        mov ecx, s
        call fn
        add esp, 12
    }
}

static void call_keep_in(RECT *rc)
{
    DWORD fn = (DWORD)VA(A6_KEEP_IN);
    const DWORD *bd = (const DWORD *)VA(A6_BOUNDS);
    DWORD a = bd[0], b = bd[1], c = bd[2], d = bd[3];
    __asm {
        push esi
        mov esi, rc
        push d
        push c
        push b
        push a
        call fn
        add esp, 16
        pop esi
    }
}

// Draw units [i0, i1) of one line with the pen at (px, py).
static void draw_units(const TUnit *u, int i0, int i1, int px, int py, int r, int g, int b, int size)
{
    int i = i0, base = u[i0].x;
    const BYTE *f = font_ptr();
    int cellH = fword(f, 8) - fword(f, 4);
    while (i < i1)
    {
        int j = i;
        if (!u[i].cjk)
        {
            char buf[MAX_UNITS + 1];
            int k = 0;
            while (j < i1 && !u[j].cjk)
                buf[k++] = (char)u[j++].ch;
            buf[k] = 0;
            *(WORD *)VA(A6_PEN_X) = (WORD)(px + u[i].x - base);
            *(WORD *)VA(A6_PEN_Y) = (WORD)py;
            call_draw_line(buf, r, g, b);
        }
        else
        {
            for (; j < i1 && u[j].cjk; j++)
            {
                CGlyph *gl = get_glyph(size, cellH, u[j].ch);
                if (gl)
                    draw_glyph(gl, px + u[j].x - base, py, r, g, b);
            }
        }
        i = j;
    }
}

static const char *g_dtString; // set by the stub

static void __cdecl my_draw_text(DWORD target, int mode, int size, int r, int g, int b, int left, int top, int right, int bottom)
{
    static TUnit u[MAX_UNITS];
    char str[0x100];
    RECT rc;
    int n, lh;
    const char *src = g_dtString;
    if (!src || !*src)
        return;
    lstrcpynA(str, src, sizeof str);
    rc.left = left, rc.top = top, rc.right = right, rc.bottom = bottom;
    if (target == *(DWORD *)VA(A6_MAIN_TARGET))
    {
        OffsetRect(&rc, *(short *)VA(A6_OFS_X), *(short *)VA(A6_OFS_Y));
        call_keep_in(&rc);
    }
    // font by size, as the original
    switch (size)
    {
    case 14: *(DWORD *)VA(A6_CUR_FONT) = (DWORD)VA(A6_FONT14); break;
    case 15: *(DWORD *)VA(A6_CUR_FONT) = (DWORD)VA(A6_FONT15); break;
    case 16: case 18: size = 16; *(DWORD *)VA(A6_CUR_FONT) = (DWORD)VA(A6_FONT16); break;
    case 20: case 22: case 24: size = 24; *(DWORD *)VA(A6_CUR_FONT) = (DWORD)VA(A6_FONT24); break;
    default: size = 12; *(DWORD *)VA(A6_CUR_FONT) = (DWORD)VA(A6_FONT12); break;
    }
    lh = size + 1; // line pitch of the original
    *(WORD *)VA(A6_LINE_H) = (WORD)lh;
    if (mode == 2 && strchr(str, '|'))
        mode = 0;
    n = make_units((const unsigned char *)str, u, MAX_UNITS, size);
    if (!n)
        return;
    if (mode == 2)
    {
        draw_units(u, 0, n, rc.left + 2, rc.top + 2, r, g, b, size);
        return;
    }
    if (mode == 1)
    {
        int w = u[n - 1].x; // the original centres on the start of the last character
        int x = (rc.left + rc.right) / 2 - w / 2 + 2;
        int y = (rc.bottom - lh + rc.top) / 2 + 2;
        draw_units(u, 0, n, x, y, r, g, b, size);
        return;
    }
    if (mode == 0)
    {
        int limit = rc.right - rc.left - 6, start = 0, lastBrk = -1, i, y = rc.top + 2;
        for (i = 0; i <= n; i++)
        {
            int forced = (i < n && u[i].ch == '|');
            int over = (i < n && i > start && u[i].x + u[i].w - u[start].x > limit);
            if (i == n || forced || over)
            {
                int end = i;
                if (over && !forced && lastBrk > start)
                    end = lastBrk;
                {
                    int a = start, z = end; // trim blanks at both ends of the line
                    while (a < z && (u[a].ch == ' ' || u[a].ch == '|'))
                        a++;
                    while (z > a && u[z - 1].ch == ' ')
                        z--;
                    if (z > a)
                        draw_units(u, a, z, rc.left + 2, y, r, g, b, size);
                }
                if (i == n)
                    break;
                y += lh + g_lineGap;
                *(WORD *)VA(A6_LINE_H) = (WORD)(*(WORD *)VA(A6_LINE_H) + lh + g_lineGap);
                start = end;
                while (start < n && (u[start].ch == ' ' || u[start].ch == '|'))
                    start++;
                lastBrk = -1;
                i = start;
                if (i >= n)
                    break;
            }
            if (i < n && u[i].brk && i > start)
                lastBrk = i;
        }
    }
}

// Text without Japanese goes to the original routine, so English is laid out exactly as before.
static DWORD g_origDrawText; // copy of the first instructions + jmp back

static int __cdecl has_japanese(const unsigned char *s)
{
    if (!s)
        return 0;
    for (; s[0]; s++)
        if (((s[0] >= 0xa1 && s[0] <= 0xfe) || s[0] == 0x8e) && s[1] >= 0xa1 && s[1] <= 0xfe)
            return 1;
    return 0;
}

__declspec(naked) static void draw_text_stub(void)
{
    __asm {
        mov g_dtString, ecx
        push ecx
        call has_japanese
        add esp, 4
        mov ecx, g_dtString
        test eax, eax
        jz english
        jmp my_draw_text
    english:
        jmp dword ptr [g_origDrawText]
    }
}

static int make_trampoline(void)
{
    BYTE *t = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    BYTE *orig = (BYTE *)VA(A6_DRAW_TEXT);
    if (!t)
        return 0;
    memcpy(t, orig, sizeof kDrawTextOrig); // push ebp / mov ebp,esp / sub esp,534h: position independent
    t[sizeof kDrawTextOrig] = 0xE9;
    *(DWORD *)(t + sizeof kDrawTextOrig + 1) = (DWORD)(orig + sizeof kDrawTextOrig) - (DWORD)(t + sizeof kDrawTextOrig + 5);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    g_origDrawText = (DWORD)t;
    return 1;
}

static int jump_to(DWORD va, void *fn, const BYTE *expect, int len, const char *name)
{
    BYTE *p = (BYTE *)VA(va);
    DWORD old;
    if (memcmp(p, expect, len) != 0)
    {
        jp_log("%s: unexpected bytes at %06x (not the Steam build?)\n", name, va);
        return 0;
    }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *(DWORD *)(p + 1) = (DWORD)fn - (DWORD)(p + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    jp_log("%s replaced\n", name);
    return 1;
}

// ---------------------------------------------------------------- text tables in .data
// Most interface text (skills, spells, help, quests, the story) is kept in fixed-size char arrays in
// .data. avernum_jp_data.tsv (UTF-8): va, max_bytes, english, japanese. A translation is written in
// place when the English at that address matches and the translation fits in max_bytes.
static void load_data_slots(const char *dir)
{
    char path[MAX_PATH], *buf, *line, *next;
    DWORD size;
    int ok = 0, mismatch = 0, toolong = 0, unmappable = 0;
    _snprintf(path, sizeof path, "%s\\avernum_jp_data.tsv", dir);
    buf = (char *)read_all(path, &size);
    if (!buf)
    {
        jp_log("no %s (text tables stay English)\n", path);
        return;
    }
    buf = (char *)realloc(buf, size + 1);
    buf[size] = 0;
    for (line = buf; line && *line; line = next)
    {
        char *col[4] = {0}, *p, *jp;
        int c = 0;
        DWORD va;
        unsigned maxb;
        BYTE *dst;
        DWORD old;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if ((p = strchr(line, '\r')) != NULL)
            *p = 0;
        if (strncmp(line, "0x", 2) != 0)
            continue;
        col[0] = line;
        for (p = line; *p && c < 3; p++)
            if (*p == '\t')
            {
                *p = 0;
                col[++c] = p + 1;
            }
        if (c < 3 || !col[3][0])
            continue;
        unescape(col[2]);
        unescape(col[3]);
        va = strtoul(col[0], NULL, 16);
        maxb = (unsigned)strtoul(col[1], NULL, 10);
        dst = (BYTE *)VA(va);
        if (strcmp((const char *)dst, col[2]) != 0)
        {
            mismatch++;
            continue;
        }
        jp = to_game_encoding(col[3], &unmappable);
        if (strlen(jp) > maxb)
        {
            toolong++;
            continue;
        }
        VirtualProtect(dst, maxb + 1, PAGE_READWRITE, &old);
        memcpy(dst, jp, strlen(jp) + 1);
        VirtualProtect(dst, maxb + 1, old, &old);
        ok++;
    }
    free(buf);
    jp_log("text tables: %d translated, %d not matching this exe, %d too long\n", ok, mismatch, toolong);
}

// ---------------------------------------------------------------- start-up
// Only the Steam build is supported: when its code is not where expected, nothing is changed
// at all and the game runs in English.
static int steam_build_ok(void)
{
    return memcmp(VA(A6_DRAW_TEXT), kDrawTextOrig, sizeof kDrawTextOrig) == 0 &&
           memcmp(VA(A6_DRAW_LINE), kDrawLineOrig, sizeof kDrawLineOrig) == 0 &&
           memcmp(VA(A6_TOKCLASS), kTokClassOrig, sizeof kTokClassOrig) == 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char dir[MAX_PATH], data[MAX_PATH], path[MAX_PATH], *p;
        DWORD attr;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, dir, MAX_PATH);
        p = strrchr(dir, '\\');
        if (p)
            *p = 0;
        _snprintf(data, sizeof data, "%s\\avernum_jp", dir);
        attr = GetFileAttributesA(data);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            lstrcpynA(data, dir, MAX_PATH);
        _snprintf(path, sizeof path, "%s\\avernum_jp.log", data);
        g_log = fopen(path, "w");
        load_config(dir);
        g_base = (BYTE *)GetModuleHandleA(NULL);
        jp_log("Avernum 6 JP hook loaded. base=%p face=%s bold=%d size_adjust=%d y_adjust=%d line_gap=%d\n", g_base,
               g_fontFace, g_bold, g_sizeAdjust, g_yAdjust, g_lineGap);
        if (!steam_build_ok())
        {
            jp_log("not supported: this Avernum 6.exe is not the Steam build the patch was made for; "
                   "the game runs in English\n");
            return TRUE;
        }
        install_token_estimate_fix();
        if (make_trampoline())
            jump_to(A6_DRAW_TEXT, (void *)draw_text_stub, kDrawTextOrig, sizeof kDrawTextOrig, "draw_text");
        load_exe_strings(data);
        load_data_slots(data);
        prepare_redirects(dir, data);
    }
    return TRUE;
}
