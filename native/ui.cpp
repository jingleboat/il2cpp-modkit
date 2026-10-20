/*
 * ModKitTweaks · 游戏内面板（Android / IL2CPP，纯 native 构建 UGUI）
 * ===============================================================
 * 手机端没有快捷键，所以面板「默认展开」，点标题栏折叠成一条 tab，再点展开。
 *
 * 为什么不走 Java 层：给游戏加窗口必须上 Android UI 线程，而那需要一个 Runnable
 * —— 没有自己的 dex 就造不出来。所以整块面板走 Unity 侧最小 UGUI 链路：
 *
 *   [游戏已有的 Canvas]  ← 直接挂在 UIRoot 的 Canvas 下面（不自己建 Canvas）
 *     └─ new GameObject("ModKitImg") + RawImage   ← 唯一的绘制单元
 *          texture = 本文件软件光栅化出来的 RGBA32 位图
 *
 * 也就是说：背景、圆角、开关药丸、文字全部由本文件的软件光栅器画进一张纹理，
 * 状态一变就重画 + LoadRawTextureData + Apply，不再建第二个 GameObject。
 * 中文字形来自 gen_glyphs.py 在构建期生成的 8 位 alpha 图集（glyphs.h），
 * 运行期完全不需要字体资源（Unity 内置字体没有中文）。
 *
 * 注意：父 Canvas 带 CanvasScaler（scaleFactor 通常 > 1），所以尺寸/坐标一律
 * 除以 scaleFactor，让纹理保持 1 像素 = 1 屏幕像素，字才不糊。
 */
#include "common.h"
#include "ui.h"
#include "hook.h"
#include "glyphs.h"

#include <string.h>
#include <math.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ 布局 */
#define PW 520
#define PH 500
#define HDR 68
#define ROW 60
#define PAD 10
#define PILL_W 84
#define PILL_H 38
#define TAB_W 220
#define OFF_R 252.0f      /* 距屏幕右边缘（让开右侧竖排按钮，屏幕像素） */
#define OFF_T 100.0f      /* 距屏幕上边缘（让开 存档/读档/系统，屏幕像素） */

/* 第 i 行对应的运行时开关 */
static volatile int* row_flag_ptr(int i) {
    switch (i) {
        case 0: return &F_TRADE_WIN;
        case 1: return &F_TRADE_POOLS;
        case 2: return &F_TRADE_TAKEALL;
        case 3: return &F_MAP;
        case 4: return &F_CAB;
        case 5: return &F_STORY;
        case 6: return &F_WATER;
        default: return nullptr;
    }
}

#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define C_BG RGBA(22, 24, 32, 244)
#define C_HDR RGBA(38, 42, 58, 252)
#define C_BORDER RGBA(96, 104, 132, 255)
#define C_TEXT RGBA(232, 234, 244, 255)
#define C_TITLE RGBA(255, 255, 255, 255)
#define C_ON RGBA(44, 138, 86, 255)
#define C_OFF RGBA(76, 78, 94, 255)
#define C_PILLTXT RGBA(255, 255, 255, 255)
#define C_HINT RGBA(150, 156, 176, 255)

/* ------------------------------------------------------------------ 状态 */
static uint8_t g_buf[PW * PH * 4];
static void* g_canvas = nullptr;
static void* g_img = nullptr;
static void* g_tex = nullptr;
static void* g_rt = nullptr;
static float g_scale = 1.0f;
static int g_collapsed = 0;
static int g_ready = 0;
static int g_fails = 0;
static int g_ticks = 0;

static bool (*orig_mouse_down)(int) = nullptr;
static V3 (*orig_mouse_pos)() = nullptr;
static void (*orig_mtsc_update)(void*) = nullptr;

#define F(ret, rva, ...) ((ret (*)(__VA_ARGS__, void*))(g_base + (rva)))
#define F0(ret, rva) ((ret (*)(void*))(g_base + (rva)))

