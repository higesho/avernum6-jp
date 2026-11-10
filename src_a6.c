
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
