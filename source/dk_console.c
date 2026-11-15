#include <string.h>

#include "dk_console.h"

#define CON_W 1280
#define CON_H 720
#define CON_FB_NUM 2
#define CON_CMDMEM_SIZE 0x1000
#define CON_LINEAR_SIZE (CON_W * CON_H * 4)
#define CON_FENCE_TIMEOUT_NS (1000000000LL)

#define CON_RGB(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u)

static const u32 kColors[24] = {
    CON_RGB(  0,  0,  0), CON_RGB(128,  0,  0), CON_RGB(  0,128,  0), CON_RGB(128,128,  0),
    CON_RGB(  0,  0,128), CON_RGB(128,  0,128), CON_RGB(  0,128,128), CON_RGB(192,192,192),

    CON_RGB(128,128,128), CON_RGB(255,  0,  0), CON_RGB(  0,255,  0), CON_RGB(255,255,  0),
    CON_RGB(  0,  0,255), CON_RGB(255,  0,255), CON_RGB(  0,255,255), CON_RGB(255,255,255),

    CON_RGB(  0,  0,  0), CON_RGB( 64,  0,  0), CON_RGB(  0, 64,  0), CON_RGB( 64, 64,  0),
    CON_RGB(  0,  0, 64), CON_RGB( 64,  0, 64), CON_RGB(  0, 64, 64), CON_RGB( 96, 96, 96),
};

typedef struct {
    ConsoleRenderer base;
    DkDevice device;
    DkQueue queue;
    DkMemBlock fbMem;
    DkMemBlock linearMem;
    DkMemBlock cmdMem;
    DkCmdBuf cmdbuf;
    DkImage images[CON_FB_NUM];
    DkSwapchain swapchain;
    DkCmdList copy[CON_FB_NUM];
    DkFence fence;
    u32* pixels;
    bool busy;
    bool dirty;
    bool presenting;
    bool initialized;
} DrakoConsole;

static DrakoConsole s_con;