/* ================================================================ 软件光栅 */
static inline void blend(int x, int ytop, uint32_t col, float cov) {
    if (x < 0 || x >= PW || ytop < 0 || ytop >= PH) return;
    float a = ((col >> 24) & 0xFF) / 255.0f * cov;
    if (a <= 0.0f) return;
    uint8_t* p = &g_buf[((PH - 1 - ytop) * PW + x) * 4];
    float sr = (float)(col & 0xFF), sg = (float)((col >> 8) & 0xFF), sb = (float)((col >> 16) & 0xFF);
    p[0] = (uint8_t)(p[0] * (1 - a) + sr * a);
    p[1] = (uint8_t)(p[1] * (1 - a) + sg * a);
    p[2] = (uint8_t)(p[2] * (1 - a) + sb * a);
    p[3] = (uint8_t)(p[3] * (1 - a) + 255 * a);
}

static void fill_rect(int x, int ytop, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) blend(x + i, ytop + j, col, 1.0f);
}

static void round_rect(int x, int ytop, int w, int h, int r, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            int cx = -1, cy = -1;
            if (i < r && j < r) { cx = r; cy = r; }
            else if (i >= w - r && j < r) { cx = w - r - 1; cy = r; }
            else if (i < r && j >= h - r) { cx = r; cy = h - r - 1; }
            else if (i >= w - r && j >= h - r) { cx = w - r - 1; cy = h - r - 1; }
            float cov = 1.0f;
            if (cx >= 0) {
                float dx = (float)(i - cx), dy = (float)(j - cy);
                float d = sqrtf(dx * dx + dy * dy);
                cov = (float)r + 0.5f - d;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
            }
            blend(x + i, ytop + j, col, cov);
        }
    }
}

static int utf8_next(const unsigned char** s, uint32_t* cp) {
    const unsigned char* p = *s;
    uint32_t c = *p;
    if (!c) return 0;
    if (c < 0x80) { *cp = c; *s = p + 1; return 1; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); *s = p + 2; return 1; }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s = p + 3;
        return 1;
    }
    *cp = '?';
    *s = p + 1;
    return 1;
}

static int glyph_index(uint32_t cp) {
    for (int i = 0; i < GLYPH_COUNT; ++i) if (G_CP[i] == cp) return i;
    return -1;
}

static int text_width(const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    uint32_t cp;
    int w = 0;
    while (utf8_next(&p, &cp)) {
        int gi = glyph_index(cp);
        w += (gi >= 0) ? G_ADV[gi] : 10;
    }
    return w;
}

static void draw_text(int x, int ytop, const char* s, uint32_t col) {
    const unsigned char* p = (const unsigned char*)s;
    uint32_t cp;
    int cx = x;
    while (utf8_next(&p, &cp)) {
        int gi = glyph_index(cp);
        if (gi < 0) { cx += 10; continue; }
        for (int gy = 0; gy < GLYPH_H; ++gy) {
            for (int gx = 0; gx < GLYPH_W; ++gx) {
                unsigned char a = G_BITS[gi][gy * GLYPH_W + gx];
                if (!a) continue;
                blend(cx + gx, ytop + gy, col, a / 255.0f);
            }
        }
        cx += G_ADV[gi];
    }
}

/* ================================================================== 重绘 */
static void redraw() {
    memset(g_buf, 0, sizeof(g_buf));

    int w = g_collapsed ? TAB_W : PW;
    int h = g_collapsed ? HDR : PH;

    round_rect(0, 0, w, h, PAD, C_BG);
    round_rect(0, 0, w, HDR, PAD, C_HDR);
    fill_rect(0, HDR - PAD, w, PAD, C_HDR);
    fill_rect(0, 0, w, 2, C_BORDER);
    fill_rect(0, 0, 2, h, C_BORDER);
    if (!g_collapsed) {
        fill_rect(0, h - 2, w, 2, C_BORDER);
        fill_rect(w - 2, 0, 2, h, C_BORDER);
    }

    int tw = text_width(G_TITLE);
    draw_text(PAD + 12, (HDR - GLYPH_H) / 2, G_TITLE, C_TITLE);
    {
        int ax = PAD + 12 + tw + 16;
        int ay = HDR / 2;
        uint32_t c = C_HINT;
        if (g_collapsed) {          /* ▶：底边在左，尖在右 */
            for (int i = 0; i < 9; ++i) fill_rect(ax + (8 - i), ay - i, 2, i * 2 + 2, c);
        } else {                    /* ▼：底边在上，尖在下 */
            for (int j = 0; j < 9; ++j) fill_rect(ax + j, ay - 9 + j, 18 - 2 * j, 2, c);
        }
    }
    if (g_collapsed) return;

    for (int i = 0; i < G_ROW_COUNT; ++i) {
        int rt = HDR + i * ROW;
        if (i > 0) fill_rect(PAD + 12, rt, PW - (PAD + 12) * 2, 1, RGBA(52, 56, 72, 200));
        draw_text(PAD + 12, rt + (ROW - GLYPH_H) / 2, G_ROWS[i], C_TEXT);

        volatile int* f = row_flag_ptr(i);
        int on = f ? *f : 0;
        int px = PW - PAD - 12 - PILL_W;
        int py = rt + (ROW - PILL_H) / 2;
        round_rect(px, py, PILL_W, PILL_H, PILL_H / 2, on ? C_ON : C_OFF);
        const char* t = on ? G_ON : G_OFF;
        int lw = text_width(t);
        draw_text(px + (PILL_W - lw) / 2, py + (PILL_H - GLYPH_H) / 2, t, C_PILLTXT);
    }
}

