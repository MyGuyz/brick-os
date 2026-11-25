"""Patch PPSSPP's SDL Vulkan context for the Brick: TrimUI's SDL2 'mali' video driver has no window
system, so Vulkan presents straight to the panel through VK_KHR_display (the PowerVR driver has it).
Run from the ppsspp source root."""
import sys

# GCC 8 (the toolchain's) cannot deduce IndependentTask's template from a move-capturing lambda: add a maker.
q = 'Common/Thread/Promise.h'
t = open(q, encoding='utf-8').read()
if 'MakeIndependentTask' not in t:
    t = t.replace('''	TaskPriority prio_;
};''', '''	TaskPriority prio_;
};

template <class T>
IndependentTask<T> *MakeIndependentTask(TaskType type, TaskPriority prio, T func) {   // Brick: GCC 8
	return new IndependentTask<T>(type, prio, std::move(func));
}''', 1)
    open(q, 'w', encoding='utf-8').write(t)
    r = 'Core/Screenshot.cpp'
    u = open(r, encoding='utf-8').read()
    u = u.replace('new IndependentTask(TaskType::IO_BLOCKING', 'MakeIndependentTask(TaskType::IO_BLOCKING', 1)
    open(r, 'w', encoding='utf-8').write(u)
    print('patched', q, r)

# The panel shows the screen through each pixel's alpha after a cold boot (an alpha-0 pixel is black;
# VK_KHR_display offers only OPAQUE, which the driver does not apply). PSP frames carry alpha 0, so the
# picture was mostly black. Last thing every frame: write alpha = 1 over the whole backbuffer.
n = 'UI/NativeApp.cpp'
t = open(n, encoding='utf-8').read()
if 'opaquePipeline' not in t:
    def nrep(old, new):
        global t
        if old not in t:
            sys.exit('pattern not found: ' + old[:60])
        t = t.replace(old, new, 1)
    nrep('static Draw::Pipeline *texColorPipeline;', '''static Draw::Pipeline *texColorPipeline;
static Draw::Pipeline *opaquePipeline;   // Brick: alpha = 1 over the backbuffer''')
    nrep('''	texColorPipeline = g_draw->CreateGraphicsPipeline(texColorDesc, "global_texcolor");''', '''	BlendState *blendAlphaOnly = g_draw->CreateBlendState({ false, 0x8 });   // Brick: writes alpha only
	PipelineDesc opaqueDesc{
		Primitive::TRIANGLE_LIST,
		{ vs_color_2d, fs_color_2d },
		inputLayout, depth, blendAlphaOnly, rasterNoCull, &vsColBufDesc,
	};
	opaquePipeline = g_draw->CreateGraphicsPipeline(opaqueDesc, "brick_opaque");
	blendAlphaOnly->Release();

	texColorPipeline = g_draw->CreateGraphicsPipeline(texColorDesc, "global_texcolor");''')
    nrep('''	if (texColorPipeline) {
		texColorPipeline->Release();
		texColorPipeline = nullptr;
	}''', '''	if (texColorPipeline) {
		texColorPipeline->Release();
		texColorPipeline = nullptr;
	}
	if (opaquePipeline) {
		opaquePipeline->Release();
		opaquePipeline = nullptr;
	}''')
    nrep('''	ui_draw2d.PopDrawMatrix();

	g_draw->EndFrame();''', '''	if (opaquePipeline) {   // Brick: the panel blends by pixel alpha; make every pixel opaque
		ui_draw2d.Flush();
		g_draw->SetViewport({ 0.0f, 0.0f, (float)g_display.pixel_xres, (float)g_display.pixel_yres, 0.0f, 1.0f });
		g_draw->SetScissorRect(0, 0, g_display.pixel_xres, g_display.pixel_yres);
		ui_draw2d.Begin(opaquePipeline);
		ui_draw2d.Rect(0.0f, 0.0f, (float)g_display.dp_xres, (float)g_display.dp_yres, 0xFFFFFFFF);
		ui_draw2d.Flush();
	}

	ui_draw2d.PopDrawMatrix();

	g_draw->EndFrame();''')
    open(n, 'w', encoding='utf-8').write(t)
    print('patched', n)