static u32 fromRgb565(u16 c)
{
    u32 r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return CON_RGB((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

static u32* beginDraw(void)
{
    if (s_con.busy) {
        dkFenceWait(&s_con.fence, CON_FENCE_TIMEOUT_NS);
        s_con.busy = false;
    }
    s_con.dirty = true;
    return s_con.pixels;
}

static bool drakoConsole_init(PrintConsole* con)
{
    if (con->font.tileWidth != 16 || con->font.tileHeight != 16)
        return false;
    if (con->consoleWidth * 16 > CON_W || con->consoleHeight * 16 > CON_H)
        return false;
    if (s_con.initialized)
        return true;

    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, s_con.device);
    layoutMaker.flags = DkImageFlags_UsagePresent;
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    layoutMaker.dimensions[0] = CON_W;
    layoutMaker.dimensions[1] = CON_H;
    DkImageLayout fbLayout;
    dkImageLayoutInitialize(&fbLayout, &layoutMaker);

    uint32_t fbAlign = dkImageLayoutGetAlignment(&fbLayout);
    uint32_t fbSize = dkImageLayoutGetSize(&fbLayout);
    fbSize = (fbSize + fbAlign - 1) & ~(fbAlign - 1);

    DkMemBlockMaker mbMaker;
    dkMemBlockMakerDefaults(&mbMaker, s_con.device, CON_FB_NUM * fbSize);
    mbMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    s_con.fbMem = dkMemBlockCreate(&mbMaker);

    DkImage const* swapImages[CON_FB_NUM];
    for (unsigned i = 0; i < CON_FB_NUM; i++) {
        dkImageInitialize(&s_con.images[i], &fbLayout, s_con.fbMem, i * fbSize);
        swapImages[i] = &s_con.images[i];
    }

    DkSwapchainMaker swapMaker;
    dkSwapchainMakerDefaults(&swapMaker, s_con.device, nwindowGetDefault(), swapImages, CON_FB_NUM);
    s_con.swapchain = dkSwapchainCreate(&swapMaker);

    dkMemBlockMakerDefaults(&mbMaker, s_con.device, CON_LINEAR_SIZE);
    mbMaker.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached;
    s_con.linearMem = dkMemBlockCreate(&mbMaker);
    s_con.pixels = (u32*)dkMemBlockGetCpuAddr(s_con.linearMem);

    dkMemBlockMakerDefaults(&mbMaker, s_con.device, CON_CMDMEM_SIZE);
    mbMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    s_con.cmdMem = dkMemBlockCreate(&mbMaker);

    DkCmdBufMaker cbMaker;
    dkCmdBufMakerDefaults(&cbMaker, s_con.device);
    s_con.cmdbuf = dkCmdBufCreate(&cbMaker);
    dkCmdBufAddMemory(s_con.cmdbuf, s_con.cmdMem, 0, CON_CMDMEM_SIZE);

    const DkCopyBuf linearSrc = { dkMemBlockGetGpuAddr(s_con.linearMem), 0, 0 };
    const DkImageRect rect = { 0, 0, 0, CON_W, CON_H, 1 };
    for (unsigned i = 0; i < CON_FB_NUM; i++) {
        DkImageView dst;
        dkImageViewDefaults(&dst, &s_con.images[i]);
        dkCmdBufCopyBufferToImage(s_con.cmdbuf, &linearSrc, &dst, &rect, 0);
        dkCmdBufSignalFence(s_con.cmdbuf, &s_con.fence, false);
        s_con.copy[i] = dkCmdBufFinishList(s_con.cmdbuf);
    }

    // Kept separate from the compute queue so presenting never touches the stress queue
    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, s_con.device);
    queueMaker.flags = 0;
    s_con.queue = dkQueueCreate(&queueMaker);

    for (uint32_t i = 0; i < CON_W * CON_H; i++)
        s_con.pixels[i] = kColors[0];
    s_con.dirty = true;
    s_con.busy = false;
    s_con.initialized = true;
    return true;
}

static void drakoConsole_deinit(PrintConsole* con)
{
    (void)con;
    if (!s_con.initialized)
        return;

    dkQueueWaitIdle(s_con.queue);
    dkQueueDestroy(s_con.queue);
    dkCmdBufDestroy(s_con.cmdbuf);
    dkMemBlockDestroy(s_con.cmdMem);
    dkMemBlockDestroy(s_con.linearMem);
    dkSwapchainDestroy(s_con.swapchain);
    dkMemBlockDestroy(s_con.fbMem);
    s_con.pixels = NULL;
    s_con.initialized = false;
}

static void drakoConsole_drawChar(PrintConsole* con, int x, int y, int c)
{
    u32* px = beginDraw();
    const u16* glyph = (const u16*)con->font.gfx + 16 * c;

    u32 fg, bg;
    if (con->flags & CONSOLE_FG_CUSTOM) {
        fg = fromRgb565(con->fg);
    } else if (con->flags & CONSOLE_COLOR_BOLD) {
        fg = kColors[con->fg + 8];
    } else if (con->flags & CONSOLE_COLOR_FAINT) {
        fg = kColors[con->fg + 16];
    } else {
        fg = kColors[con->fg];
    }
    bg = (con->flags & CONSOLE_BG_CUSTOM) ? fromRgb565(con->bg) : kColors[con->bg];

    if (con->flags & CONSOLE_COLOR_REVERSE) {
        u32 tmp = fg;
        fg = bg;
        bg = tmp;
    }

    u32* cell = px + (y * 16) * CON_W + x * 16;
    for (int row = 0; row < 16; row++) {
        u16 bits = glyph[row];
        if (row == 15 && (con->flags & CONSOLE_UNDERLINE)) bits = 0xFFFF;
        if (row == 7 && (con->flags & CONSOLE_CROSSED_OUT)) bits = 0xFFFF;

        u32* line = cell + row * CON_W;
        for (int col = 0; col < 16; col++)
            line[col] = (bits & (0x8000u >> col)) ? fg : bg;
    }
}

static void drakoConsole_scrollWindow(PrintConsole* con)
{
    u32* px = beginDraw();
    int x = (con->windowX - 1) * 16;
    int y = (con->windowY - 1) * 16;
    size_t rowBytes = (size_t)con->windowWidth * 16 * sizeof(u32);

    for (int j = 0; j < (con->windowHeight - 1) * 16; j++)
        memmove(px + (y + j) * CON_W + x, px + (y + j + 16) * CON_W + x, rowBytes);
}

static void drakoConsole_flushAndSwap(PrintConsole* con)
{
    (void)con;
    if (!s_con.dirty || s_con.presenting)
        return;
    s_con.presenting = true;
    s_con.dirty = false;

    dkMemBlockFlushCpuCache(s_con.linearMem, 0, CON_LINEAR_SIZE);
    int slot = dkQueueAcquireImage(s_con.queue, s_con.swapchain);
    dkQueueSubmitCommands(s_con.queue, s_con.copy[slot]);
    dkQueuePresentImage(s_con.queue, s_con.swapchain, slot);
    s_con.busy = true;
    s_con.presenting = false;
}

ConsoleRenderer* drakoConsoleRenderer(DkDevice device)
{
    s_con.base.init = drakoConsole_init;
    s_con.base.deinit = drakoConsole_deinit;
    s_con.base.drawChar = drakoConsole_drawChar;
    s_con.base.scrollWindow = drakoConsole_scrollWindow;
    s_con.base.flushAndSwap = drakoConsole_flushAndSwap;
    s_con.device = device;
    return &s_con.base;
}