/* ============================================================== UGUI 构建 */
static void upload() {
    if (!g_tex) return;
    F(void, RVA_TEX_LOAD_RAW, void*, void*, int)(g_tex, (void*)g_buf, PW * PH * 4, nullptr);
    F(void, RVA_TEX_APPLY, void*)(g_tex, nullptr);
}

static int screen_h() { return F0(int, RVA_SCREEN_HEIGHT)(nullptr); }
static int screen_w() { return F0(int, RVA_SCREEN_WIDTH)(nullptr); }

/* 面板在**屏幕像素**里的左下角与尺寸 */
static void panel_px(float* left, float* bottom, float* w, float* h) {
    float sw = (float)screen_w(), sh = (float)screen_h();
    *w = (float)(g_collapsed ? TAB_W : PW);
    *h = (float)(g_collapsed ? HDR : PH);
    *left = sw - OFF_R - *w;
    *bottom = sh - OFF_T - *h;
}

static void apply_layout() {
    if (!g_rt || !g_img) return;
    float left, bottom, w, h;
    panel_px(&left, &bottom, &w, &h);
    float sc = g_scale > 0.01f ? g_scale : 1.0f;

    V2 anchor = {1.0f, 1.0f};
    F(void, RVA_RT_ANCHOR_MIN, void*, V2)(g_rt, anchor, nullptr);
    F(void, RVA_RT_ANCHOR_MAX, void*, V2)(g_rt, anchor, nullptr);
    F(void, RVA_RT_PIVOT, void*, V2)(g_rt, (V2){1.0f, 1.0f}, nullptr);
    F(void, RVA_RT_ANCHORED_POS, void*, V2)(g_rt, (V2){-OFF_R / sc, -OFF_T / sc}, nullptr);
    F(void, RVA_RT_SIZE_DELTA, void*, V2)(g_rt, (V2){w / sc, h / sc}, nullptr);

    Rect4 uv;
    uv.x = 0.0f;
    uv.y = g_collapsed ? (float)(PH - HDR) / (float)PH : 0.0f;
    uv.w = g_collapsed ? (float)TAB_W / (float)PW : 1.0f;
    uv.h = g_collapsed ? (float)HDR / (float)PH : 1.0f;
    F(void, RVA_RAWIMG_SET_UVRECT, void*, Rect4)(g_img, uv, nullptr);
}

/* 找一块现成的 Canvas 当爹：UIRoot 的 Canvas 优先（常驻），退回 FindObjectOfType */
static void* find_parent_canvas(void* kCanvas) {
    void* kUiRoot = find_class("WuLin", "UIRoot");
    if (kUiRoot) {
        void* uiroot = find_object_of_type(kUiRoot);
        if (uiroot) {
            void* go = F(void*, RVA_COMP_GET_GO, void*)(uiroot, nullptr);
            if (go) {
                void* c = F(void*, RVA_GO_GET_COMPONENT, void*, void*)(go, class_type(kCanvas), nullptr);
                if (c) { LOGI("面板: 用 UIRoot 的 Canvas=%p", c); return c; }
            }
        }
    }
    void* c = find_object_of_type(kCanvas);
    if (c) LOGI("面板: 退回 FindObjectOfType(Canvas)=%p", c);
    return c;
}