# Shared with the PSP core (NX_PPSSPP_ASSETS): the assets ship once. Our own assets/ dir only holds what the core lacks.
n = 'UI/NativeApp.cpp'
t = open(n, encoding='utf-8').read()
if 'NX_PPSSPP_ASSETS' not in t:
    for old, new in (('''	g_VFS.Register("", new DirectoryReader(File::GetExeDirectory()));
''', '''	if (const char *nx = getenv("NX_PPSSPP_ASSETS"))   // Brick
		g_VFS.Register("", new DirectoryReader(Path(nx)));
	g_VFS.Register("", new DirectoryReader(File::GetExeDirectory()));
'''), ('''	g_Config.flash0Directory = File::GetExeDirectory() / "assets/flash0";
''', '''	g_Config.flash0Directory = File::GetExeDirectory() / "assets/flash0";
	if (const char *nx = getenv("NX_PPSSPP_ASSETS"))   // Brick
		g_Config.flash0Directory = Path(nx) / "flash0";
''')):
        if old not in t:
            sys.exit('pattern not found: ' + old[:60])
        t = t.replace(old, new, 1)
    open(n, 'w', encoding='utf-8').write(t)
    print('patched assets', n)

p = 'SDL/SDLVulkanGraphicsContext.cpp'
s = open(p, encoding='utf-8').read()
if 'Brick:' in s:
    print('already patched')
    sys.exit(0)

def rep(old, new):
    global s
    if old not in s:
        sys.exit('pattern not found: ' + old[:60])
    s = s.replace(old, new, 1)

# 1. A window without SDL_WINDOW_VULKAN when the driver refuses one (Vulkan never draws into it).
rep('''	window = SDL_CreateWindow("Initializing Vulkan...", x, y, w, h, mode);
	if (!window) {''', '''	window = SDL_CreateWindow("Initializing Vulkan...", x, y, w, h, mode);
	if (!window)   // Brick: the mali driver has no Vulkan windows; VK_KHR_display presents instead
		window = SDL_CreateWindow("Initializing Vulkan...", x, y, w, h, mode & ~SDL_WINDOW_VULKAN);
	if (!window) {''')

# 2. Draw size: the window's own size when SDL knows nothing about Vulkan.
rep('''		SDL_Vulkan_GetDrawableSize(window, &w, &h);
		return''', '''		SDL_Vulkan_GetDrawableSize(window, &w, &h);
		if (w <= 1 || h <= 1) SDL_GetWindowSize(window, &w, &h);   // Brick
		return''')

# 3. No WM info or an unknown window system: present through VK_KHR_display.
rep('''	if (!SDL_GetWindowWMInfo(window, &sys_info)) {
		fprintf(stderr, "Error getting SDL window wm info: %s\\n", SDL_GetError());
		exit(1);
	}''', '''	if (!SDL_GetWindowWMInfo(window, &sys_info)) {   // Brick: the mali driver has none
		fprintf(stderr, "No SDL window wm info (%s): Vulkan presents through VK_KHR_display\\n", SDL_GetError());
		sys_info.subsystem = SDL_SYSWM_UNKNOWN;
	}''')
rep('''	default:
		fprintf(stderr, "Vulkan subsystem %d not supported\\n", sys_info.subsystem);
		exit(1);
		break;''', '''	default:
#if defined(VK_USE_PLATFORM_DISPLAY_KHR)
		vulkan_->InitSurface(WINDOWSYSTEM_DISPLAY, nullptr, nullptr);   // Brick
#else
		fprintf(stderr, "Vulkan subsystem %d not supported\\n", sys_info.subsystem);
		exit(1);
#endif
		break;''')

open(p, 'w', encoding='utf-8').write(s)
print('patched', p)