static int ui_init() {
    if (g_ready) return 1;
    if (!g_base || !A_domain_get || !A_object_new) return 0;

    void* kGo = find_class("UnityEngine", "GameObject");
    void* kCanvas = find_class("UnityEngine", "Canvas");
    void* kRaw = find_class("UnityEngine.UI", "RawImage");
    void* kTex = find_class("UnityEngine", "Texture2D");
    LOGI("面板: 类 go=%p canvas=%p raw=%p tex=%p", kGo, kCanvas, kRaw, kTex);
    if (!kGo || !kCanvas || !kRaw || !kTex) return 0;
    if (A_class_init) {
        A_class_init(kGo);
        A_class_init(kCanvas);
        A_class_init(kRaw);
        A_class_init(kTex);
    }

    /* ---- 1) 自己的 Canvas：sortingOrder 拉到 32000，保证盖在游戏 UI 之上 ----
     * （挂在游戏已有 Canvas 下面试过：会被主菜单标题画、以及排序更高的
     *   游戏 Canvas 盖住，所以还是自己建一块覆盖层画布。） */
    void* canvas = nullptr;
    void* rootTr = nullptr;
    void* goRoot = A_object_new(kGo);
    if (goRoot) {
        F(void, RVA_GO_CTOR_NAME, void*, void*)(goRoot, A_string_new("ModKitPanel"), nullptr);
        canvas = F(void*, RVA_GO_ADD_COMP, void*, void*)(goRoot, class_type(kCanvas), nullptr);
        LOGI("面板: 自建 Canvas=%p", canvas);
        if (canvas) {
            F(void, RVA_CANVAS_SET_MODE, void*, int)(canvas, 0, nullptr);      /* Overlay */
            F(void, RVA_CANVAS_SET_ORDER, void*, int)(canvas, 32000, nullptr);
            rootTr = F(void*, RVA_GO_GET_TRANSFORM, void*)(goRoot, nullptr);
            F(void, RVA_DONTDESTROY, void*)(goRoot, nullptr);
        }
    }
    if (!canvas || !rootTr) {
        /* 兜底：退回挂在游戏自己的 Canvas 下（可能被别的 UI 盖住，但能用） */
        LOGE("面板: 自建 Canvas 失败，退回游戏 Canvas");
        canvas = find_parent_canvas(kCanvas);
        if (!canvas) { LOGE("面板: 找不到任何 Canvas"); return 0; }
        rootTr = F(void*, RVA_COMP_GET_TRANSFORM, void*)(canvas, nullptr);
        if (!rootTr) { LOGE("面板: Canvas 没有 transform"); return 0; }
    }

    /* ---- 2) 绘制单元：RawImage ---- */
    void* go = A_object_new(kGo);
    if (!go) { LOGE("面板: 建对象失败"); return 0; }
    F(void, RVA_GO_CTOR_NAME, void*, void*)(go, A_string_new("ModKitImg"), nullptr);
    void* raw = F(void*, RVA_GO_ADD_COMP, void*, void*)(go, class_type(kRaw), nullptr);
    LOGI("面板: RawImage=%p", raw);
    if (!raw) { LOGE("面板: 挂 RawImage 失败"); return 0; }
    F(void, RVA_GRAPHIC_SET_RAYCAST, void*, int)(raw, 0, nullptr);
    void* rt = F(void*, RVA_GO_GET_TRANSFORM, void*)(go, nullptr);
    if (!rt) { LOGE("面板: RawImage 没有 transform"); return 0; }
    F(void, RVA_TR_SETPARENT, void*, void*, int)(rt, rootTr, 0, nullptr);

    float sc = F(float, RVA_CANVAS_SCALE, void*)(canvas, nullptr);
    if (sc < 0.01f) sc = 1.0f;
    LOGI("面板: Canvas=%p transform=%p scaleFactor=%.2f", canvas, rootTr, sc);

    /* ---- 3) 纹理 ---- */
    void* tex = A_object_new(kTex);
    if (!tex) { LOGE("面板: 建纹理失败"); return 0; }
    F(void, RVA_TEX_CTOR, void*, int, int, int, int, int)(tex, PW, PH, 4, 1, 0, nullptr);
    F(void, RVA_TEX_SET_FILTER, void*, int)(tex, 0, nullptr);   /* Point */
    F(void, RVA_TEX_SET_WRAP, void*, int)(tex, 0, nullptr);    /* Clamp */

    g_canvas = canvas;
    g_img = raw;
    g_rt = rt;
    g_tex = tex;
    g_scale = sc;

    redraw();
    upload();
    F(void, RVA_RAWIMG_SET_TEX, void*, void*)(raw, tex, nullptr);
    apply_layout();
    LOGI("面板: 初始化完成 %dx%d，比 1:%.2f（默认展开）", PW, PH, sc);
    g_ready = 1;
    return 1;
}

/* ================================================================ 触摸 */
static void set_collapsed(int c) {
    if (g_collapsed == c) return;
    g_collapsed = c;
    redraw();
    upload();
    apply_layout();
    LOGI("面板: %s", c ? "已折叠" : "已展开");
}

static void toggle_row(int i) {
    volatile int* f = row_flag_ptr(i);
    if (!f) return;
    *f = *f ? 0 : 1;
    redraw();
    upload();
    LOGI("面板: 第 %d 行 -> %s", i, *f ? "开" : "关");
}

static void on_tap(float sx, float sy) {
    if (!g_ready) return;
    float sc = g_scale > 0.01f ? g_scale : 1.0f;
    float cx = sx / sc, cy = sy / sc;      /* 屏幕像素 → canvas 单位 */
    float left, bottom, w, h;
    panel_px(&left, &bottom, &w, &h);
    left /= sc; bottom /= sc; w /= sc; h /= sc;
    if (cx < left || cx > left + w || cy < bottom || cy > bottom + h) return;
    float ly = (bottom + h) - cy;          /* 距离面板上沿 */
    if (g_collapsed) { set_collapsed(0); return; }
    if (ly * sc <= (float)HDR) { set_collapsed(1); return; }
    int row = (int)((ly * sc - (float)HDR) / (float)ROW);
    if (row >= 0 && row < G_ROW_COUNT) toggle_row(row);
}

int ui_hit_test_screen(float sx, float sy) {
    if (!g_ready) return 0;
    float left, bottom, w, h;
    panel_px(&left, &bottom, &w, &h);
    return sx >= left && sx <= left + w && sy >= bottom && sy <= bottom + h;
}

/* ================================================================ hooks */
static bool hook_mouse_down(int btn) {
    bool r = orig_mouse_down ? orig_mouse_down(btn) : false;
    if (!r || btn != 0 || !g_ready) return r;
    V3 p = orig_mouse_pos ? orig_mouse_pos() : (V3){0, 0, 0};
    return ui_hit_test_screen(p.x, p.y) ? false : r;   /* 点到面板就不透给游戏 */
}

static void hook_mtsc_update(void* self) {
    if (orig_mtsc_update) orig_mtsc_update(self);
    if (g_fails > 3) return;
    if (++g_ticks < 5) return;

    if (!g_ready) {
        if (g_ticks < 40 || (g_ticks % 120) == 0) {
            if (!ui_init()) { g_fails++; LOGE("面板: 初始化失败 %d", g_fails); }
        }
        if (!g_ready) return;
    }

    bool down = orig_mouse_down ? orig_mouse_down(0) : false;
    if (down) {
        V3 p = orig_mouse_pos ? orig_mouse_pos() : (V3){0, 0, 0};
        on_tap(p.x, p.y);
    }
}

void ui_install() {
    hook_install("面板.Input.GetMouseButtonDown", (void*)(g_base + RVA_INPUT_MOUSE_DOWN),
                 (void*)hook_mouse_down, (void**)&orig_mouse_down);
    if (!orig_mouse_down)
        orig_mouse_down = (bool (*)(int))(g_base + RVA_INPUT_MOUSE_DOWN);
    orig_mouse_pos = (V3 (*)())(g_base + RVA_INPUT_MOUSE_POS);
    hook_install("面板.MainThreadSync.Update", (void*)(g_base + RVA_MTSC_UPDATE),
                 (void*)hook_mtsc_update, (void**)&orig_mtsc_update);
}
