#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cmath>
#include <dlfcn.h>
#include <pthread.h>
#include <cerrno>
#include <signal.h>
#include <ucontext.h>
#include <map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#include <wchar.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <GLES2/gl2ext.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/vfs.h>
#include <cxxabi.h>
#include <fnmatch.h>
#include <unwind.h>
#include <zlib.h>

// Подключение stb_truetype
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
// Подключение stb_image
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
// Подключение stb_image_write
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
//
#include "sqlite3.h"
#include <setjmp.h>
#include <dirent.h>
#include <sched.h>

// [ ! ] Если не получается достать информацию о краше через перехват ANR, JNI, Java крашей через LogFox а damn32_log.txt обрывается не успев вывести информацию о краше (имя функции, адреса, регистры и т.д.) то разрешаю использовать root и logcat для вывода информации в damn32_log.txt таким обходным путем, ОДНАКО, полагатся на чтение информации исключительно из logcat ЗАПРЕЩАЮ, мне очень неудобно читать logcat потому напрямую использовать его для логгирования запрещаю.

std::string GetNSString(void* nsstr);
extern bool g_disableLogging;
extern bool g_logRender, g_logSound, g_logFs, g_logNet, g_logTodo, g_logRenderDebug, g_logFuncList, g_logHiddenClasses, g_logOther;
int g_spamFiltersMask = 0;
int g_gpuOffloadMask = 0;
void _LogToJava(const std::string& msg);
#define LogToJava(msg) do { if (!g_disableLogging) _LogToJava(msg); } while(0)
void _LogToBlackBox(const std::string& msg);
#define LogToBlackBox(msg) do { if (!g_disableLogging) _LogToBlackBox(msg); } while(0)
void _SyncLog(const std::string& msg);
#define SyncLog(msg) do { if (!g_disableLogging) _SyncLog(msg); } while(0)
std::string DumpHexToString(const char* data, int max_len);

struct HLE_Method { const char* name; const char* types; void* imp; };
// Переименовали ifa_dstaddr, так как в Android NDK это макрос
struct ifaddrs_dummy { void* ifa_next; char* ifa_name; unsigned int ifa_flags; void* ifa_addr; void* ifa_netmask; void* dummy_ifa_dstaddr; void* ifa_data; };

extern "C" {
    const char* wrap_sel_getName(void* sel) { return (const char*)sel; }
    void* wrap_sel_registerName(const char* name) { return (void*)name; }
    void* wrap_NSSelectorFromString(void* nsstr) { return (void*)strdup(GetNSString(nsstr).c_str()); }
    char* wrap_realpath_darwin(const char* path, char* resolved_path) { return realpath(path, resolved_path); }

    __attribute__((naked)) int wrap_setjmp(void* env) {
        asm volatile (
            "stmia r0!, {r4-r11}\n"        // Сохраняем r4-r11
            "mov r1, sp\n"                 // Копируем SP во временный r1
            "stmia r0!, {r1, lr}\n"        // Сохраняем SP и LR
            "vstmia r0!, {d8-d15}\n"       // Сохраняем FPU регистры
            "mov r0, #0\n"
            "bx lr\n"
        );
    }

    __attribute__((naked)) void wrap_longjmp(void* env, int val) {
        asm volatile (
            "ldmia r0!, {r4-r11}\n"        // Восстанавливаем r4-r11
            "ldmia r0!, {r2, lr}\n"        // Загружаем сохраненный SP в r2, и восстанавливаем LR
            "mov sp, r2\n"                 // Восстанавливаем SP из r2
            "vldmia r0!, {d8-d15}\n"       // Восстанавливаем FPU регистры
            "mov r0, r1\n"                 // Возвращаем val
            "cmp r0, #0\n"
            "it eq\n"
            "moveq r0, #1\n"
            "bx lr\n"
        );
    }
}
// -------------------------------

pthread_t g_iosMainThread;
struct MainQueueItem { void* target; const char* sel; void* arg; void* arg2; };
std::vector<MainQueueItem> g_mainQueue;
pthread_mutex_t g_mainQueueMutex = PTHREAD_MUTEX_INITIALIZER;

std::string SimpleHTTP(const std::string& method) {
    // Симуляция отсутствия сети: блокируем реальный HTTP запрос, 
    // чтобы избежать зависаний в gethostbyname или read на уровне Android.
    return "";
}

#define LOG_TAG "DamnWrapper32_CPP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "ERROR: " __VA_ARGS__)

JavaVM* g_jvm = nullptr; jobject g_mainActivity = nullptr; std::string g_workDir;
char g_crashLogPath[1024] = {0}; volatile int g_crash_counter = 0;
EGLDisplay g_eglDisplay = EGL_NO_DISPLAY; EGLContext g_eglContext = EGL_NO_CONTEXT; EGLSurface g_eglSurface = EGL_NO_SURFACE;
void* g_currentEAGLContext = nullptr;
int g_surfaceWidth = 480; int g_surfaceHeight = 320;
uint32_t g_entryPoint = 0; uint32_t g_appSlide = 0; std::map<std::string, uint32_t> g_appSymbols;
std::map<uintptr_t, std::string> g_missingSymbolAddrs;
std::vector<uint32_t> g_initFuncs;

struct MachOSectionInfo { std::string name; uint32_t start; uint32_t end; };
std::vector<MachOSectionInfo> g_machoSections;
// Адреса и размеры секций __nl_symbol_ptr / __la_symbol_ptr (post-slide).
// Нужны для post-init патча слотов данных (см. фикс _AVAudioSessionCategory* ниже).
struct DysymSlotRange { uint32_t addr; uint32_t size; };
std::vector<DysymSlotRange> g_dysymSections;

struct HLEClass { uint32_t magic; const char* className; };
std::map<std::string, HLEClass*> g_hleClasses;

void* g_displayLinkTarget = nullptr; const char* g_displayLinkSelector = nullptr; bool g_renderingStarted = false;
bool g_frameHasDraw = false;
bool g_onScreenDebugOverlay = false;
bool g_showPerfOverlay = false;
int g_esModeOption = 2;

// Variables for FPS calculation
uint64_t g_fpsLastTimeMs = 0;
int g_fpsFrameCount = 0;
int g_currentFpsInt = 0;
int g_currentFpsFrac = 0;
int g_avgFpsInt = 0;
int g_avgFpsFrac = 0;
long long g_totalFrames = 0;
uint64_t g_startTimeMs = 0;
pthread_mutex_t g_fpsMutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t GetRealTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}
int g_gameViewportW = 0;
int g_gameViewportH = 0;
bool g_isFakeViewport = false;
int g_activeESVersion = 2;
int g_debugHeartbeat = 0;
int g_lastActiveFBO = 0;
std::map<GLuint, GLuint> g_fboColorTex;
std::map<GLuint, std::vector<float>> g_fboDepthBuf;
std::map<GLuint, bool> g_fboTextures;

// Структура кэша текстур текста
struct UITextCache {
    GLuint texture = 0;
    int width = 0;
    int height = 0;
    std::string text = "";
    float pixelHeight = 0;
    std::vector<unsigned char> bitmap;
};

// --- RENDERER STATE ---
ANativeWindow* g_nativeWindow = nullptr;
std::vector<uint32_t> g_cpuColorBuffer;
std::vector<float> g_cpuDepthBuffer;
float g_cpuClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
std::map<GLuint, std::map<std::string, GLuint>> g_attribLocations;

extern std::map<GLuint, std::vector<uint32_t>> g_cpuTextures;
extern std::map<GLuint, int> g_cpuTexW;
extern std::map<GLuint, int> g_cpuTexH;
extern GLuint g_cpuActiveTexture;

// --- ES 1.1 FFP CONSTANTS (Missing in GLES2) ---
#ifndef GL_MODELVIEW
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_SMOOTH 0x1D01
#define GL_MATRIX_PALETTE_OES 0x8840
#define GL_MATRIX_INDEX_ARRAY_OES 0x8B44
#define GL_WEIGHT_ARRAY_OES 0x8B46
#endif

// --- ES 1.1 FFP RENDER STATE ---
GLuint g_currentPaletteMatrix = 0;
const GLvoid* g_matrixIndexPointer = nullptr;
GLint g_matrixIndexSize = 0;
GLenum g_matrixIndexType = GL_UNSIGNED_BYTE;
GLsizei g_matrixIndexStride = 0;
GLuint g_matrixIndexVBO = 0;
const GLvoid* g_weightPointer = nullptr;
GLint g_weightSize = 0;
GLenum g_weightType = GL_FLOAT;
GLsizei g_weightStride = 0;
GLuint g_weightVBO = 0;
const GLvoid* g_pointSizePointer = nullptr;
GLenum g_pointSizeType = GL_FLOAT;
GLsizei g_pointSizeStride = 0;

GLenum g_matrixMode = GL_MODELVIEW;
std::vector<std::vector<float>> g_modelViewStack = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
std::vector<std::vector<float>> g_projectionStack = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
std::vector<std::vector<float>> g_textureStack = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
std::vector<std::vector<std::vector<float>>> g_paletteStacks(32, std::vector<std::vector<float>>(1, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}));

GLenum g_alphaFunc = GL_ALWAYS;
GLclampf g_alphaRef = 0.0f;
GLenum g_shadeModel = GL_SMOOTH;
bool g_blendEnabled = false;
GLenum g_depthFunc = 0x0201; // GL_LESS
GLenum g_blendSrc = GL_ONE;
GLenum g_blendDst = GL_ZERO;
bool g_depthTestEnabled = false;
bool g_depthMask = true;
bool g_colorMask[4] = {true, true, true, true};
bool g_texture2DEnabled = false;
bool g_cullFaceEnabled = false;
GLenum g_cullFaceMode = 0x0405; // GL_BACK
GLenum g_frontFace = 0x0901; // GL_CCW
bool g_matrixPaletteEnabled = false;
bool g_matrixIndexEnabled = false;
bool g_weightEnabled = false;
float g_pointSize = 1.0f;

std::vector<std::vector<float>>& GetCurrentMatrixStack() {
    if (g_matrixMode == GL_PROJECTION) return g_projectionStack;
    if (g_matrixMode == GL_TEXTURE) return g_textureStack;
    if (g_matrixMode == GL_MATRIX_PALETTE_OES) {
        if (g_currentPaletteMatrix < 32) return g_paletteStacks[g_currentPaletteMatrix];
    }
    return g_modelViewStack;
}

// --- Shadow VBO Storage for Render ---
std::map<GLuint, std::vector<uint8_t>> g_vboShadow;
GLuint g_currentVBO = 0;

struct CGPoint { float x, y; };
struct CGSize { float width, height; };
struct CGRect { CGPoint origin; CGSize size; };
struct CGAffineTransform { float a, b, c, d, tx, ty; };

static const CGRect wrap_CGRectZero = {{0.0f, 0.0f}, {0.0f, 0.0f}};
static const CGRect wrap_CGRectNull = {{INFINITY, INFINITY}, {0.0f, 0.0f}};
static const CGSize wrap_CGSizeZero = {0.0f, 0.0f};
static const CGPoint wrap_CGPointZero = {0.0f, 0.0f};
static const CGAffineTransform wrap_CGAffineTransformIdentity = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

struct CGPathElement { int type; float points[6]; };
struct CGState {
    CGAffineTransform ctm;
    float fillColor[4];
    float strokeColor[4];
    float lineWidth;
};

struct HLE_CGContext {
    int width; int height; int bpp; void* data;
    std::vector<CGState> stateStack;
    CGState currentState;
    std::vector<CGPathElement> currentPath;
    float currentX, currentY;
    
    HLE_CGContext() {
        currentState.ctm = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        currentState.fillColor[0] = 0; currentState.fillColor[1] = 0; currentState.fillColor[2] = 0; currentState.fillColor[3] = 1;
        currentState.strokeColor[0] = 0; currentState.strokeColor[1] = 0; currentState.strokeColor[2] = 0; currentState.strokeColor[3] = 1;
        currentState.lineWidth = 1.0f;
        currentX = 0.0f; currentY = 0.0f;
    }
};

struct HLE_CGImage { int width; int height; int bpp; void* data; };
struct HLE_CGColor { float components[4]; };

void CPUDrawPixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < g_surfaceWidth && y >= 0 && y < g_surfaceHeight) {
        if (g_cpuColorBuffer.size() == (size_t)(g_surfaceWidth * g_surfaceHeight)) {
            g_cpuColorBuffer[y * g_surfaceWidth + x] = color;
        }
    }
}
void CPUDrawLine(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        CPUDrawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void CPUDrawText(float rx, float ry, float rw, float rh, float r, float g, float b, const UITextCache& cache) {
    if (cache.bitmap.empty() || g_cpuColorBuffer.size() != (size_t)(g_surfaceWidth * g_surfaceHeight)) return;
    uint8_t cr = (uint8_t)(r * 255.0f); uint8_t cg = (uint8_t)(g * 255.0f); uint8_t cb = (uint8_t)(b * 255.0f);
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    
    int px0 = (int)(rx * scaleX); int py0 = (int)(ry * scaleY);
    int px1 = (int)((rx + rw) * scaleX); int py1 = (int)((ry + rh) * scaleY);
    
    for (int y = py0; y < py1; y++) {
        for (int x = px0; x < px1; x++) {
            if (x >= 0 && x < g_surfaceWidth && y >= 0 && y < g_surfaceHeight) {
                float u = (float)(x - px0) / (px1 - px0);
                float v = (float)(y - py0) / (py1 - py0);
                int tx = (int)(u * cache.width);
                int ty = (int)(v * cache.height);
                if (tx >= 0 && tx < cache.width && ty >= 0 && ty < cache.height) {
                    uint8_t alpha = cache.bitmap[ty * cache.width + tx];
                    if (alpha > 0) {
                        uint32_t bg = g_cpuColorBuffer[y * g_surfaceWidth + x];
                        uint8_t b_bg = (bg >> 16) & 0xFF; uint8_t g_bg = (bg >> 8) & 0xFF; uint8_t r_bg = bg & 0xFF;
                        float srcA = alpha / 255.0f; float invA = 1.0f - srcA;
                        uint8_t finalR = (uint8_t)(cr * srcA + r_bg * invA);
                        uint8_t finalG = (uint8_t)(cg * srcA + g_bg * invA);
                        uint8_t finalB = (uint8_t)(cb * srcA + b_bg * invA);
                        g_cpuColorBuffer[y * g_surfaceWidth + x] = 0xFF000000 | (finalB << 16) | (finalG << 8) | finalR;
                    }
                }
            }
        }
    }
}

void CPUDrawImage(float rx, float ry, float rw, float rh, void* imgPtr, float alpha) {
    if (!imgPtr || g_cpuColorBuffer.size() != (size_t)(g_surfaceWidth * g_surfaceHeight)) return;
    HLE_CGImage* img = (HLE_CGImage*)imgPtr;
    if (!img->data) return;
    int px0 = (int)rx; int py0 = (int)ry;
    int px1 = (int)(rx + rw); int py1 = (int)(ry + rh);
    for (int y = py0; y < py1; y++) {
        for (int x = px0; x < px1; x++) {
            if (x >= 0 && x < g_surfaceWidth && y >= 0 && y < g_surfaceHeight) {
                float u = (float)(x - px0) / (px1 - px0);
                float v = (float)(y - py0) / (py1 - py0);
                int tx = (int)(u * img->width); int ty = (int)(v * img->height);
                if (tx >= 0 && tx < img->width && ty >= 0 && ty < img->height) {
                    uint32_t srcColor = ((uint32_t*)img->data)[ty * img->width + tx];
                    uint8_t sr = srcColor & 0xFF; uint8_t sg = (srcColor >> 8) & 0xFF; uint8_t sb = (srcColor >> 16) & 0xFF; uint8_t sa = (srcColor >> 24) & 0xFF;
                    float srcA = (sa / 255.0f) * alpha;
                    if (srcA > 0.0f) {
                        uint32_t bg = g_cpuColorBuffer[y * g_surfaceWidth + x];
                        uint8_t br = bg & 0xFF; uint8_t bg_g = (bg >> 8) & 0xFF; uint8_t bb = (bg >> 16) & 0xFF;
                        float invA = 1.0f - srcA;
                        uint8_t finalR = (uint8_t)(sr * srcA + br * invA);
                        uint8_t finalG = (uint8_t)(sg * srcA + bg_g * invA);
                        uint8_t finalB = (uint8_t)(sb * srcA + bb * invA);
                        g_cpuColorBuffer[y * g_surfaceWidth + x] = 0xFF000000 | (finalB << 16) | (finalG << 8) | finalR;
                    }
                }
            }
        }
    }
}

void CPUDrawSolidRect(float rx, float ry, float rw, float rh, float r, float g, float b, float a, float cornerRadius = 0.0f) {
    if (g_cpuColorBuffer.size() != (size_t)(g_surfaceWidth * g_surfaceHeight)) return;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    int px0 = (int)(rx * scaleX); int py0 = (int)(ry * scaleY);
    int px1 = (int)((rx + rw) * scaleX); int py1 = (int)((ry + rh) * scaleY);
    if (px0 < 0) px0 = 0; if (py0 < 0) py0 = 0;
    if (px1 > g_surfaceWidth) px1 = g_surfaceWidth; if (py1 > g_surfaceHeight) py1 = g_surfaceHeight;

    float scaledRadius = cornerRadius * scaleX;
    float hw = (rw / 2.0f) * scaleX;
    float hh = (rh / 2.0f) * scaleY;
    if (scaledRadius > hw) scaledRadius = hw;
    if (scaledRadius > hh) scaledRadius = hh;
    float cx = (rx + rw / 2.0f) * scaleX;
    float cy = (ry + rh / 2.0f) * scaleY;
    float inner_w = hw - scaledRadius;
    float inner_h = hh - scaledRadius;
    float rSq = scaledRadius * scaledRadius;

    for (int y = py0; y < py1; y++) {
        for (int x = px0; x < px1; x++) {
            if (cornerRadius > 0.0f) {
                float dx = std::abs(x - cx);
                float dy = std::abs(y - cy);
                if (dx > inner_w && dy > inner_h) {
                    float dist_x = dx - inner_w;
                    float dist_y = dy - inner_h;
                    if (dist_x * dist_x + dist_y * dist_y > rSq) continue;
                }
            }
            if (a >= 1.0f) {
                uint8_t cr = (uint8_t)(r * 255.0f); uint8_t cg = (uint8_t)(g * 255.0f); uint8_t cb = (uint8_t)(b * 255.0f);
                g_cpuColorBuffer[y * g_surfaceWidth + x] = 0xFF000000 | (cb << 16) | (cg << 8) | cr;
            } else if (a > 0.0f) {
                uint32_t bg = g_cpuColorBuffer[y * g_surfaceWidth + x];
                uint8_t rb = bg & 0xFF, gb = (bg >> 8) & 0xFF, bb = (bg >> 16) & 0xFF;
                uint8_t rs = (uint8_t)(r * 255.0f), gs = (uint8_t)(g * 255.0f), bs = (uint8_t)(b * 255.0f);
                uint8_t rf = (uint8_t)(rs * a + rb * (1.0f - a));
                uint8_t gf = (uint8_t)(gs * a + gb * (1.0f - a));
                uint8_t bf = (uint8_t)(bs * a + bb * (1.0f - a));
                g_cpuColorBuffer[y * g_surfaceWidth + x] = 0xFF000000 | (bf << 16) | (gf << 8) | rf;
            }
        }
    }
}
// --------------------------

// UI & Touch HLE State
struct ButtonHit { void* button; void* target; std::string action; };
std::vector<ButtonHit> g_buttons;

struct ObserverInfo { void* observer; std::string selector; void* object; };
std::map<std::string, std::vector<ObserverInfo>> g_notifications;
void* hle_MPMoviePlayerPlaybackDidFinishNotification_ptr = nullptr;
void* hle_MPMoviePlayerContentPreloadDidFinishNotification_ptr = nullptr;
void* hle_MPMoviePlayerLoadStateDidChangeNotification_ptr = nullptr;
void* g_appDelegateInstance = nullptr;
std::vector<void*> g_pendingVideoFinishes;
pthread_mutex_t g_videoMutex = PTHREAD_MUTEX_INITIALIZER;

struct HLEColor { float r, g, b, a; };
struct HLEUIView {
    std::vector<float> frame = {0,0,0,0}; void* parent = nullptr; std::vector<void*> children;
    std::string type = ""; std::string text = "";
    HLEColor bgColor = {0,0,0,0}; bool hasBg = false;
    HLEColor textColor = {0,0,0,0}; bool hasTextCol = false;
    float cornerRadius = 0.0f; bool opaque = false; float alpha = 1.0f;
    bool hidden = false; bool userInteraction = true; int textAlignment = -1;
    bool buttonPressed = false; bool switchState = false; bool textViewEditable = false;
    bool exclusiveTouch = false; int lineBreakMode = 0; void* font = nullptr; void* uiColorObj = nullptr;
};
std::map<void*, HLEUIView> g_views;
std::map<void*, void*> g_viewControllersViews;
std::map<void*, HLEColor> g_uiColors;
std::map<void*, void*> g_layerDrawableProperties;
std::map<int, void*> g_pointerToUI;

void* g_mainView = nullptr;
void* g_presentedView = nullptr;
void* g_accelerometerDelegate = nullptr;

// File System & Foundation HLE State
std::string g_sandboxDir;
std::string g_appBundlePath;
std::string g_execPath;
std::map<void*, std::vector<void*>> g_arrays;
std::map<void*, std::map<std::string, void*>> g_dictionariesHLE;
std::map<std::string, void*> g_userDefaults; // Простейшее хранилище UserDefaults

void LoadUserDefaults();
void SaveUserDefaults();

struct NSFastEnumerationState { unsigned long state; void** itemsPtr; unsigned long* mutationsPtr; unsigned long extra[5]; };
struct FakeUITouch { uint32_t isa; const char* className; float x; float y; void* view; uint32_t touchId; };
struct FakeNSSet { uint32_t isa; const char* className; std::vector<void*> touches; };
struct FakeUIAcceleration { uint32_t isa; const char* className; double timestamp; double x; double y; double z; };
std::map<int, FakeUITouch*> g_activeTouches;
double g_latestAccelX = 0.0;
double g_latestAccelY = 0.0;
double g_latestAccelZ = 0.0;
std::map<void*, std::map<void*, void*>> g_dictionaries;
int g_ignoringInteractionEvents = 0;

// Глобальные переменные для шрифтов
std::vector<unsigned char> g_fontBuffer;
stbtt_fontinfo g_fontInfo;
bool g_fontLoaded = false;

std::map<void*, UITextCache> g_uiTextCache;

void UpdateTextCache(void* view, const std::string& text, float logicalH);

bool isValidString(const char* str) {
    if (!str || (uintptr_t)str < 0x1000) return false;
    for (int i = 0; i < 10000; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == 0) return true;
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') return false;
    }
    return false;
}

void* ResolveSymbol(const std::string& name);

// Утилита для создания HLE NSString на лету
void* CreateNSString(const std::string& str) {
    if (str.find("pngConf") != std::string::npos || str.find("jungle") != std::string::npos) {
        LogToJava("STR-DEBUG: [CreateNSString] Создается строка: [" + str + "] (len=" + std::to_string(str.length()) + ")");
        LogToJava("STR-DEBUG: [CreateNSString] HEX DUMP: " + DumpHexToString(str.c_str(), str.length() + 4));
    }
    uint32_t* inst = (uint32_t*)calloc(1, 32);
    inst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSString");
    char* c_str = strdup(str.c_str());
    inst[2] = (uint32_t)c_str;
    return inst;
}

// FIX: Проверяет, попадает ли адрес в одну из маппированных секций бинаря ИЛИ в heap.
// Используется в GetNSString перед разыменованием c_str_ptr, чтобы не словить SIGSEGV
// когда гость передаёт кривой "NSString" (например, &переменная вместо *переменная).
static bool IsMappedReadable(uint32_t addr) {
    if (addr < 0x1000) return false;
    // Heap аллокатора (scudo/malloc) — обычно выше 0x80000000 на Android ARMv7
    // Это быстрый путь для наших CreateNSString() объектов.
    if (addr >= 0x80000000u) return true;
    for (const auto& sInfo : g_machoSections) {
        if (addr >= sInfo.start && addr < sInfo.end) return true;
    }
    return false;
}

std::string GetNSString(void* nsstr) {
    if (!nsstr || (uintptr_t)nsstr < 0x1000) return "";
    uint32_t* ptr = (uint32_t*)nsstr;
    uint32_t isa = ptr[0];
    
    std::string result = "Unknown";
    
    // СНАЧАЛА проверяем, является ли это валидным Objective-C объектом (NSString/CFString).
    // Это предотвратит парсинг байтов указателя 'isa' как сырой C-строки!
    if (isa > 0x1000 && (isa % 4 == 0)) {
        uint32_t c_str_ptr = ptr[2];
        // FIX: Проверяем маппинг c_str_ptr ПЕРЕД разыменованием.
        // Без этой проверки, если гость передаёт &variable вместо *variable
        // (одинарный deref вместо двойного для nl_symbol_ptr), ptr[2] попадает
        // в случайную BSS-память и isValidString падает с SIGSEGV (fault addr R5).
        if (IsMappedReadable(c_str_ptr) && isValidString((const char*)c_str_ptr)) {
            result = std::string((const char*)c_str_ptr);
        } else {
            uint32_t c_str_ptr3 = ptr[3];
            if (IsMappedReadable(c_str_ptr3) && isValidString((const char*)c_str_ptr3)) {
                result = std::string((const char*)c_str_ptr3);
            }
        }
        // FIX: Если объект не похож на валидный NSString (ptr[2]/ptr[3] кривые),
        // но сам указатель nsstr является указателем на void* (т.е. гость передал
        // &hle_SomeNSString_ptr вместо hle_SomeNSString_ptr), пробуем разыменовать.
        if (result == "Unknown") {
            uint32_t deref = ptr[0]; // ptr[0] в этом случае = само значение NSString*
            // Если ptr[0] — это адрес heap-объекта (наш CreateNSString), рекурсируем.
            // Защита от бесконечной рекурсии: deref != (uint32_t)(uintptr_t)nsstr
            if (IsMappedReadable(deref) && deref != (uint32_t)(uintptr_t)nsstr) {
                uint32_t* inner = (uint32_t*)deref;
                uint32_t inner_isa = inner[0];
                if (inner_isa > 0x1000 && (inner_isa % 4 == 0)) {
                    uint32_t inner_cstr = inner[2];
                    if (IsMappedReadable(inner_cstr) && isValidString((const char*)inner_cstr)) {
                        LogToJava("STR-FIX: [GetNSString] Обнаружен двойной указатель (nl_symbol_ptr одинарный deref), раскрываем: " + std::to_string(deref));
                        result = std::string((const char*)inner_cstr);
                    }
                }
            }
        }
    }

    // ФОЛБЕК: Только если это не похоже на строку внутри объекта (например, игра 
    // криво передала сырой C-String туда, где ожидался NSString), читаем напрямую.
    if (result == "Unknown" && IsMappedReadable((uint32_t)(uintptr_t)nsstr) && isValidString((const char*)nsstr)) {
        result = std::string((const char*)nsstr);
    }
    
    if (result.find("pngConf") != std::string::npos || result.find("jungle") != std::string::npos) {
        LogToJava("STR-DEBUG: [GetNSString] Вытащили строку: [" + result + "] len=" + std::to_string(result.length()));
        LogToJava("STR-DEBUG: [GetNSString] HEX: " + DumpHexToString(result.c_str(), result.length() + 4));
    }
    
    return result;
}

extern "C" void* Stub_NSHomeDirectory() {
    return CreateNSString(g_sandboxDir);
}

extern "C" void* Stub_NSTemporaryDirectory() {
    return CreateNSString(g_sandboxDir + "tmp/");
}

extern "C" void* Stub_NSSearchPathForDirectoriesInDomains(uint32_t directory, uint32_t domainMask, uint32_t expandTilde) {
    std::string path = g_sandboxDir;
    if (directory == 9) path += "Documents"; // NSDocumentDirectory
    else if (directory == 13) path += "Library/Caches"; // NSCachesDirectory
    else path += "Documents";
    
    uint32_t* arrayInst = (uint32_t*)calloc(1, 32);
    arrayInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
    
    void* nsPath = CreateNSString(path);
    g_arrays[arrayInst].push_back(nsPath);
    return arrayInst;
}

inline bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string MaskPointers(const std::string& str) {
    std::string res = str;
    size_t pos = 0;
    
    // 1. Маскируем hex указатели (0x...)
    while ((pos = res.find("0x", pos)) != std::string::npos) {
        size_t end = pos + 2;
        while (end < res.length() && is_hex_char(res[end])) end++;
        if (end > pos + 2) {
            res.replace(pos, end - pos, "0x...");
            pos += 5;
        } else {
            pos += 2;
        }
    }
    
    // 2. Маскируем десятичные указатели и счетчики (this=..., src=..., ref=...)
    auto mask_decimal = [&](const std::string& prefix) {
        size_t p = 0;
        while ((p = res.find(prefix, p)) != std::string::npos) {
            size_t start = p + prefix.length();
            size_t end = start;
            while (end < res.length() && res[end] >= '0' && res[end] <= '9') end++;
            if (end > start) {
                res.replace(start, end - start, "...");
                p = start + 3; // Сдвигаемся за "..."
            } else {
                p += prefix.length();
            }
        }
    };
    
    mask_decimal("this=");
    mask_decimal("src=");
    mask_decimal("ref=");
    
    return res;
}

std::vector<std::string> g_logHistory;
int g_spamCount = 0;
pthread_t g_lastLoggedThread = 0;

bool g_logRender = false;
bool g_logSound = false;
bool g_logFs = false;
bool g_logNet = false;
bool g_logTodo = false;
bool g_logRenderDebug = false;
bool g_logFuncList = false;
bool g_logHiddenClasses = false;
bool g_logOther = false;
bool g_nativeRootMmap = false;
bool g_disableLogging = true;

void InternalWriteLog(const std::string& msg) {
    LOGI("%s", msg.c_str());
    if (!g_workDir.empty()) {
        std::string logPath = g_workDir + "damn32_log.txt";
        int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            std::string out = msg + "\n";
            write(fd, out.c_str(), out.length());
            fsync(fd);
            close(fd);
        }
    }
    if (g_jvm && g_mainActivity) {
        JNIEnv* env; if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) g_jvm->AttachCurrentThread(&env, nullptr);
        jclass clazz = env->GetObjectClass(g_mainActivity); jmethodID method = env->GetMethodID(clazz, "addLogFromNative", "(Ljava/lang/String;)V");
        if (method) { jstring jmsg = env->NewStringUTF(msg.c_str()); env->CallVoidMethod(g_mainActivity, method, jmsg); env->DeleteLocalRef(jmsg); }
        env->DeleteLocalRef(clazz);
    }
}

std::string g_blackBox[1000];
int g_blackBoxIndex = 0;
bool g_blackBoxWrapped = false;
pthread_mutex_t g_blackBoxMutex = PTHREAD_MUTEX_INITIALIZER;

void _LogToBlackBox(const std::string& msg) {
    pthread_mutex_lock(&g_blackBoxMutex);
    g_blackBox[g_blackBoxIndex] = msg;
    g_blackBoxIndex++;
    if (g_blackBoxIndex >= 1000) {
        g_blackBoxIndex = 0;
        g_blackBoxWrapped = true;
    }
    pthread_mutex_unlock(&g_blackBoxMutex);
}

void _LogToJava(const std::string& msg) {
    bool keep = false;
    
    bool isRender = msg.find("[RENDER]") != std::string::npos || msg.find("[GL-TRACE]") != std::string::npos || msg.find("[SHADER-DUMP]") != std::string::npos || msg.find("EGL") != std::string::npos;
    bool isSound = msg.find("HLE_AUDIO") != std::string::npos || msg.find("AL-") != std::string::npos || msg.find("OpenAL") != std::string::npos || msg.find("Audio") != std::string::npos;
    bool isFs = msg.find("C-API-IO") != std::string::npos || msg.find("FOPEN") != std::string::npos || msg.find("fopen") != std::string::npos || msg.find("stat") != std::string::npos;
    bool isNet = msg.find("Reachability") != std::string::npos || msg.find("socket") != std::string::npos || msg.find("bind") != std::string::npos || msg.find("connect") != std::string::npos || msg.find("gethostbyname") != std::string::npos;
    bool isTodo = msg.find("TODO") != std::string::npos || msg.find("Unimplemented") != std::string::npos || msg.find("STUBBED") != std::string::npos || msg.find("STUB") != std::string::npos;
    bool isRenderDebug = msg.find("[ABSOLUTE-DEBUG]") != std::string::npos || msg.find("[MEGA-DEBUG]") != std::string::npos || msg.find("DumpGLState") != std::string::npos;
    bool isFuncList = msg.find("C-API-IMPLEMENTED") != std::string::npos || msg.find("=== РЕАЛИЗОВАННЫЕ") != std::string::npos || msg.find("=== C-API ЗАГЛУШКИ") != std::string::npos || msg.find("==================================") != std::string::npos;
    bool isHiddenClass = msg.find("OBJC-CLASS-FOUND") != std::string::npos;
    bool isOther = !(isRender || isSound || isFs || isNet || isTodo || isRenderDebug || isFuncList || isHiddenClass);
    
    if (isRenderDebug && g_logRenderDebug) keep = true;
    else if (isRender && g_logRender) keep = true;
    else if (isSound && g_logSound) keep = true;
    else if (isFs && g_logFs) keep = true;
    else if (isNet && g_logNet) keep = true;
    else if (isTodo && g_logTodo) keep = true;
    else if (isFuncList && g_logFuncList) keep = true;
    else if (isHiddenClass && g_logHiddenClasses) keep = true;
    else if (g_logOther) keep = true;
    
    if (msg.find("FATAL") != std::string::npos || (msg.find("CRITICAL") != std::string::npos && msg.find("[SIZE-CRITICAL]") == std::string::npos) || msg.find("ERROR") != std::string::npos || msg.find("ОШИБКА") != std::string::npos || msg.find("ASSERT") != std::string::npos) {
        keep = true; // Always log fatals/errors if logging is enabled at all
    }
    
    if (!keep) return;

    std::string masked = MaskPointers(msg);
    bool isRepeat = false;
    
    bool hideSpam = false;
    if (isRender && (g_spamFiltersMask & (1 << 0))) hideSpam = true;
    else if (isSound && (g_spamFiltersMask & (1 << 1))) hideSpam = true;
    else if (isFs && (g_spamFiltersMask & (1 << 2))) hideSpam = true;
    else if (isNet && (g_spamFiltersMask & (1 << 3))) hideSpam = true;
    else if (isTodo && (g_spamFiltersMask & (1 << 4))) hideSpam = true;
    else if (isRenderDebug && (g_spamFiltersMask & (1 << 5))) hideSpam = true;
    else if (isFuncList && (g_spamFiltersMask & (1 << 6))) hideSpam = true;
    else if (isHiddenClass && (g_spamFiltersMask & (1 << 7))) hideSpam = true;
    else if (isOther && (g_spamFiltersMask & (1 << 8))) hideSpam = true;
                             
    if (hideSpam) {
        for (const auto& hist : g_logHistory) {
            if (hist == masked) {
                isRepeat = true;
                break;
            }
        }
    }

    if (isRepeat) {
        g_spamCount++;
        return;
    }

    if (g_spamCount > 0) {
        InternalWriteLog("...");
        g_spamCount = 0;
    }

    g_logHistory.push_back(masked);
    if (g_logHistory.size() > 32) { // 32 строк должно хватить для перекрытия цикла рендера
        g_logHistory.erase(g_logHistory.begin());
    }

    pthread_t currentThread = pthread_self();
    if (currentThread != g_lastLoggedThread) {
        char tid_buf[64];
        snprintf(tid_buf, sizeof(tid_buf), "\n=== [СМЕНА ПОТОКА: T:%ld] ===", (long)currentThread);
        InternalWriteLog(std::string(tid_buf));
        g_lastLoggedThread = currentThread;
    }

    InternalWriteLog(msg);
}

void AddUIElementToJava(void* viewPtr, const std::string& type, const std::string& text) {}
void RemoveUIElementFromJava(void* viewPtr) {}
void RefreshUILayer(void* activeView) {}

void ShowArchErrorToJava(const std::string& arch) {
    if (!g_jvm || !g_mainActivity) return; JNIEnv* env; g_jvm->AttachCurrentThread(&env, nullptr);
    jclass clazz = env->GetObjectClass(g_mainActivity); jmethodID method = env->GetMethodID(clazz, "showArchErrorPopup", "(Ljava/lang/String;)V");
    if (method) { jstring jarch = env->NewStringUTF(arch.c_str()); env->CallVoidMethod(g_mainActivity, method, jarch); env->DeleteLocalRef(jarch); }
    env->DeleteLocalRef(clazz);
}

void ShowDrmErrorToJava() {
    if (!g_jvm || !g_mainActivity) return; JNIEnv* env; g_jvm->AttachCurrentThread(&env, nullptr);
    jclass clazz = env->GetObjectClass(g_mainActivity); jmethodID method = env->GetMethodID(clazz, "showDrmErrorPopup", "()V");
    if (method) { env->CallVoidMethod(g_mainActivity, method); }
    env->DeleteLocalRef(clazz);
}

void SwitchToRenderView() {
    if (!g_jvm || !g_mainActivity) return; JNIEnv* env; g_jvm->AttachCurrentThread(&env, nullptr);
    jclass clazz = env->GetObjectClass(g_mainActivity); jmethodID method = env->GetMethodID(clazz, "switchToRender", "()V");
    if (method) env->CallVoidMethod(g_mainActivity, method); env->DeleteLocalRef(clazz);
}

std::string GetModuleInfoForAddress(uintptr_t addr) {
    for (auto const& pair : g_missingSymbolAddrs) {
        if (addr >= pair.first && addr < pair.first + 64) {
            return "UNIMPLEMENTED_C_API: " + pair.second;
        }
    }
    
    std::string section_info = "";
    for (const auto& sec : g_machoSections) {
        if (addr >= sec.start && addr < sec.end) {
            char sbuf[128];
            snprintf(sbuf, sizeof(sbuf), "[%s + 0x%x] ", sec.name.c_str(), (uint32_t)(addr - sec.start));
            section_info = sbuf;
            break;
        }
    }

    uint32_t best_addr = 0;
    const char* best_name = nullptr;
    for (std::map<std::string, uint32_t>::const_iterator it = g_appSymbols.begin(); it != g_appSymbols.end(); ++it) {
        if (it->second <= addr && it->second > best_addr) {
            best_addr = it->second;
            best_name = it->first.c_str();
        }
    }
    if (best_name && (addr - best_addr) < 0x400000) {
        uint32_t offset = addr - best_addr;
        char buf[512];
        // Если это код, то выводим Nearest. Если это явно секция данных, то Nearest функция не имеет смысла
        if (section_info.find("__DATA") != std::string::npos || section_info.find("__OBJC") != std::string::npos) {
            return section_info;
        }
        
        if (offset > 0x1000) {
            snprintf(buf, sizeof(buf), "%s[Stripped] (Nearest: %s + 0x%x)", section_info.c_str(), best_name, offset);
        } else {
            snprintf(buf, sizeof(buf), "%s%s + 0x%x", section_info.c_str(), best_name, offset);
        }
        return std::string(buf);
    }

    if (!section_info.empty()) return section_info;

    Dl_info info;
    if (dladdr((void*)addr, &info) && info.dli_sname) {
        const char* fname = info.dli_fname ? info.dli_fname : "unknown";
        const char* slash = strrchr(fname, '/');
        if (slash) fname = slash + 1;
        char buf[512];
        unsigned long offset = (unsigned long)(addr - (uintptr_t)info.dli_saddr);
        if (offset > 0x1000) {
            snprintf(buf, sizeof(buf), "[Stripped Function] (Nearest: %s + 0x%lx in %s)", info.dli_sname, offset, fname);
        } else {
            snprintf(buf, sizeof(buf), "%s + 0x%lx in %s", info.dli_sname, offset, fname);
        }
        return std::string(buf);
    }
    std::string result = "Unknown Module";
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            uintptr_t start, end;
            char permissions[5];
            char path[256] = "";
            if (sscanf(line, "%x-%x %4s %*s %*s %255[^\n]", &start, &end, permissions, path) >= 3) {
                if (addr >= start && addr < end) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "%s (Base: 0x%x, Offset: 0x%x, Perm: %s)", 
                             strlen(path) > 0 ? path : "[Anonymous/mmap]", start, addr - start, permissions);
                    result = buf;
                    break;
                }
            }
        }
        fclose(fp);
    }
    return result;
}


void SafeWriteStr(int fd, const char* str) {
    if (!str) return;
    size_t len = 0; while(str[len]) len++;
    write(fd, str, len);
}

void SafeWriteHex(int fd, uint32_t val) {
    char buf[16]; buf[0] = '0'; buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        int nibble = (val >> (i * 4)) & 0xF;
        buf[9 - i] = nibble < 10 ? ('0' + nibble) : ('A' + (nibble - 10));
    }
    write(fd, buf, 10);
}

void SafeWriteModuleInfo(int fd, uintptr_t addr) {
    if (addr == 0) { SafeWriteStr(fd, "NULL"); return; }
    
    uint32_t best_addr = 0;
    const char* best_name = nullptr;
    for (std::map<std::string, uint32_t>::const_iterator it = g_appSymbols.begin(); it != g_appSymbols.end(); ++it) {
        if (it->second <= addr && it->second > best_addr) {
            best_addr = it->second;
            best_name = it->first.c_str();
        }
    }
    if (best_name && (addr - best_addr) < 0x400000) {
        uint32_t offset = addr - best_addr;
        if (offset > 0x1000) {
            SafeWriteStr(fd, "[Stripped Function] (Nearest: ");
            SafeWriteStr(fd, best_name); SafeWriteStr(fd, " + "); SafeWriteHex(fd, offset);
            SafeWriteStr(fd, ")");
        } else {
            SafeWriteStr(fd, best_name); SafeWriteStr(fd, " + "); SafeWriteHex(fd, offset);
        }
        return;
    }
    
    Dl_info info;
    if (dladdr((void*)addr, &info) && info.dli_sname) {
        uint32_t offset = addr - (uintptr_t)info.dli_saddr;
        if (offset > 0x1000) {
            SafeWriteStr(fd, "[Stripped Function] (Nearest: ");
            SafeWriteStr(fd, info.dli_sname); SafeWriteStr(fd, " + "); SafeWriteHex(fd, offset);
        } else {
            SafeWriteStr(fd, info.dli_sname); SafeWriteStr(fd, " + "); SafeWriteHex(fd, offset);
        }
        SafeWriteStr(fd, " in ");
        const char* fname = info.dli_fname ? info.dli_fname : "unknown";
        const char* slash = fname;
        for (const char* p = fname; *p; p++) if (*p == '/') slash = p + 1;
        SafeWriteStr(fd, slash);
        if (offset > 0x1000) SafeWriteStr(fd, ")");
        return;
    }
    SafeWriteStr(fd, "Unknown Module / Heap");
}

// --- SAFE MEMORY PROBING ---
bool SafeRead32(uintptr_t addr, uint32_t* out_val) {
    if (addr < 0x1000 || addr % 4 != 0) return false;
    int fds[2];
    if (pipe(fds) != 0) return false;
    bool ok = (write(fds[1], (void*)addr, 4) == 4);
    if (ok) read(fds[0], out_val, 4);
    close(fds[0]); close(fds[1]);
    return ok;
}

bool SafeReadString(uintptr_t addr, char* out_buf, int max_len) {
    if (addr < 0x1000) return false;
    int fds[2];
    if (pipe(fds) != 0) return false;
    bool ok = (write(fds[1], (void*)addr, max_len) > 0);
    if (ok) {
        int bytes = read(fds[0], out_buf, max_len - 1);
        if (bytes > 0) {
            out_buf[bytes] = '\0';
            for (int i = 0; i < bytes; i++) {
                if (out_buf[i] == '\0') break;
                if ((unsigned char)out_buf[i] < 32 && out_buf[i] != '\n' && out_buf[i] != '\r' && out_buf[i] != '\t') {
                    out_buf[i] = '.'; // Заменяем мусор на точки
                }
            }
            close(fds[0]); close(fds[1]);
            return true;
        }
    }
    close(fds[0]); close(fds[1]);
    return false;
}

void ProbeRegisterSafe(int fd, const char* reg_name, uint32_t val) {
    SafeWriteStr(fd, "\t"); SafeWriteStr(fd, reg_name); SafeWriteStr(fd, ": "); SafeWriteHex(fd, val);
    uint32_t isa = 0;
    if (SafeRead32(val, &isa) && isa > 0x1000) {
        if (isa == 0xDEADBEEF) {
            uint32_t hle_cls_ptr = 0;
            if (SafeRead32(val + 4, &hle_cls_ptr) && hle_cls_ptr > 0x1000) {
                char cls_name[32] = {0};
                if (SafeReadString(hle_cls_ptr, cls_name, 31)) {
                    SafeWriteStr(fd, " -> [HLE Class: "); SafeWriteStr(fd, cls_name); SafeWriteStr(fd, "]");
                }
            }
        } else {
            uint32_t maybe_deadbeef = 0;
            if (SafeRead32(isa, &maybe_deadbeef) && maybe_deadbeef == 0xDEADBEEF) {
                uint32_t hle_cls_ptr = 0;
                if (SafeRead32(isa + 4, &hle_cls_ptr) && hle_cls_ptr > 0x1000) {
                    char cls_name[32] = {0};
                    if (SafeReadString(hle_cls_ptr, cls_name, 31)) {
                        SafeWriteStr(fd, " -> [HLE Instance: "); SafeWriteStr(fd, cls_name); SafeWriteStr(fd, "]");
                    }
                }
            } else {
                uint32_t data_ptr = 0;
                if (SafeRead32(isa + 16, &data_ptr) && data_ptr > 0x1000) {
                    data_ptr &= ~3;
                    uint32_t name_ptr = 0;
                    if (SafeRead32(data_ptr + 16, &name_ptr) && name_ptr > 0x1000) {
                        char cls_name[32] = {0};
                        if (SafeReadString(name_ptr, cls_name, 31)) {
                            SafeWriteStr(fd, " -> [ObjC Class: "); SafeWriteStr(fd, cls_name); SafeWriteStr(fd, "]");
                        }
                    }
                }
            }
        }
    }
    if (val > 0x1000) {
        char str_buf[128] = {0};
        if (SafeReadString(val, str_buf, 127)) {
            bool is_ascii = true; int len = 0;
            for (int i = 0; i < 10 && str_buf[i]; i++) {
                if ((unsigned char)str_buf[i] < 32 || (unsigned char)str_buf[i] > 126) { is_ascii = false; break; }
                len++;
            }
            if (is_ascii && len >= 3) {
                SafeWriteStr(fd, " -> [ASCII: '"); SafeWriteStr(fd, str_buf); SafeWriteStr(fd, "']");
            }
        }
    }
    SafeWriteStr(fd, "\n");
}
// ------------------------------

struct BacktraceState { void** current; void** end; };
static _Unwind_Reason_Code UnwindCallback(struct _Unwind_Context* context, void* arg) {
    BacktraceState* state = static_cast<BacktraceState*>(arg);
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc) {
        if (state->current == state->end) return _URC_END_OF_STACK;
        *state->current++ = reinterpret_cast<void*>(pc);
    }
    return _URC_NO_REASON;
}

void CrashHandler(int sig, siginfo_t *info, void *context) {
    // ФИК SIGSYS: Перехват iOS syscall'ов через svc инструкцию.
    // На iOS ARM syscall номер передаётся в R12 (ip), затем идёт svc 0x80.
    // Android не знает эти syscall'ы и кидает SIGSYS.
    // Мы перехватываем, эмулируем что нужно (большинство — заглушки/0), и возвращаем управление.
#if defined(__arm__)
    if (sig == SIGSYS) {
        ucontext_t *uc_sys = (ucontext_t *)context;
        if (uc_sys) {
            uint32_t syscall_num = uc_sys->uc_mcontext.arm_ip; // R12 = iOS syscall number
            uint32_t r0 = uc_sys->uc_mcontext.arm_r0;
            uint32_t pc_log = uc_sys->uc_mcontext.arm_pc;
            // Логируем в файл: номер syscall, PC, R0
            if (g_crashLogPath[0] != '\0') {
                int log_fd = open(g_crashLogPath, O_RDWR | O_CREAT | O_APPEND, 0666);
                if (log_fd >= 0) {
                    SafeWriteStr(log_fd, "SIGSYS: syscall=0x");
                    SafeWriteHex(log_fd, syscall_num);
                    SafeWriteStr(log_fd, " PC=0x");
                    SafeWriteHex(log_fd, pc_log);
                    SafeWriteStr(log_fd, " R0=0x");
                    SafeWriteHex(log_fd, r0);
                    SafeWriteStr(log_fd, "\n");
                    close(log_fd);
                }
            }
            // Известные iOS syscalls:
            // 4   = write
            // 6   = close
            // 20  = getpid
            // 33  = access
            // 90  = mmap (критичный — возвращаем MAP_FAILED чтобы код упал в обычный fallback)
            // 194 = mmap2
            // 199 = lseek
            // 336 = pthread_sigmask / bsdthread_create — просто 0
            // Большинство остальных: возвращаем 0 (успех) и продолжаем
            // ВАЖНО: При SIGSYS ядро уже остановилось ДО выполнения syscall,
            // PC указывает на svc инструкцию. Нам нужно пропустить её и эмулировать.
            // R0 трогаем ТОЛЬКО для syscall'ов которые реально возвращают значение —
            // иначе затираем this-указатель или другие важные значения.
            uint32_t ret = r0; // сохраняем R0 по умолчанию
            bool write_r0 = false;
            switch (syscall_num & 0xFFFF) {
                case 20:  // getpid
                    ret = (uint32_t)getpid(); write_r0 = true; break;
                case 4:   // write
                    ret = (uint32_t)write((int)r0,
                        (const void*)uc_sys->uc_mcontext.arm_r1,
                        (size_t)uc_sys->uc_mcontext.arm_r2);
                    write_r0 = true; break;
                case 3:   // read
                    ret = 0; write_r0 = true; break;
                case 6:   // close — void, R0 не трогаем
                    break;
                case 33:  // access
                    ret = 0; write_r0 = true; break;
                case 90:  // mmap
                case 194: // mmap2
                    ret = (uint32_t)MAP_FAILED; write_r0 = true; break;
                case 199: // lseek
                    ret = 0; write_r0 = true; break;
                case 240: // bsdthread_create
                case 241: // bsdthread_terminate
                case 336: // pthread_sigmask
                    break; // void — R0 не трогаем
                default:
                    break; // неизвестный — R0 не трогаем, просто пропускаем svc
            }
            if (write_r0) uc_sys->uc_mcontext.arm_r0 = ret;
            // Пропускаем svc: Thumb = 2 байта, ARM32 = 4 байта
            uint32_t pc_sys = uc_sys->uc_mcontext.arm_pc;
            uint32_t cpsr_sys = uc_sys->uc_mcontext.arm_cpsr;
            if (cpsr_sys & (1 << 5)) {
                uc_sys->uc_mcontext.arm_pc = pc_sys + 2;
            } else {
                uc_sys->uc_mcontext.arm_pc = pc_sys + 4;
            }
            __sync_fetch_and_add(&g_crash_counter, -1);
            return;
        }
    }
#endif

    // ЗАЩИТА ОТ ДВОЙНОГО КРАША:
    if (__sync_fetch_and_add(&g_crash_counter, 1) > 0) {
        struct sigaction sa; sa.sa_handler = SIG_DFL; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
        sigaction(sig, &sa, nullptr);
        raise(sig);
        return;
    }

    off_t start_pos = -1;
    int fd = -1;
    if (g_crashLogPath[0] != '\0') {
        fd = open(g_crashLogPath, O_RDWR | O_CREAT, 0666);
        if (fd >= 0) start_pos = lseek(fd, 0, SEEK_END);
    }

    uint32_t fault_addr = info ? (uint32_t)info->si_addr : 0;
    ucontext_t *uc = (ucontext_t *)context;
    uint32_t pc = 0, lr = 0, sp = 0, fp = 0, ip = 0, cpsr = 0;
    uint32_t regs[16] = {0};
    const char* rnames[] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10", "FP", "IP", "SP", "LR", "PC"};

#if defined(__arm__)
    if (uc) {
        pc = uc->uc_mcontext.arm_pc; lr = uc->uc_mcontext.arm_lr;
        sp = uc->uc_mcontext.arm_sp; fp = uc->uc_mcontext.arm_fp;
        ip = uc->uc_mcontext.arm_ip; cpsr = uc->uc_mcontext.arm_cpsr;
        regs[0] = uc->uc_mcontext.arm_r0; regs[1] = uc->uc_mcontext.arm_r1; regs[2] = uc->uc_mcontext.arm_r2; regs[3] = uc->uc_mcontext.arm_r3;
        regs[4] = uc->uc_mcontext.arm_r4; regs[5] = uc->uc_mcontext.arm_r5; regs[6] = uc->uc_mcontext.arm_r6; regs[7] = uc->uc_mcontext.arm_r7;
        regs[8] = uc->uc_mcontext.arm_r8; regs[9] = uc->uc_mcontext.arm_r9; regs[10] = uc->uc_mcontext.arm_r10;
        regs[11] = fp; regs[12] = ip; regs[13] = sp; regs[14] = lr; regs[15] = pc;
    }
#endif

    // СТУПЕНЬ 1: БЕЗОПАСНЫЙ И МАКСИМАЛЬНО ПОДРОБНЫЙ ЛОГ
    if (fd >= 0) {
        SafeWriteStr(fd, "\n==================================================\n!!! SAFE FATAL NATIVE CRASH !!!\nSignal: ");
        SafeWriteHex(fd, sig); SafeWriteStr(fd, "\n");
        SafeWriteStr(fd, "Thread ID: "); SafeWriteHex(fd, (uint32_t)pthread_self()); SafeWriteStr(fd, "\n");
        SafeWriteStr(fd, "Fault Address: "); SafeWriteHex(fd, fault_addr);
        if (fault_addr < 0x1000) SafeWriteStr(fd, " -> [Null Pointer Dereference]");
        SafeWriteStr(fd, "\n");
#if defined(__arm__)
        if (uc) {
            SafeWriteStr(fd, "Crash Module: "); SafeWriteModuleInfo(fd, pc); SafeWriteStr(fd, "\n");
            SafeWriteStr(fd, "LR Module: "); SafeWriteModuleInfo(fd, lr); SafeWriteStr(fd, "\n\n");
            
            SafeWriteStr(fd, "Dumping registers for current thread:\n");
            for (int i = 0; i < 16; i++) ProbeRegisterSafe(fd, rnames[i], regs[i]);
            SafeWriteStr(fd, "\tCPSR: "); SafeWriteHex(fd, cpsr); SafeWriteStr(fd, "\n\n");

            if (fault_addr >= 0x1000) {
                for (int i = 0; i < 15; i++) {
                    if (fault_addr == regs[i]) {
                        SafeWriteStr(fd, "-> [Invalid Memory Read/Write from "); SafeWriteStr(fd, rnames[i]); SafeWriteStr(fd, "]\n");
                    }
                }
            }
            
            SafeWriteStr(fd, "Code Dump around PC:\n");
            uint32_t* pc_ptr = (uint32_t*)(pc & ~3);
            if ((uintptr_t)pc_ptr > 0x1000) {
                pc_ptr -= 4;
                for (int i = 0; i < 8; i++) {
                    if ((uintptr_t)pc_ptr < 0x1000) break;
                    if ((uint32_t)pc_ptr == pc) SafeWriteStr(fd, "=> "); else SafeWriteStr(fd, "   ");
                    SafeWriteHex(fd, (uint32_t)pc_ptr); SafeWriteStr(fd, ": "); SafeWriteHex(fd, *pc_ptr); SafeWriteStr(fd, "\n");
                    pc_ptr++;
                }
            }
            SafeWriteStr(fd, "\n"); fsync(fd);

            SafeWriteStr(fd, "Code Dump around LR:\n");
            uint32_t* lr_ptr = (uint32_t*)(lr & ~3);
            if ((uintptr_t)lr_ptr > 0x1000) {
                lr_ptr -= 4;
                for (int i = 0; i < 8; i++) {
                    if ((uintptr_t)lr_ptr < 0x1000) break;
                    if ((uint32_t)lr_ptr == lr) SafeWriteStr(fd, "=> "); else SafeWriteStr(fd, "   ");
                    SafeWriteHex(fd, (uint32_t)lr_ptr); SafeWriteStr(fd, ": "); SafeWriteHex(fd, *lr_ptr); SafeWriteStr(fd, "\n");
                    lr_ptr++;
                }
            }
            SafeWriteStr(fd, "\n"); fsync(fd);
            
            SafeWriteStr(fd, "Raw Stack Dump (SP heuristic walk):\n");
            uint32_t* sp_ptr = (uint32_t*)sp;
            if ((uintptr_t)sp_ptr > 0x1000 && (uintptr_t)sp_ptr % 4 == 0) {
                for (int i = 0; i < 128; i++) {
                    SafeWriteHex(fd, (uint32_t)sp_ptr); SafeWriteStr(fd, ": ");
                    uint32_t val = *sp_ptr;
                    SafeWriteHex(fd, val);
                    if (val > 0x10000) { SafeWriteStr(fd, " -> "); SafeWriteModuleInfo(fd, val); }
                    SafeWriteStr(fd, "\n");
                    sp_ptr++;
                    fsync(fd);
                }
            } else {
                SafeWriteStr(fd, "Stack Pointer is invalid or unaligned.\n");
            }
        }
#endif
        SafeWriteStr(fd, "\n=== BLACK BOX (Последние 1000 операций ФС) ===\n");
        pthread_mutex_lock(&g_blackBoxMutex);
        int bb_start = g_blackBoxWrapped ? g_blackBoxIndex : 0;
        int bb_count = g_blackBoxWrapped ? 1000 : g_blackBoxIndex;
        for (int i = 0; i < bb_count; i++) {
            int idx = (bb_start + i) % 1000;
            SafeWriteStr(fd, g_blackBox[idx].c_str());
            SafeWriteStr(fd, "\n");
        }
        pthread_mutex_unlock(&g_blackBoxMutex);
        SafeWriteStr(fd, "==================================================\n");
        fsync(fd);
    }

    // СТУПЕНЬ 2: ПОДРОБНЫЙ ЛОГ С ИМЕНАМИ ФУНКЦИЙ
    static char buf[32768]; // 32 КБ статического буфера хватит на весь лог
    int offset = 0;
    auto append = [&](const char* fmt, ...) {
        if (offset >= sizeof(buf) - 1) return;
        va_list args; va_start(args, fmt);
        int n = vsnprintf(buf + offset, sizeof(buf) - offset, fmt, args);
        va_end(args);
        if (n > 0) {
            int written = (offset + n >= sizeof(buf)) ? (sizeof(buf) - offset - 1) : n;
            if (written > 0) {
                // Сразу сбрасываем в файл, чтобы не потерять при двойном краше
                if (fd >= 0) { write(fd, buf + offset, written); fsync(fd); }
                offset += written;
            }
        }
    };

    auto get_probe_str = [](uint32_t val) -> std::string {
        if (val <= 0x1000) return "";
        std::string res = ""; uint32_t isa = 0;
        bool has_mem = false; uint32_t m0 = 0, m1 = 0, m2 = 0, m3 = 0;
        
        if (val % 4 == 0 && SafeRead32(val, &m0)) {
            has_mem = true; isa = m0;
            SafeRead32(val + 4, &m1); SafeRead32(val + 8, &m2); SafeRead32(val + 12, &m3);
        }

        if (has_mem && isa > 0x1000) {
            if (isa == 0xDEADBEEF) {
                uint32_t hle_cls_ptr = 0;
                if (SafeRead32(val + 4, &hle_cls_ptr) && hle_cls_ptr > 0x1000) {
                    char cls_name[32] = {0};
                    if (SafeReadString(hle_cls_ptr, cls_name, 31)) res += " -> [HLE Class: " + std::string(cls_name) + "]";
                }
            } else {
                uint32_t maybe_deadbeef = 0;
                if (SafeRead32(isa, &maybe_deadbeef) && maybe_deadbeef == 0xDEADBEEF) {
                    uint32_t hle_cls_ptr = 0;
                    if (SafeRead32(isa + 4, &hle_cls_ptr) && hle_cls_ptr > 0x1000) {
                        char cls_name[32] = {0};
                        if (SafeReadString(hle_cls_ptr, cls_name, 31)) res += " -> [HLE Instance: " + std::string(cls_name) + "]";
                    }
                } else {
                    uint32_t data_ptr = 0;
                    if (SafeRead32(isa + 16, &data_ptr) && data_ptr > 0x1000) {
                        data_ptr &= ~3; uint32_t name_ptr = 0;
                        if (SafeRead32(data_ptr + 16, &name_ptr) && name_ptr > 0x1000) {
                            char cls_name[32] = {0};
                            if (SafeReadString(name_ptr, cls_name, 31)) res += " -> [ObjC Class: " + std::string(cls_name) + "]";
                        }
                    }
                }
            }
        }
        char str_buf[128] = {0};
        if (SafeReadString(val, str_buf, 127)) {
            bool is_ascii = true; int len = 0;
            for (int i = 0; i < 10 && str_buf[i]; i++) {
                if ((unsigned char)str_buf[i] < 32 || (unsigned char)str_buf[i] > 126) { is_ascii = false; break; }
                len++;
            }
            if (is_ascii && len >= 3) res += " -> [ASCII: '" + std::string(str_buf) + "']";
        }
        if (has_mem) {
            char mem_buf[128];
            snprintf(mem_buf, sizeof(mem_buf), " | Mem:[%08x %08x %08x %08x]", m0, m1, m2, m3);
            res += mem_buf;
        }
        return res;
    };

    append("\n==================================================\n!!! FATAL NATIVE CRASH !!!\nSignal: %d\nThread ID: %ld\nFault Address: 0x%08x%s\n", sig, (long)pthread_self(), fault_addr, fault_addr < 0x1000 ? " -> [Null Pointer Dereference]" : "");
    append("Fault Memory Area: %s\n", GetModuleInfoForAddress(fault_addr).c_str());

#if defined(__arm__)
    if (uc) {
        append("Crash Module: %s\nLR Module: %s\n\n", GetModuleInfoForAddress(pc).c_str(), GetModuleInfoForAddress(lr).c_str());
        
        append("Dumping registers for current thread:\n");
        for (int i = 0; i < 16; i++) {
            append("\t%-3s: 0x%08x%s\n", rnames[i], regs[i], get_probe_str(regs[i]).c_str());
        }
        append("\tCPSR: 0x%08x\n\n", cpsr);
        
        if (fault_addr >= 0x1000) {
            for (int i = 0; i < 15; i++) {
                if (fault_addr == regs[i]) append("-> [Invalid Memory Read/Write from %s]\n", rnames[i]);
            }
        }
        
        append("\nCode Dump around PC (Thumb-friendly 16-bit pairs):\n");
        uint32_t* pc_ptr = (uint32_t*)(pc & ~3);
        if ((uintptr_t)pc_ptr > 0x1000) {
            pc_ptr -= 4;
            for (int i = 0; i < 8; i++) {
                if ((uintptr_t)pc_ptr < 0x1000) break;
                uint32_t val = *pc_ptr;
                append("%s0x%08x: %04x %04x\n", ((uint32_t)pc_ptr == (pc & ~3)) ? "=> " : "   ", (uint32_t)pc_ptr, val & 0xFFFF, val >> 16);
                pc_ptr++;
            }
        }
        
        append("\nCode Dump around LR (Thumb-friendly 16-bit pairs):\n");
        uint32_t* lr_ptr = (uint32_t*)(lr & ~3);
        if ((uintptr_t)lr_ptr > 0x1000) {
            lr_ptr -= 4;
            for (int i = 0; i < 8; i++) {
                if ((uintptr_t)lr_ptr < 0x1000) break;
                uint32_t val = *lr_ptr;
                append("%s0x%08x: %04x %04x\n", ((uint32_t)lr_ptr == (lr & ~3)) ? "=> " : "   ", (uint32_t)lr_ptr, val & 0xFFFF, val >> 16);
                lr_ptr++;
            }
        }
        
        append("\nRaw Stack Dump (SP heuristic walk):\n");
        uint32_t* sp_ptr = (uint32_t*)sp;
        if ((uintptr_t)sp_ptr > 0x1000 && (uintptr_t)sp_ptr % 4 == 0) {
            for (int i = 0; i < 128; i++) {
                uint32_t val = 0;
                if (!SafeRead32((uintptr_t)sp_ptr, &val)) break;
                std::string probe = get_probe_str(val);
                if (val > 0x10000) {
                    std::string mod = GetModuleInfoForAddress(val);
                    if (mod != "Unknown Module / Heap" || !probe.empty()) {
                        append("0x%08x: 0x%08x -> %s%s\n", (uint32_t)sp_ptr, val, mod.c_str(), probe.c_str());
                    } else {
                        append("0x%08x: 0x%08x\n", (uint32_t)sp_ptr, val);
                    }
                } else {
                    append("0x%08x: 0x%08x%s\n", (uint32_t)sp_ptr, val, probe.c_str());
                }
                sp_ptr++;
            }
        } else {
            append("Stack Pointer is invalid or unaligned.\n");
        }

        append("\nAttempting to produce stack trace (Frame Pointer walk):\n");
        append(" 0. 0x%08x (PC) -> %s\n", pc, GetModuleInfoForAddress(pc).c_str());
        append(" 1. 0x%08x (LR) -> %s\n", lr, GetModuleInfoForAddress(lr).c_str());
        
        uint32_t current_fp = fp;
        int depth = 2;
        while (current_fp != 0 && depth < 20) {
            if (current_fp % 4 != 0 || current_fp < 0x1000) {
                append("Next FP (0x%08x) is invalid/unaligned.\n", current_fp);
                break;
            }
            uint32_t next_fp = 0;
            uint32_t ret_addr = 0;
            if (!SafeRead32(current_fp, &next_fp) || !SafeRead32(current_fp + 4, &ret_addr)) {
                append("Failed to safely read Frame Pointer at 0x%08x.\n", current_fp);
                break;
            }
            
            append("%2d. 0x%08x -> %s\n", depth, ret_addr, GetModuleInfoForAddress(ret_addr).c_str());
            current_fp = next_fp;
            depth++;
        }

        append("\nAndroid Unwind Native Backtrace (Accurate C++ Stack):\n");
        void* buffer[64];
        BacktraceState state = {buffer, buffer + 64};
        _Unwind_Backtrace(UnwindCallback, &state);
        int count = state.current - buffer;
        for (int i = 0; i < count; i++) {
            uintptr_t u_pc = (uintptr_t)buffer[i];
            append(" %2d. 0x%08x -> %s\n", i, (uint32_t)u_pc, GetModuleInfoForAddress(u_pc).c_str());
        }

    }
#endif
    append("==================================================\n");
    
    if (fd >= 0) {
        // Если мы дошли до сюда, значит второй этап сформирован успешно без двойного краша.
        // Теперь можно безопасно удалить первый этап и записать только второй, чтобы не было дублей.
        if (start_pos >= 0) {
            ftruncate(fd, start_pos);      
            lseek(fd, start_pos, SEEK_SET); 
        } else {
            ftruncate(fd, 0);
            lseek(fd, 0, SEEK_SET); 
        }
        if (offset > 0) {
            write(fd, buf, offset); fsync(fd); 
        }
        close(fd); 
    }

    // СТУПЕНЬ ПОСЛЕДНЯЯ: ПРОБРОС КРАША В СИСТЕМУ
    struct sigaction sa; sa.sa_handler = SIG_DFL; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, sig); sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    raise(sig);
}



// ==========================================
// JNI HELPER
// ==========================================
JNIEnv* GetJNIEnv() {
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        g_jvm->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

// ==========================================
// MEDIA JNI BRIDGES (Audio & Video)
// ==========================================
void CallJNI_V_I(const char* m, void* ptr) { JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return; jclass c = env->GetObjectClass(g_mainActivity); jmethodID id = env->GetMethodID(c, m, "(I)V"); if (id) env->CallVoidMethod(g_mainActivity, id, (jint)(uintptr_t)ptr); env->DeleteLocalRef(c); }
void CallJNI_V_IF(const char* m, void* ptr, float v) { JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return; jclass c = env->GetObjectClass(g_mainActivity); jmethodID id = env->GetMethodID(c, m, "(IF)V"); if (id) env->CallVoidMethod(g_mainActivity, id, (jint)(uintptr_t)ptr, v); env->DeleteLocalRef(c); }
void CallJNI_V_IZ(const char* m, void* ptr, bool v) { JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return; jclass c = env->GetObjectClass(g_mainActivity); jmethodID id = env->GetMethodID(c, m, "(IZ)V"); if (id) env->CallVoidMethod(g_mainActivity, id, (jint)(uintptr_t)ptr, v); env->DeleteLocalRef(c); }
void CallJNI_V_IS(const char* m, void* ptr, const std::string& s) { JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return; jclass c = env->GetObjectClass(g_mainActivity); jmethodID id = env->GetMethodID(c, m, "(ILjava/lang/String;)V"); if (id) { jstring js = env->NewStringUTF(s.c_str()); env->CallVoidMethod(g_mainActivity, id, (jint)(uintptr_t)ptr, js); env->DeleteLocalRef(js); } env->DeleteLocalRef(c); }
float CallJNI_F_I(const char* m, void* ptr) { JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return 0.0f; jclass c = env->GetObjectClass(g_mainActivity); jmethodID id = env->GetMethodID(c, m, "(I)F"); float res = id ? env->CallFloatMethod(g_mainActivity, id, (jint)(uintptr_t)ptr) : 0.0f; env->DeleteLocalRef(c); return res; }
bool CallJNI_Z_I(const char* m, void* ptr) { JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return false; jclass c = env->GetObjectClass(g_mainActivity); jmethodID id = env->GetMethodID(c, m, "(I)Z"); bool res = id ? env->CallBooleanMethod(g_mainActivity, id, (jint)(uintptr_t)ptr) : false; env->DeleteLocalRef(c); return res; }

void AudioInitToJava(void* ptr, const std::string& path) { CallJNI_V_IS("audioInit", ptr, path); }
void AudioPlayToJava(void* ptr) { CallJNI_V_I("audioPlay", ptr); }
void AudioPauseToJava(void* ptr) { CallJNI_V_I("audioPause", ptr); }
void AudioSetLoopingToJava(void* ptr, bool looping) { CallJNI_V_IZ("audioSetLooping", ptr, looping); }
bool AudioIsPlayingToJava(void* ptr) { return CallJNI_Z_I("audioIsPlaying", ptr); }
void AudioReleaseToJava(void* ptr) { CallJNI_V_I("audioRelease", ptr); }
void AudioStopToJava(void* ptr) { CallJNI_V_I("audioStop", ptr); }
void AudioSetVolumeToJava(void* ptr, float volume) { CallJNI_V_IF("audioSetVolume", ptr, volume); }
float AudioGetDurationToJava(void* ptr) { return CallJNI_F_I("audioGetDuration", ptr); }
float AudioGetCurrentTimeToJava(void* ptr) { return CallJNI_F_I("audioGetCurrentTime", ptr); }
void AudioSetCurrentTimeToJava(void* ptr, float time) { CallJNI_V_IF("audioSetCurrentTime", ptr, time); }

void VideoInitToJava(void* ptr, const std::string& path) { CallJNI_V_IS("videoInit", ptr, path); }
void VideoPlayToJava(void* ptr) { CallJNI_V_I("videoPlay", ptr); }
void VideoStopToJava(void* ptr) { CallJNI_V_I("videoStop", ptr); }

void AudioUnitStreamInitToJava(int sampleRate, int channels) {
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "audioUnitStreamInit", "(II)V");
    if (m) env->CallVoidMethod(g_mainActivity, m, sampleRate, channels);
    env->DeleteLocalRef(clazz);
}

void AudioUnitStreamWriteToJava(const uint8_t* data, int size) {
    // Жесткая защита от мусорных размеров буфера, которые может вернуть игра
    if (size <= 0 || size > 1024 * 1024 || !data) return; 
    
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "audioUnitStreamWrite", "([BI)V");
    if (m) {
        jbyteArray jData = env->NewByteArray(size);
        if (jData) { // Проверяем, что JNI реально выделил память (спасает от OutOfMemory)
            env->SetByteArrayRegion(jData, 0, size, (const jbyte*)data);
            env->CallVoidMethod(g_mainActivity, m, jData, size);
            env->DeleteLocalRef(jData);
        } else {
            env->ExceptionClear(); // Молча гасим исключение JNI, чтобы не уронить C++
        }
    }
    env->DeleteLocalRef(clazz);
}

// ==========================================
// ABSOLUTE MEGA DEBUGGER FOR 0x8 CRASH
// ==========================================
void _SyncLog(const std::string& msg) {
    if (!g_logRenderDebug) return;
    _LogToJava(msg);
}

void DumpGLState(const std::string& prefix) {
    if (!g_logRenderDebug) return;

    char buf[8192];
    int offset = 0;
    offset += snprintf(buf + offset, sizeof(buf) - offset, "\n>>>>>>>> [ABSOLUTE-DEBUG] %s <<<<<<<<\n", prefix.c_str());
    offset += snprintf(buf + offset, sizeof(buf) - offset, "Thread: %ld | Target: Render\n", (long)pthread_self());

    offset += snprintf(buf + offset, sizeof(buf) - offset, "[Render] Surface Target: %dx%d\n", g_surfaceWidth, g_surfaceHeight);
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[Render] ANativeWindow State: %p\n", g_nativeWindow);
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[Render] Color Buffer Elements: %zu\n", g_cpuColorBuffer.size());
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[Render] Depth Buffer Elements: %zu\n", g_cpuDepthBuffer.size());
    offset += snprintf(buf + offset, sizeof(buf) - offset, "[Render] Clear Color RGBA: %.2f %.2f %.2f %.2f\n", g_cpuClearColor[0], g_cpuClearColor[1], g_cpuClearColor[2], g_cpuClearColor[3]);
    offset += snprintf(buf + offset, sizeof(buf) - offset, ">>>>>>>> END RENDER DUMP <<<<<<<<\n");
    SyncLog(buf);
}


// Вспомогательная функция отключена, чтобы не ломать персистентный стейт игры
void ForceSafeGLState() {
    // Пусто
}

extern "C" void Stub_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    g_cpuClearColor[0] = red; g_cpuClearColor[1] = green; g_cpuClearColor[2] = blue; g_cpuClearColor[3] = alpha;
    if (g_gpuOffloadMask & 2) {
        // ФИКС ПРОЗРАЧНОСТИ: Принудительно альфа = 1.0, чтобы сквозь игру не просвечивал рабочий стол телефона
        glClearColor(red, green, blue, 1.0f);
    }
}

extern "C" void MegaDebug_glClear(GLbitfield mask) {
    // ФИКС ВЫЛЕТА (Signal 11 в драйвере): Очищаем мусорные биты расширений iOS (например 0x16640),
    // от которых Android-драйвер крашится при аппаратном рендере
    mask &= (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    bool isSpamOn = (g_spamFiltersMask & (1 << 5)) != 0;
    static int clear_cnt = 0; clear_cnt++;
    if (!isSpamOn || clear_cnt <= 30 || clear_cnt % 120 == 0) {
        SyncLog("ENTER MegaDebug_glClear(0x" + std::to_string(mask) + ")");
        DumpGLState("BEFORE glClear");
    }

    if (g_gpuOffloadMask & 2) {
        SyncLog("[RENDER] Выполняем GPU очистку буфера...");
        
        static bool s_viewport_forced = false;
        if (!s_viewport_forced) {
            EGLint realW = g_surfaceWidth, realH = g_surfaceHeight;
            EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
            if (surf != EGL_NO_SURFACE) {
                eglQuerySurface(eglGetCurrentDisplay(), surf, EGL_WIDTH, &realW);
                eglQuerySurface(eglGetCurrentDisplay(), surf, EGL_HEIGHT, &realH);
            }
            glViewport(0, 0, realW, realH);
            s_viewport_forced = true;
        }

        if (g_lastActiveFBO == 0 && (mask & GL_COLOR_BUFFER_BIT)) {
            // ФИКС ПРОЗРАЧНОСТИ 2: Защищаем альфа-канал от затирания очисткой
            glColorMask(g_colorMask[0], g_colorMask[1], g_colorMask[2], GL_FALSE);
            glClear(mask);
            glColorMask(g_colorMask[0], g_colorMask[1], g_colorMask[2], g_colorMask[3]);
        } else {
            glClear(mask);
        }
    } else {
        SyncLog("[RENDER] Выполняем очистку буфера...");
        std::vector<uint32_t>* targetColorBuf = &g_cpuColorBuffer;
        std::vector<float>* targetDepthBuf = &g_cpuDepthBuffer;
        int targetW = g_surfaceWidth;
        int targetH = g_surfaceHeight;

        if (g_lastActiveFBO != 0 && g_fboColorTex.count(g_lastActiveFBO)) {
            GLuint tex = g_fboColorTex[g_lastActiveFBO];
            if (tex != 0 && g_cpuTexW.count(tex) && g_cpuTexW[tex] > 0) {
                targetW = g_cpuTexW[tex];
                targetH = g_cpuTexH[tex];
                if (g_cpuTextures[tex].size() != (size_t)(targetW * targetH)) g_cpuTextures[tex].resize(targetW * targetH, 0x00000000);
                targetColorBuf = &g_cpuTextures[tex];
                if (g_fboDepthBuf[g_lastActiveFBO].size() != (size_t)(targetW * targetH)) g_fboDepthBuf[g_lastActiveFBO].resize(targetW * targetH, 1.0f);
                targetDepthBuf = &g_fboDepthBuf[g_lastActiveFBO];
            }
        }

        if (targetColorBuf->size() != (size_t)(targetW * targetH)) {
            targetColorBuf->resize(targetW * targetH, 0xFF000000);
            targetDepthBuf->resize(targetW * targetH, 1.0f);
        }
        
        if (mask & GL_COLOR_BUFFER_BIT) {
            uint8_t cr = (uint8_t)(g_cpuClearColor[0] * 255.0f);
            uint8_t cg = (uint8_t)(g_cpuClearColor[1] * 255.0f);
            uint8_t cb = (uint8_t)(g_cpuClearColor[2] * 255.0f);
            uint8_t ca = (uint8_t)(g_cpuClearColor[3] * 255.0f);
            uint32_t baseColor = (ca << 24) | (cb << 16) | (cg << 8) | cr;
            std::fill(targetColorBuf->begin(), targetColorBuf->end(), baseColor);
        }
        if (mask & GL_DEPTH_BUFFER_BIT) {
            std::fill(targetDepthBuf->begin(), targetDepthBuf->end(), 1.0f);
        }
    }
    
    GLint err = glGetError();
    static int clear_err_cnt = 0; clear_err_cnt++;
    if (err != 0 || !isSpamOn || clear_err_cnt <= 30 || clear_err_cnt % 120 == 0) {
        SyncLog("POST glClear Error: 0x" + std::to_string(err));
    }
}

extern "C" EGLBoolean MegaDebug_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    bool isSpamOn = (g_spamFiltersMask & (1 << 5)) != 0;
    static int swap_cnt = 0; swap_cnt++;
    if (!isSpamOn || swap_cnt <= 30 || swap_cnt % 120 == 0) {
        SyncLog("ENTER MegaDebug_eglSwapBuffers");
        DumpGLState("BEFORE eglSwapBuffers");
    }

    // ФИКС ЧЕРНОГО ЭКРАНА: Если GPU Draw (16) включен, а GPU Swap (1) выключен,
    // кадр остался в видеопамяти! Нужно вытянуть его в CPU, чтобы оверлей и ANativeWindow отработали.
    if ((g_gpuOffloadMask & 16) && !(g_gpuOffloadMask & 1)) {
        std::vector<uint32_t> tempGpuBuf(g_surfaceWidth * g_surfaceHeight);
        glReadPixels(0, 0, g_surfaceWidth, g_surfaceHeight, GL_RGBA, GL_UNSIGNED_BYTE, tempGpuBuf.data());
        
        // OpenGL читает снизу-вверх (перевернуто). Нужно отзеркалить по Y в g_cpuColorBuffer
        for (int y = 0; y < g_surfaceHeight; y++) {
            int invY = g_surfaceHeight - 1 - y;
            uint32_t* dstRow = &g_cpuColorBuffer[invY * g_surfaceWidth];
            uint32_t* srcRow = &tempGpuBuf[y * g_surfaceWidth];
            
            // Фикс формата: glReadPixels дает ABGR или RGBA, а нам нужен формат Android.
            for (int x = 0; x < g_surfaceWidth; x++) {
                uint32_t px = srcRow[x];
                uint8_t r = px & 0xFF;
                uint8_t g = (px >> 8) & 0xFF;
                uint8_t b = (px >> 16) & 0xFF;
                dstRow[x] = 0xFF000000 | (b << 16) | (g << 8) | r;
            }
        }
    }

    // --- ВИЗУАЛЬНАЯ ПРИБОРНАЯ ПАНЕЛЬ ОТЛАДКИ ---
    if (g_onScreenDebugOverlay && g_cpuColorBuffer.size() == (size_t)(g_surfaceWidth * g_surfaceHeight)) {
        // 1. Рамка безопасности (Зеленая по краям экрана)
        CPUDrawLine(0, 0, g_surfaceWidth - 1, 0, 0xFF00FF00);
        CPUDrawLine(0, g_surfaceHeight - 1, g_surfaceWidth - 1, g_surfaceHeight - 1, 0xFF00FF00);
        CPUDrawLine(0, 0, 0, g_surfaceHeight - 1, 0xFF00FF00);
        CPUDrawLine(g_surfaceWidth - 1, 0, g_surfaceWidth - 1, g_surfaceHeight - 1, 0xFF00FF00);

        // 2. Heartbeat потока (левый верхний угол)
        g_debugHeartbeat++;
        uint32_t hbColor = (g_debugHeartbeat % 20 < 10) ? 0xFF0000FF : 0xFFFF0000; // Красный / Синий
        for (int y = 5; y < 15; y++) {
            for (int x = 5; x < 15; x++) g_cpuColorBuffer[y * g_surfaceWidth + x] = hbColor;
        }

        // 3. Индикатор вызова отрисовки (рядом с Heartbeat)
        uint32_t drawColor = g_frameHasDraw ? 0xFF00FF00 : 0xFF444444; // Зеленый / Серый
        for (int y = 5; y < 15; y++) {
            for (int x = 20; x < 30; x++) g_cpuColorBuffer[y * g_surfaceWidth + x] = drawColor;
        }

        // 4. Индикатор активного FBO (Пурпурный = 0, Оранжевый = скрытый FBO)
        uint32_t fboColor = (g_lastActiveFBO == 0) ? 0xFFFF00FF : 0xFF00A5FF;
        for (int y = 5; y < 15; y++) {
            for (int x = 35; x < 45; x++) g_cpuColorBuffer[y * g_surfaceWidth + x] = fboColor;
        }

        // 5. Миниатюра последней активной текстуры (правый верхний угол)
        if (g_cpuActiveTexture != 0 && g_cpuTextures.count(g_cpuActiveTexture) && !g_cpuTextures[g_cpuActiveTexture].empty()) {
            int tw = g_cpuTexW[g_cpuActiveTexture];
            int th = g_cpuTexH[g_cpuActiveTexture];
            if (tw > 0 && th > 0) {
                for (int y = 0; y < 50; y++) {
                    for (int x = 0; x < 50; x++) {
                        int tx = (x * tw) / 50;
                        int ty = (y * th) / 50;
                        uint32_t texCol = g_cpuTextures[g_cpuActiveTexture][ty * tw + tx];
                        g_cpuColorBuffer[(y + 5) * g_surfaceWidth + (g_surfaceWidth - 55 + x)] = texCol;
                    }
                }
                // Рамка вокруг миниатюры
                CPUDrawLine(g_surfaceWidth - 56, 4, g_surfaceWidth - 4, 4, 0xFFFFFFFF);
                CPUDrawLine(g_surfaceWidth - 56, 56, g_surfaceWidth - 4, 56, 0xFFFFFFFF);
                CPUDrawLine(g_surfaceWidth - 56, 4, g_surfaceWidth - 56, 56, 0xFFFFFFFF);
                CPUDrawLine(g_surfaceWidth - 4, 4, g_surfaceWidth - 4, 56, 0xFFFFFFFF);
            }
        }

        // 6. Индикаторы нажатий (Красные квадраты с белым перекрестием)
        for (auto const& pair : g_activeTouches) {
            FakeUITouch* t = pair.second;
            if (t) {
                CPUDrawSolidRect(t->x - 15.0f, t->y - 15.0f, 30.0f, 30.0f, 1.0f, 0.0f, 0.0f, 0.5f);
                CPUDrawLine((int)t->x - 5, (int)t->y, (int)t->x + 5, (int)t->y, 0xFFFFFFFF);
                CPUDrawLine((int)t->x, (int)t->y - 5, (int)t->x, (int)t->y + 5, 0xFFFFFFFF);
            }
        }

        // 7. Вывод реального разрешения игры (по glViewport)
        if (g_fontLoaded) {
            void* debugTextPtr = (void*)0xDEB001; // Фейковый указатель (ключ) для кэша мапы
            char resStr[64];
            if (g_isFakeViewport) {
                snprintf(resStr, sizeof(resStr), "Fake 480x320 (Real 0x0)");
            } else {
                snprintf(resStr, sizeof(resStr), "VP: %dx%d", g_gameViewportW, g_gameViewportH);
            }
            UpdateTextCache(debugTextPtr, resStr, 14.0f); // Кешируем текст нужного размера
            auto& cache = g_uiTextCache[debugTextPtr];
            
            if (!cache.bitmap.empty()) {
                // Подложка, чтобы текст читался на любом фоне
                CPUDrawSolidRect(4.0f, 19.0f, cache.width + 2.0f, cache.height + 2.0f, 0.0f, 0.0f, 0.0f, 0.7f, 2.0f);
                // Рисуем сам текст (Ярко-зеленый цвет) прямо под кубиками-статусами (y=20)
                CPUDrawText(5.0f, 20.0f, cache.width, cache.height, 0.0f, 1.0f, 0.0f, cache);
            }
        }
    }
    // -------------------------------------------

    // --- СЛОЙ PERFOMANCE OVERLAY (FPS) ---
    if (g_showPerfOverlay && g_cpuColorBuffer.size() == (size_t)(g_surfaceWidth * g_surfaceHeight)) {
        uint64_t currentTimeMs = GetRealTimeMs();
        
        pthread_mutex_lock(&g_fpsMutex);
        
        if (g_fpsLastTimeMs == 0) {
            g_fpsLastTimeMs = currentTimeMs;
            g_startTimeMs = currentTimeMs;
            g_fpsFrameCount = 0;
            g_totalFrames = 0;
        }
        
        g_fpsFrameCount++;
        g_totalFrames++;
        
        uint64_t deltaMs = currentTimeMs - g_fpsLastTimeMs;
        if (deltaMs >= 1000) {
            float fps = (g_fpsFrameCount * 1000.0f) / (float)deltaMs;
            g_currentFpsInt = (int)fps;
            g_currentFpsFrac = (int)((fps - g_currentFpsInt) * 10.0f);
            
            g_fpsFrameCount = 0;
            g_fpsLastTimeMs = currentTimeMs;
        }
        
        uint64_t totalDeltaMs = currentTimeMs - g_startTimeMs;
        if (totalDeltaMs > 0) {
            float avgFps = (g_totalFrames * 1000.0f) / (float)totalDeltaMs;
            g_avgFpsInt = (int)avgFps;
            g_avgFpsFrac = (int)((avgFps - g_avgFpsInt) * 10.0f);
        }
        
        int cFpsInt = g_currentFpsInt;
        int cFpsFrac = g_currentFpsFrac;
        int aFpsInt = g_avgFpsInt;
        int aFpsFrac = g_avgFpsFrac;
        
        pthread_mutex_unlock(&g_fpsMutex);

        if (g_fontLoaded) {
            void* fpsTextPtr = (void*)0xDEB002;
            char fpsStr[128];
            // Используем только INT для обхода бага ARMv7 variadic ABI
            snprintf(fpsStr, sizeof(fpsStr), "AVG FPS: %d.%d\nFPS: %d.%d", aFpsInt, aFpsFrac, cFpsInt, cFpsFrac);
            
            UpdateTextCache(fpsTextPtr, fpsStr, 16.0f);
            auto& cache = g_uiTextCache[fpsTextPtr];
            
            if (!cache.bitmap.empty()) {
                float drawX = 10.0f;
                float drawY = g_surfaceHeight - cache.height - 10.0f;
                
                CPUDrawSolidRect(drawX - 2.0f, drawY - 2.0f, cache.width + 4.0f, cache.height + 4.0f, 0.0f, 0.0f, 0.0f, 0.7f, 3.0f);
                CPUDrawText(drawX, drawY, cache.width, cache.height, 1.0f, 1.0f, 0.0f, cache);
            }
        }
    }
    // -------------------------------------------

    EGLBoolean res = EGL_TRUE;
    if (g_gpuOffloadMask & 1) {
        // --- GPU ОВЕРЛЕИ ---
        if (g_onScreenDebugOverlay || g_showPerfOverlay) {
            static GLuint overlayTex = 0;
            static GLuint overlayProg = 0;
            if (overlayTex == 0) {
                glGenTextures(1, &overlayTex);
                glBindTexture(GL_TEXTURE_2D, overlayTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                
                const char* vs = "attribute vec4 pos; attribute vec2 uv; varying vec2 v_uv; void main() { gl_Position = pos; v_uv = uv; }";
                const char* fs = "precision mediump float; varying vec2 v_uv; uniform sampler2D tex; void main() { gl_FragColor = texture2D(tex, v_uv); }";
                GLuint vsh = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vsh, 1, &vs, nullptr); glCompileShader(vsh);
                GLuint fsh = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fsh, 1, &fs, nullptr); glCompileShader(fsh);
                overlayProg = glCreateProgram(); glAttachShader(overlayProg, vsh); glAttachShader(overlayProg, fsh);
                glBindAttribLocation(overlayProg, 0, "pos"); glBindAttribLocation(overlayProg, 1, "uv");
                glLinkProgram(overlayProg);
            }
            
            GLint oldProg; glGetIntegerv(GL_CURRENT_PROGRAM, &oldProg);
            GLint oldTex; glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
            GLint oldActiveTex; glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTex);
            GLint oldArrayBuf; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuf);
            GLboolean blendEnabled = glIsEnabled(GL_BLEND);
            GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean cullFace = glIsEnabled(GL_CULL_FACE);
            
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            
            glUseProgram(overlayProg);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, overlayTex);
            
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_surfaceWidth, g_surfaceHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_cpuColorBuffer.data());
            glUniform1i(glGetUniformLocation(overlayProg, "tex"), 0);
            
            float verts[] = {
                // ФИКС ПЕРЕВОРОТА: Меняем V координаты местами (1.0 -> 0.0, 0.0 -> 1.0)
                // Так как glTexImage2D читает снизу-вверх, а наш CPU-буфер идет сверху-вниз.
                -1.0f,  1.0f, 0.0f, 0.0f, // top left (V=0)
                -1.0f, -1.0f, 0.0f, 1.0f, // bottom left (V=1)
                 1.0f,  1.0f, 1.0f, 0.0f, // top right (V=0)
                 1.0f, -1.0f, 1.0f, 1.0f  // bottom right (V=1)
            };
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, verts);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            
            glUseProgram(oldProg);
            glActiveTexture(oldActiveTex);
            glBindTexture(GL_TEXTURE_2D, oldTex);
            glBindBuffer(GL_ARRAY_BUFFER, oldArrayBuf);
            if (!blendEnabled) glDisable(GL_BLEND);
            if (depthTest) glEnable(GL_DEPTH_TEST);
            if (cullFace) glEnable(GL_CULL_FACE);
        }
        
        // Очищаем буфер прозрачным цветом, чтобы в следующем кадре оверлеи рисовались на чистом фоне
        std::fill(g_cpuColorBuffer.begin(), g_cpuColorBuffer.end(), 0x00000000);

        SyncLog("[RENDER] Отправка буфера на экран (GPU eglSwapBuffers)...");
        res = eglSwapBuffers(g_eglDisplay, g_eglSurface);
    } else if (g_nativeWindow) {
        SyncLog("[RENDER] Отправка буфера на экран (ANativeWindow)...");
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(g_nativeWindow, &buffer, nullptr) == 0) {
            int copyW = (buffer.width < (int32_t)g_surfaceWidth) ? buffer.width : g_surfaceWidth;
            int copyH = (buffer.height < (int32_t)g_surfaceHeight) ? buffer.height : g_surfaceHeight;
            for (int y = 0; y < copyH; y++) {
                uint32_t* dst = (uint32_t*)((uint8_t*)buffer.bits + y * buffer.stride * 4);
                uint32_t* src = &g_cpuColorBuffer[y * g_surfaceWidth];
                memcpy(dst, src, copyW * 4);
            }
            ANativeWindow_unlockAndPost(g_nativeWindow);
        } else {
            static int lock_err_cnt = 0; lock_err_cnt++;
            if (!isSpamOn || lock_err_cnt <= 30 || lock_err_cnt % 120 == 0) {
                SyncLog("[RENDER] ОШИБКА: Не удалось заблокировать ANativeWindow!");
            }
        }
    }
    
    EGLint err = eglGetError();
    static int swap_err_cnt = 0; swap_err_cnt++;
    if (err != 0 || !isSpamOn || swap_err_cnt <= 30 || swap_err_cnt % 120 == 0) {
        SyncLog("POST eglSwapBuffers Error: 0x" + std::to_string(err));
    }
    return res;
}

// OPENGL FBO REDIRECTORS
// ==========================================
extern "C" void Stub_glBindFramebuffer(GLenum target, GLuint framebuffer) { 
    g_lastActiveFBO = framebuffer;
    if (g_gpuOffloadMask & 64) {
        if (framebuffer == 1) glBindFramebuffer(target, 0); else glBindFramebuffer(target, framebuffer); 
    }
}
extern "C" void Stub_glBindRenderbuffer(GLenum target, GLuint renderbuffer) { 
    if (g_gpuOffloadMask & 64) {
        if (renderbuffer == 1 || renderbuffer == 2) glBindRenderbuffer(target, 0); else glBindRenderbuffer(target, renderbuffer); 
    }
}
extern "C" void Stub_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) { 
    if (g_gpuOffloadMask & 64) {
        if (renderbuffer == 1 || renderbuffer == 2) return; glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer); 
    }
}
extern "C" void Stub_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) { 
    GLint bound_rbo = 0; glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound_rbo); if (bound_rbo == 0) return; 
    if (g_gpuOffloadMask & 64) glRenderbufferStorage(target, internalformat, width, height); 
}
extern "C" GLenum Stub_glCheckFramebufferStatus(GLenum target) { 
    GLint bound_fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo); if (bound_fbo == 0) return GL_FRAMEBUFFER_COMPLETE; 
    if (g_gpuOffloadMask & 64) return glCheckFramebufferStatus(target);
    return GL_FRAMEBUFFER_COMPLETE;
}
extern "C" void Stub_glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params) { 
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    if (pname == 0x8D42) { 
        *params = g_surfaceWidth; 
        LogToJava(">>>>>>>> [SIZE-CRITICAL] glGetRenderbufferParameteriv(WIDTH) <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr) + " | Отдаем: " + std::to_string(*params));
        return; 
    } 
    if (pname == 0x8D43) { 
        *params = g_surfaceHeight; 
        LogToJava(">>>>>>>> [SIZE-CRITICAL] glGetRenderbufferParameteriv(HEIGHT) <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr) + " | Отдаем: " + std::to_string(*params));
        return; 
    } 
    glGetRenderbufferParameteriv(target, pname, params); 
}

extern "C" void Stub_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    if (name) g_attribLocations[program][name] = index;
    glBindAttribLocation(program, index, name);
}

extern "C" void Stub_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    GLint bound_fbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo);
    if (bound_fbo != 0 && attachment == 0x8CE0) {
        g_fboColorTex[bound_fbo] = texture;
    }
    if (texture != 0) {
        g_fboTextures[texture] = true;
    }
    if (g_gpuOffloadMask & 64) glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

extern "C" void Stub_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { 
    LogToJava("[GL-TRACE] glViewport(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(width) + ", " + std::to_string(height) + ")");
    if (width == 0 && height == 0) {
        g_isFakeViewport = true;
        width = 480;
        height = 320;
    } else {
        g_isFakeViewport = false;
    }
    g_gameViewportW = width; 
    g_gameViewportH = height;
    if (g_gpuOffloadMask & 64) {
        if (g_lastActiveFBO == 0 || g_lastActiveFBO == 1) {
            EGLint realW = g_surfaceWidth, realH = g_surfaceHeight;
            EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
            if (surf != EGL_NO_SURFACE) {
                eglQuerySurface(eglGetCurrentDisplay(), surf, EGL_WIDTH, &realW);
                eglQuerySurface(eglGetCurrentDisplay(), surf, EGL_HEIGHT, &realH);
            }
            glViewport(0, 0, realW, realH); 
        } else {
            glViewport(x, y, width, height); 
        } 
    }
}

std::map<GLenum, GLuint> g_boundBuffers;

struct VertexAttribState {
    GLint enabled = 0;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    const GLvoid* pointer = nullptr;
    GLuint vbo = 0;
    float constantValue[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // ИСПРАВЛЕНО: Белый цвет по умолчанию, чтобы текстуры не умножались на ноль
};
VertexAttribState g_vertexAttribs[16];

GLuint g_vboIdCounter = 1;
extern "C" void Stub_glGenBuffers(GLsizei n, GLuint *buffers) {
    for (int i = 0; i < n; i++) buffers[i] = g_vboIdCounter++;
}

extern "C" void Stub_glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    if (index < 16) {
        g_vertexAttribs[index].constantValue[0] = v0;
        g_vertexAttribs[index].constantValue[1] = v1;
        g_vertexAttribs[index].constantValue[2] = v2;
        g_vertexAttribs[index].constantValue[3] = v3;
    }
}

extern "C" void Stub_glVertexAttrib4fv(GLuint index, const GLfloat *v) {
    if (index < 16 && v) {
        g_vertexAttribs[index].constantValue[0] = v[0];
        g_vertexAttribs[index].constantValue[1] = v[1];
        g_vertexAttribs[index].constantValue[2] = v[2];
        g_vertexAttribs[index].constantValue[3] = v[3];
    }
}

extern "C" void Stub_glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) {
    if (index >= 16 || !params) return;
    if (pname == 0x8622) *params = g_vertexAttribs[index].enabled; // GL_VERTEX_ATTRIB_ARRAY_ENABLED
    else if (pname == 0x8623) *params = g_vertexAttribs[index].size; // GL_VERTEX_ATTRIB_ARRAY_SIZE
    else if (pname == 0x8624) *params = g_vertexAttribs[index].stride; // GL_VERTEX_ATTRIB_ARRAY_STRIDE
    else if (pname == 0x8625) *params = g_vertexAttribs[index].type; // GL_VERTEX_ATTRIB_ARRAY_TYPE
    else if (pname == 0x889F) *params = g_vertexAttribs[index].vbo; // GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING
}

extern "C" void Stub_glGetVertexAttribPointerv(GLuint index, GLenum pname, GLvoid **pointer) {
    if (index < 16 && pname == 0x8645 && pointer) { // GL_VERTEX_ATTRIB_ARRAY_POINTER
        *pointer = (GLvoid*)g_vertexAttribs[index].pointer;
    }
}

extern "C" void Stub_glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    if (pname == 0x8764 && params) { // GL_BUFFER_SIZE
        GLuint buffer = g_boundBuffers[target];
        if (buffer != 0 && g_vboShadow.count(buffer)) *params = g_vboShadow[buffer].size();
        else *params = 0;
    }
}

extern "C" void Stub_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *pointer) {
    if (index < 16) {
        g_vertexAttribs[index].size = size;
        g_vertexAttribs[index].type = type;
        g_vertexAttribs[index].stride = stride;
        g_vertexAttribs[index].pointer = pointer;
        g_vertexAttribs[index].vbo = g_boundBuffers[0x8892]; // Запоминаем текущий GL_ARRAY_BUFFER
    }
    if (g_gpuOffloadMask & 16) glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

extern "C" void Stub_glEnableVertexAttribArray(GLuint index) {
    if (index < 16) g_vertexAttribs[index].enabled = 1;
    if (g_gpuOffloadMask & 16) glEnableVertexAttribArray(index);
}

extern "C" void Stub_glDisableVertexAttribArray(GLuint index) {
    if (index < 16) g_vertexAttribs[index].enabled = 0;
    if (g_gpuOffloadMask & 16) glDisableVertexAttribArray(index);
}

extern "C" void Stub_glBindBuffer(GLenum target, GLuint buffer) {
    g_boundBuffers[target] = buffer;
    if (g_gpuOffloadMask & 16) glBindBuffer(target, buffer);
}

extern "C" void Stub_glBufferData(GLenum target, GLsizeiptr size, const GLvoid* data, GLenum usage) {
    GLuint buffer = g_boundBuffers[target];
    if (buffer != 0) {
        g_vboShadow[buffer].resize(size);
        if (data) memcpy(g_vboShadow[buffer].data(), data, size);
    }
    if (g_gpuOffloadMask & 16) glBufferData(target, size, data, usage);
}

extern "C" void Stub_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid* data) {
    GLuint buffer = g_boundBuffers[target];
    if (buffer != 0 && data) {
        auto& buf = g_vboShadow[buffer];
        if (offset + size > buf.size()) buf.resize(offset + size);
        memcpy(buf.data() + offset, data, size);
    }
    if (g_gpuOffloadMask & 16) glBufferSubData(target, offset, size, data);
}

extern "C" void Stub_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    for (int i = 0; i < n; i++) g_vboShadow.erase(buffers[i]);
    if (g_gpuOffloadMask & 16) glDeleteBuffers(n, buffers);
}

extern "C" void Stub_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    if (count <= 0 || !string) return;

    std::string fullSrc = "";
    for (int i = 0; i < count; i++) {
        const char* currentStr = string[i];
        if (!currentStr) continue;

        int currentLen = (length && length[i] >= 0) ? length[i] : strlen(currentStr);
        for (int j = 0; j < currentLen; j++) {
            char c = currentStr[j];
            if (c == '\0') break; 
            if (c >= 32 && c <= 126) fullSrc += c;
            else if (c == '\n' || c == '\r' || c == '\t') fullSrc += c;
        }
    }

    // БУЛЬДОЗЕР 3.0: Интеллектуальный разделитель шейдеров
    size_t mainPos = fullSrc.find("main");
    if (mainPos != std::string::npos) {
        size_t openBrace = fullSrc.find('{', mainPos);
        if (openBrace != std::string::npos) {
            // Ищем признаки второго шейдера ВНУТРИ функции main()
            size_t badPos1 = fullSrc.find("attribute ", openBrace);
            size_t badPos2 = fullSrc.find("varying ", openBrace);
            size_t badPos3 = fullSrc.find("uniform ", openBrace);
            size_t badPos4 = fullSrc.find("void main", openBrace);
            
            size_t cutPos = std::string::npos;
            if (badPos1 != std::string::npos) cutPos = std::min(cutPos, badPos1);
            if (badPos2 != std::string::npos) cutPos = std::min(cutPos, badPos2);
            if (badPos3 != std::string::npos) cutPos = std::min(cutPos, badPos3);
            if (badPos4 != std::string::npos) cutPos = std::min(cutPos, badPos4);
            
            if (cutPos != std::string::npos) {
                // В шейдер протек чужой код. Отрезаем его с корнем!
                fullSrc = fullSrc.substr(0, cutPos);
            }

            int braces = 0;
            size_t scan = openBrace;
            size_t lastValidPos = openBrace;
            
            for (; scan < fullSrc.length(); scan++) {
                if (fullSrc[scan] == '{') braces++;
                if (fullSrc[scan] == '}') braces--;
                
                if (fullSrc[scan] == ';' || fullSrc[scan] == '}' || fullSrc[scan] == '{') {
                    lastValidPos = scan;
                }
                
                if (braces == 0) {
                    fullSrc = fullSrc.substr(0, scan + 1);
                    break;
                }
            }
            
            if (braces > 0) {
                // Если скобка не закрылась, отрезаем хвост до последней точки с запятой
                fullSrc = fullSrc.substr(0, lastValidPos + 1);
                while (braces > 0) {
                    fullSrc += "\n}";
                    braces--;
                }
            }
        }
    }

    std::string patched = fullSrc;
    if (patched.find("precision") == std::string::npos) {
        std::string prec = "";
        if (patched.find("gl_FragColor") != std::string::npos || patched.find("texture2D") != std::string::npos) {
            prec = "precision mediump float;\n";
        } else if (patched.find("gl_Position") != std::string::npos) {
            prec = "precision highp float;\n";
        }
        if (!prec.empty()) {
            size_t verPos = patched.find("#version");
            if (verPos != std::string::npos) {
                size_t nl = patched.find('\n', verPos);
                if (nl != std::string::npos) patched.insert(nl + 1, prec);
                else patched = prec + patched;
            } else {
                patched = prec + patched;
            }
        }
    }

    if (patched.find("gl_Position") != std::string::npos) {
        std::string wrapper = "uniform float u_damn_rot;\n#define main damn_main\n";
        size_t verPos = patched.find("#version");
        if (verPos != std::string::npos) {
            size_t nl = patched.find('\n', verPos);
            if (nl != std::string::npos) patched.insert(nl + 1, wrapper);
            else patched = wrapper + patched;
        } else {
            patched = wrapper + patched;
        }
        patched += "\n#undef main\nvoid main() {\n  damn_main();\n  if (u_damn_rot > 0.5) gl_Position.xy = vec2(-gl_Position.y, gl_Position.x);\n}\n";
    }
    
    LogToJava("[SHADER-DUMP] Compiling Shader ID " + std::to_string(shader) + "\n=== CODE ===\n" + patched + "\n============");

    const GLchar* p = patched.c_str(); 
    GLint l = patched.length();
    glShaderSource(shader, 1, &p, &l); 
}

extern "C" void Stub_glCompileShader(GLuint shader) { glCompileShader(shader); GLint status = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &status); if (status == GL_FALSE) { GLint logLength = 0; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength); if (logLength > 0) { std::vector<char> log(logLength); glGetShaderInfoLog(shader, logLength, nullptr, log.data()); LogToJava(std::string("ОШИБКА КОМПИЛЯЦИИ: ") + log.data()); } } }
extern "C" void Stub_glLinkProgram(GLuint program) { glLinkProgram(program); GLint status = 0; glGetProgramiv(program, GL_LINK_STATUS, &status); if (status == GL_FALSE) { GLint logLength = 0; glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength); if (logLength > 0) { std::vector<char> log(logLength); glGetProgramInfoLog(program, logLength, nullptr, log.data()); LogToJava(std::string("ОШИБКА ЛИНКОВКИ: ") + log.data()); } } }

extern GLuint g_uiProgram;
extern GLuint g_textProgram;
extern GLuint g_dummyProgram;

// --- CPU TEXTURE CACHE ---
std::map<GLuint, std::vector<uint32_t>> g_cpuTextures;
std::map<GLuint, int> g_cpuTexW;
std::map<GLuint, int> g_cpuTexH;
GLuint g_cpuActiveTexture = 0;
extern int g_clientActiveTexture;

// Вспомогательная функция для вычисления точного размера текстуры в байтах
static inline size_t SafeGetGLTextureSize(GLsizei width, GLsizei height, GLenum format, GLenum type) {
    size_t bpp = 4;
    if (format == GL_RGB && type == 0x8363) bpp = 2; // GL_UNSIGNED_SHORT_5_6_5
    else if (format == GL_RGBA && type == 0x8033) bpp = 2; // GL_UNSIGNED_SHORT_4_4_4_4
    else if (format == GL_RGBA && type == 0x8034) bpp = 2; // GL_UNSIGNED_SHORT_5_5_5_1
    else if (format == GL_RGB && type == GL_UNSIGNED_BYTE) bpp = 3;
    else if (format == GL_ALPHA && type == GL_UNSIGNED_BYTE) bpp = 1;
    else if (format == 0x1909 && type == GL_UNSIGNED_BYTE) bpp = 1; // GL_LUMINANCE
    else if (format == 0x190A && type == GL_UNSIGNED_BYTE) bpp = 2; // GL_LUMINANCE_ALPHA
    else if (format == 0x80E1 && type == GL_UNSIGNED_BYTE) bpp = 4; // GL_BGRA_EXT

    // НАМЕРЕННО не вызываем glGetIntegerv(GL_UNPACK_ALIGNMENT) — это может вызвать
    // краш в MTK-драйвере при вызове из промежуточного состояния GL-контекста.
    // Используем alignment=1: чуть консервативнее по памяти, но полностью безопасно.
    return (size_t)width * (size_t)height * bpp;
}

extern "C" void Stub_glBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) g_cpuActiveTexture = texture;
    if (g_gpuOffloadMask & 8) glBindTexture(target, texture);
}

extern "C" void Stub_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    // Лог с hex-значениями для диагностики
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[GL-TEX] glTexImage2D: tex=%u w=%d h=%d intFmt=0x%X fmt=0x%X type=0x%X pixels=%d",
        g_cpuActiveTexture, width, height, (unsigned)internalformat, (unsigned)format, (unsigned)type, pixels != nullptr);
    LogToJava(logbuf);
    if (target == GL_TEXTURE_2D && level == 0) {
        g_cpuTexW[g_cpuActiveTexture] = width;
        g_cpuTexH[g_cpuActiveTexture] = height;
        std::vector<uint32_t>& texBuf = g_cpuTextures[g_cpuActiveTexture];
        texBuf.resize(width * height);
        
        if (!pixels) {
            std::fill(texBuf.begin(), texBuf.end(), 0xFF000000);
        } else {
            // Намеренно не вызываем glGetIntegerv(GL_UNPACK_ALIGNMENT) — может крашить MTK.
            // Используем alignment=4 (дефолт OpenGL ES; Wolf3D не меняет glPixelStorei).
            GLint align = 4;

            if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
                int rowLength = width * 4;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) memcpy(&texBuf[y*width], src + y*stride, width * 4);
            } else if (format == GL_RGB && type == GL_UNSIGNED_BYTE) {
                int rowLength = width * 3;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint8_t* row = src + y * stride;
                    for (int x=0; x<width; x++) texBuf[y*width + x] = 0xFF000000 | (row[x*3+2]<<16) | (row[x*3+1]<<8) | row[x*3];
                }
            } else if (format == GL_RGB && type == 0x8363) { // GL_UNSIGNED_SHORT_5_6_5
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint16_t* row = (const uint16_t*)(src + y * stride);
                    for (int x=0; x<width; x++) {
                        uint16_t px = row[x];
                        uint8_t r = (px >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                        uint8_t g = (px >> 5) & 0x3F;  g = (g << 2) | (g >> 4);
                        uint8_t b = px & 0x1F;         b = (b << 3) | (b >> 2);
                        texBuf[y*width + x] = 0xFF000000 | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == GL_RGBA && type == 0x8033) { // GL_UNSIGNED_SHORT_4_4_4_4
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint16_t* row = (const uint16_t*)(src + y * stride);
                    for (int x=0; x<width; x++) {
                        uint16_t px = row[x];
                        uint8_t r = (px >> 12) & 0xF; r = (r << 4) | r;
                        uint8_t g = (px >> 8) & 0xF;  g = (g << 4) | g;
                        uint8_t b = (px >> 4) & 0xF;  b = (b << 4) | b;
                        uint8_t a = px & 0xF;         a = (a << 4) | a;
                        texBuf[y*width + x] = (a<<24) | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == GL_RGBA && type == 0x8034) { // GL_UNSIGNED_SHORT_5_5_5_1
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint16_t* row = (const uint16_t*)(src + y * stride);
                    for (int x=0; x<width; x++) {
                        uint16_t px = row[x];
                        uint8_t r = (px >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                        uint8_t g = (px >> 6) & 0x1F;  g = (g << 3) | (g >> 2);
                        uint8_t b = (px >> 1) & 0x1F;  b = (b << 3) | (b >> 2);
                        uint8_t a = (px & 0x1) ? 255 : 0;
                        texBuf[y*width + x] = (a<<24) | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == 0x80E1 && type == GL_UNSIGNED_BYTE) { // GL_BGRA_EXT
                int rowLength = width * 4;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint32_t* row = (const uint32_t*)(src + y * stride);
                    for (int x=0; x<width; x++) {
                        uint32_t px = row[x];
                        uint8_t b = px & 0xFF; uint8_t g = (px >> 8) & 0xFF;
                        uint8_t r = (px >> 16) & 0xFF; uint8_t a = (px >> 24) & 0xFF;
                        texBuf[y*width + x] = (a<<24) | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == 0x1909 && type == GL_UNSIGNED_BYTE) { // GL_LUMINANCE
                int rowLength = width * 1;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint8_t* row = src + y * stride;
                    for (int x=0; x<width; x++) { uint8_t l = row[x]; texBuf[y*width + x] = 0xFF000000 | (l<<16) | (l<<8) | l; }
                }
            } else if (format == 0x190A && type == GL_UNSIGNED_BYTE) { // GL_LUMINANCE_ALPHA
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint8_t* row = src + y * stride;
                    for (int x=0; x<width; x++) { uint8_t l = row[x*2]; uint8_t a = row[x*2+1]; texBuf[y*width + x] = (a<<24) | (l<<16) | (l<<8) | l; }
                }
            } else if (format == GL_ALPHA && type == GL_UNSIGNED_BYTE) {
                int rowLength = width * 1;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y=0; y<height; y++) {
                    const uint8_t* row = src + y * stride;
                    for (int x=0; x<width; x++) texBuf[y*width + x] = (row[x]<<24) | 0x00FFFFFF;
                }
            } else {
                std::fill(texBuf.begin(), texBuf.end(), 0xFFFF00FF);
            }
        }
    }

    // --- БЕЗОПАСНАЯ ОТПРАВКА В ЖЕЛЕЗО (ИСПРАВЛЕНИЕ КРАША MTK) ---

    GLenum hw_format = format;
    GLenum hw_internalformat = (GLenum)internalformat;

    // Нормализуем GLES1-стиль internalformat (числа 1–4) в реальные enum-ы GLES2.
    // iOS/GLES1.1 позволял передавать кол-во компонент (1=LUMINANCE, 2=LUMINANCE_ALPHA,
    // 3=RGB, 4=RGBA). MTK-драйвер на GLES2 это не понимает и крашит.
    if (hw_internalformat == 1) hw_internalformat = GL_LUMINANCE;
    else if (hw_internalformat == 2) hw_internalformat = GL_LUMINANCE_ALPHA;
    else if (hw_internalformat == 3) hw_internalformat = GL_RGB;
    else if (hw_internalformat == 4) hw_internalformat = GL_RGBA;
    // Нормализуем нестандартные значения (BGRA_EXT как internalformat, и пр.)
    else if (hw_internalformat == 0x80E1) hw_internalformat = GL_RGBA; // GL_BGRA_EXT
    else if (hw_internalformat != GL_RGBA && hw_internalformat != GL_RGB &&
             hw_internalformat != GL_ALPHA && hw_internalformat != GL_LUMINANCE &&
             hw_internalformat != GL_LUMINANCE_ALPHA) {
        hw_internalformat = hw_format; // Fallback: берём format
    }

    std::vector<uint8_t> converted_buf;
    const GLvoid* safe_pixels = pixels;

    if (format == 0x80E1 && type == GL_UNSIGNED_BYTE) {
        // GL_BGRA_EXT -> GL_RGBA: свапаем R и B каналы
        hw_format = GL_RGBA;
        hw_internalformat = GL_RGBA;
        if (pixels) {
            size_t totalBytes = (size_t)width * height * 4;
            converted_buf.resize(totalBytes);
            const uint8_t* src = (const uint8_t*)pixels;
            uint8_t* dst = converted_buf.data();
            for (size_t i = 0; i < (size_t)width * height; i++) {
                dst[i*4+0] = src[i*4+2]; // R <- B
                dst[i*4+1] = src[i*4+1]; // G
                dst[i*4+2] = src[i*4+0]; // B <- R
                dst[i*4+3] = src[i*4+3]; // A
            }
            safe_pixels = converted_buf.data();
        }
    } else if (pixels != nullptr) {
        // Всегда копируем в свой буфер — MTK требует выровненную память
        size_t totalBytes = SafeGetGLTextureSize(width, height, format, type);
        if (totalBytes > 0) {
            converted_buf.resize(totalBytes);
            memcpy(converted_buf.data(), pixels, totalBytes);
            safe_pixels = converted_buf.data();
        }
    }
    if (g_gpuOffloadMask & 8) glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

extern "C" void Stub_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {
    if (target == GL_TEXTURE_2D && level == 0 && pixels && g_cpuTextures.count(g_cpuActiveTexture)) {
        std::vector<uint32_t>& texBuf = g_cpuTextures[g_cpuActiveTexture];
        int texW = g_cpuTexW[g_cpuActiveTexture];
        int texH = g_cpuTexH[g_cpuActiveTexture];
        
        if (xoffset >= 0 && yoffset >= 0 && xoffset + width <= texW && yoffset + height <= texH) {
            // Не вызываем glGetIntegerv(GL_UNPACK_ALIGNMENT) — может крашить MTK.
            // Используем дефолтный alignment=4.
            GLint align = 4;

            if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
                int rowLength = width * 4;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y = 0; y < height; y++) {
                    memcpy(&texBuf[(yoffset + y) * texW + xoffset], src + y * stride, width * 4);
                }
            } else if (format == GL_RGB && type == GL_UNSIGNED_BYTE) {
                int rowLength = width * 3;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y = 0; y < height; y++) {
                    const uint8_t* row = src + y * stride;
                    for (int x = 0; x < width; x++) {
                        texBuf[(yoffset + y) * texW + (xoffset + x)] = 0xFF000000 | (row[x*3+2]<<16) | (row[x*3+1]<<8) | row[x*3];
                    }
                }
            } else if (format == GL_RGB && type == 0x8363) { // GL_UNSIGNED_SHORT_5_6_5
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y = 0; y < height; y++) {
                    const uint16_t* row = (const uint16_t*)(src + y * stride);
                    for (int x = 0; x < width; x++) {
                        uint16_t px = row[x];
                        uint8_t r = (px >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                        uint8_t g = (px >> 5) & 0x3F;  g = (g << 2) | (g >> 4);
                        uint8_t b = px & 0x1F;         b = (b << 3) | (b >> 2);
                        texBuf[(yoffset + y) * texW + (xoffset + x)] = 0xFF000000 | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == GL_RGBA && type == 0x8033) { // GL_UNSIGNED_SHORT_4_4_4_4
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y = 0; y < height; y++) {
                    const uint16_t* row = (const uint16_t*)(src + y * stride);
                    for (int x = 0; x < width; x++) {
                        uint16_t px = row[x];
                        uint8_t r = (px >> 12) & 0xF; r = (r << 4) | r;
                        uint8_t g = (px >> 8) & 0xF;  g = (g << 4) | g;
                        uint8_t b = (px >> 4) & 0xF;  b = (b << 4) | b;
                        uint8_t a = px & 0xF;         a = (a << 4) | a;
                        texBuf[(yoffset + y) * texW + (xoffset + x)] = (a<<24) | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == GL_RGBA && type == 0x8034) { // GL_UNSIGNED_SHORT_5_5_5_1
                int rowLength = width * 2;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y = 0; y < height; y++) {
                    const uint16_t* row = (const uint16_t*)(src + y * stride);
                    for (int x = 0; x < width; x++) {
                        uint16_t px = row[x];
                        uint8_t r = (px >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                        uint8_t g = (px >> 6) & 0x1F;  g = (g << 3) | (g >> 2);
                        uint8_t b = (px >> 1) & 0x1F;  b = (b << 3) | (b >> 2);
                        uint8_t a = (px & 0x1) ? 255 : 0;
                        texBuf[(yoffset + y) * texW + (xoffset + x)] = (a<<24) | (b<<16) | (g<<8) | r;
                    }
                }
            } else if (format == GL_ALPHA && type == GL_UNSIGNED_BYTE) {
                int rowLength = width * 1;
                int stride = rowLength + ((align - (rowLength % align)) % align);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int y = 0; y < height; y++) {
                    const uint8_t* row = src + y * stride;
                    for (int x = 0; x < width; x++) {
                        texBuf[(yoffset + y) * texW + (xoffset + x)] = (row[x]<<24) | 0x00FFFFFF;
                    }
                }
            }
        }
    }
    if (g_gpuOffloadMask & 8) glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

extern "C" void Stub_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid *data) {
    LogToJava("[GL-TEX] glCompressedTexSubImage2D called!");
    if (g_gpuOffloadMask & 8) glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}

extern "C" void Stub_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data) {
    LogToJava("[GL-TEX] glCompressedTexImage2D: tex=" + std::to_string(g_cpuActiveTexture) + " w=" + std::to_string(width) + " h=" + std::to_string(height) + " intFmt=0x" + std::to_string(internalformat) + " size=" + std::to_string(imageSize));
    if (target == GL_TEXTURE_2D && level == 0) {
        g_cpuTexW[g_cpuActiveTexture] = width;
        g_cpuTexH[g_cpuActiveTexture] = height;
        std::vector<uint32_t>& texBuf = g_cpuTextures[g_cpuActiveTexture];
        texBuf.resize(width * height);
        
        // PVRTC не реализован на CPU. Рисуем серую шахматку.
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                bool isDark = ((x / 8) % 2) == ((y / 8) % 2);
                texBuf[y * width + x] = isDark ? 0xFF888888 : 0xFFCCCCCC; 
            }
        }
        LogToJava("HLE_WARNING: Stub_glCompressedTexImage2D PVRTC перехвачен! Заменено на шахматку.");
    }
    if (g_gpuOffloadMask & 8) glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}

std::map<GLint, std::vector<float>> g_uniformShadowFloat;

struct SWVertex { float x, y, z, w, invW, r, g, b, a, u, v; };

void CPUExtractAndDraw(GLenum drawMode, GLint first, GLsizei count, const GLvoid* indices, GLenum type) {
    std::vector<uint32_t>* targetColorBuf = &g_cpuColorBuffer;
    std::vector<float>* targetDepthBuf = &g_cpuDepthBuffer;
    int targetW = g_surfaceWidth;
    int targetH = g_surfaceHeight;

    if (g_lastActiveFBO != 0 && g_fboColorTex.count(g_lastActiveFBO)) {
        GLuint tex = g_fboColorTex[g_lastActiveFBO];
        if (tex != 0 && g_cpuTexW.count(tex) && g_cpuTexW[tex] > 0) {
            targetW = g_cpuTexW[tex];
            targetH = g_cpuTexH[tex];
            targetColorBuf = &g_cpuTextures[tex];
            if (g_fboDepthBuf[g_lastActiveFBO].size() != (size_t)(targetW * targetH)) {
                g_fboDepthBuf[g_lastActiveFBO].resize(targetW * targetH, 1.0f);
            }
            targetDepthBuf = &g_fboDepthBuf[g_lastActiveFBO];
        }
    }

    if (targetDepthBuf->size() != targetColorBuf->size()) {
        targetDepthBuf->assign(targetColorBuf->size(), 1.0f);
    }
    SyncLog("[RENDER] Анализ вызова отрисовки...");
    
    GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);

    GLint ibo = g_boundBuffers[0x8893]; // Читаем GL_ELEMENT_ARRAY_BUFFER напрямую из кэша враппера
    uint8_t* iboPtr = nullptr;
    if (ibo != 0) {
        if (g_vboShadow.count(ibo)) iboPtr = g_vboShadow[ibo].data();
    }

    GLint posLoc = -1, colorLoc = -1, texLoc = -1, idxLoc = -1, weightLoc = -1;
    if (prog > 0) {
        GLint numActiveAttrs = 0;
        glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &numActiveAttrs);
        for (int i = 0; i < numActiveAttrs; i++) {
            char name[256]; GLsizei len; GLint a_size; GLenum a_type;
            glGetActiveAttrib(prog, i, sizeof(name), &len, &a_size, &a_type, name);
            GLint loc = -1;
            if (g_attribLocations[prog].count(name)) loc = g_attribLocations[prog][name];
            else loc = glGetAttribLocation(prog, name);
            
            if (strstr(name, "Pos") || strstr(name, "pos") || strstr(name, "Vert") || strstr(name, "vert")) posLoc = loc;
            else if (strstr(name, "Norm") || strstr(name, "norm")) { /* ignore */ }
            else if (strstr(name, "Color") || strstr(name, "color") || strstr(name, "Col") || strstr(name, "col") || strstr(name, "Diff") || strstr(name, "diff")) colorLoc = loc;
            else if (strstr(name, "Tex") || strstr(name, "tex") || strstr(name, "UV") || strstr(name, "uv")) texLoc = loc;
            else if (strstr(name, "Weig") || strstr(name, "weig") || strstr(name, "Wt") || strstr(name, "wt")) weightLoc = loc;
            else if (strstr(name, "Ind") || strstr(name, "ind") || strstr(name, "Idx") || strstr(name, "idx") || strstr(name, "Bone") || strstr(name, "bone") || strstr(name, "Mat") || strstr(name, "mat")) idxLoc = loc;
        }
    }
    
    // БРОНЕБОЙНЫЙ ФОЛБЕК: В GLES 2.0 мы НЕ ДОЛЖНЫ жестко привязывать Color к 1
    if (prog == 0) {
        if (posLoc == -1) posLoc = 0;
        if (colorLoc == -1) colorLoc = 1;
        if (texLoc == -1) texLoc = 2 + g_clientActiveTexture;
    } else {
        if (posLoc == -1) posLoc = 0;
    }

    GLint posEnabled = 0; 
    if (posLoc != -1 && posLoc < 16) posEnabled = g_vertexAttribs[posLoc].enabled;
    if (!posEnabled) { SyncLog("[RENDER] Атрибут позиции отключен."); return; }

            float mvp[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            float diffuseUniform[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float texMat[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            bool hasTexMat = false;
            GLint paletteLoc = -1;
            int paletteSize = 0;
            std::vector<float> bonePalette;
            bool isPackedVec4 = false;
        if (prog > 0) {
            GLint numUniforms = 0; glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &numUniforms);
            for (int i = 0; i < numUniforms; i++) {
                char name[256]; GLsizei len; GLint u_size; GLenum u_type;
                glGetActiveUniform(prog, i, sizeof(name), &len, &u_size, &u_type, name);
                if (u_type == GL_FLOAT_MAT4 && (strstr(name, "WVP") || strstr(name, "MVP") || strstr(name, "Projection"))) {
                    GLint loc = glGetUniformLocation(prog, name);
                    if (g_uniformShadowFloat.count(loc) && g_uniformShadowFloat[loc].size() >= 16) memcpy(mvp, g_uniformShadowFloat[loc].data(), 16 * sizeof(float));
                }
                if (u_type == GL_FLOAT_VEC4 && (strstr(name, "Diffuse") || strstr(name, "Color"))) {
                    GLint loc = glGetUniformLocation(prog, name);
                    if (g_uniformShadowFloat.count(loc) && g_uniformShadowFloat[loc].size() >= 4) memcpy(diffuseUniform, g_uniformShadowFloat[loc].data(), 4 * sizeof(float));
                }
                if (u_type == GL_FLOAT_MAT4 && (strstr(name, "Texture") || strstr(name, "TexMatrix"))) {
                    GLint loc = glGetUniformLocation(prog, name);
                    if (g_uniformShadowFloat.count(loc) && g_uniformShadowFloat[loc].size() >= 16) memcpy(texMat, g_uniformShadowFloat[loc].data(), 16 * sizeof(float));
                    hasTexMat = true;
                }
                if ((u_type == GL_FLOAT_MAT4 || u_type == 0x8B52 /*GL_FLOAT_VEC4*/) && 
                   (strstr(name, "alette") || strstr(name, "one") || strstr(name, "atrix") || strstr(name, "atrices"))) {
                    paletteLoc = glGetUniformLocation(prog, name);
                    
                    int uploadedFloats = g_uniformShadowFloat.count(paletteLoc) ? g_uniformShadowFloat[paletteLoc].size() : 0;
                    
                    if (u_type == 0x8B52) { 
                        isPackedVec4 = true;
                        int numVec4s = uploadedFloats / 4;
                        if (numVec4s < u_size) numVec4s = u_size;
                        paletteSize = numVec4s / 3;
                        bonePalette.resize(paletteSize * 16, 0.0f);
                        const float* src = g_uniformShadowFloat.count(paletteLoc) ? g_uniformShadowFloat[paletteLoc].data() : nullptr;
                        if (src) {
                            for (int i = 0; i < paletteSize; i++) {
                                bonePalette[i*16 + 0] = src[i*12 + 0]; bonePalette[i*16 + 4] = src[i*12 + 1]; bonePalette[i*16 + 8] = src[i*12 + 2]; bonePalette[i*16 + 12] = src[i*12 + 3];
                                bonePalette[i*16 + 1] = src[i*12 + 4]; bonePalette[i*16 + 5] = src[i*12 + 5]; bonePalette[i*16 + 9] = src[i*12 + 6]; bonePalette[i*16 + 13] = src[i*12 + 7];
                                bonePalette[i*16 + 2] = src[i*12 + 8]; bonePalette[i*16 + 6] = src[i*12 + 9]; bonePalette[i*16 + 10] = src[i*12 + 10]; bonePalette[i*16 + 14] = src[i*12 + 11];
                                bonePalette[i*16 + 3] = 0.0f;          bonePalette[i*16 + 7] = 0.0f;          bonePalette[i*16 + 11] = 0.0f;           bonePalette[i*16 + 15] = 1.0f;
                            }
                        }
                    } else {
                        paletteSize = uploadedFloats / 16;
                        if (paletteSize < u_size) paletteSize = u_size;
                        bonePalette.resize(paletteSize * 16, 0.0f);
                        if (uploadedFloats > 0) memcpy(bonePalette.data(), g_uniformShadowFloat[paletteLoc].data(), uploadedFloats * sizeof(float));
                    }
                }
            }
        } else {
            // Перемножаем матрицы для ES 1.1 FFP (Projection * ModelView)
            const float* p = g_projectionStack.back().data();
            const float* m = g_modelViewStack.back().data();
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    mvp[c * 4 + r] = p[0 * 4 + r] * m[c * 4 + 0] +
                                     p[1 * 4 + r] * m[c * 4 + 1] +
                                     p[2 * 4 + r] * m[c * 4 + 2] +
                                     p[3 * 4 + r] * m[c * 4 + 3];
                }
            }
        }

    
    auto getTypeSize = [](GLenum t) -> int {
        if (t == GL_FLOAT || t == 0x140C || t == GL_INT || t == GL_UNSIGNED_INT) return 4;
        if (t == GL_SHORT || t == GL_UNSIGNED_SHORT || t == 0x8D61) return 2;
        return 1;
    };
    
    GLvoid* pPtr = nullptr; GLint pSize=0, pStride=0, pType=0; GLint pVBO = 0;
    if (posLoc != -1 && posLoc < 16) {
        pPtr = (GLvoid*)g_vertexAttribs[posLoc].pointer;
        pSize = g_vertexAttribs[posLoc].size;
        pStride = g_vertexAttribs[posLoc].stride;
        pType = g_vertexAttribs[posLoc].type;
        pVBO = g_vertexAttribs[posLoc].vbo;
        if (pStride == 0) pStride = pSize * getTypeSize(pType);
    }
    
    GLvoid* cPtr = nullptr; GLint cSize=0, cStride=0, cType=0; GLint cVBO = 0; GLint cEnabled = 0; 
    if (colorLoc != -1 && colorLoc < 16) {
        cEnabled = g_vertexAttribs[colorLoc].enabled;
        if (cEnabled) {
            cPtr = (GLvoid*)g_vertexAttribs[colorLoc].pointer;
            cSize = g_vertexAttribs[colorLoc].size;
            cStride = g_vertexAttribs[colorLoc].stride;
            cType = g_vertexAttribs[colorLoc].type;
            cVBO = g_vertexAttribs[colorLoc].vbo;
            if (cStride == 0) cStride = cSize * getTypeSize(cType);
        }
    }

    GLvoid* tPtr = nullptr; GLint tSize=0, tStride=0, tType=0; GLint tVBO = 0; GLint tEnabled = 0; 
    if (texLoc != -1 && texLoc < 16) {
        tEnabled = g_vertexAttribs[texLoc].enabled;
        if (tEnabled) {
            tPtr = (GLvoid*)g_vertexAttribs[texLoc].pointer;
            tSize = g_vertexAttribs[texLoc].size;
            tStride = g_vertexAttribs[texLoc].stride;
            tType = g_vertexAttribs[texLoc].type;
            tVBO = g_vertexAttribs[texLoc].vbo;
            if (tStride == 0) tStride = tSize * getTypeSize(tType);
        }
    }

    GLvoid* iPtr = nullptr; GLint iSize=0, iStride=0, iType=0; GLint iVBO = 0; GLint iEnabled = 0; 
    if (idxLoc != -1 && idxLoc < 16) {
        iEnabled = g_vertexAttribs[idxLoc].enabled;
        if (iEnabled) {
            iPtr = (GLvoid*)g_vertexAttribs[idxLoc].pointer;
            iSize = g_vertexAttribs[idxLoc].size;
            iStride = g_vertexAttribs[idxLoc].stride;
            iType = g_vertexAttribs[idxLoc].type;
            iVBO = g_vertexAttribs[idxLoc].vbo;
            if (iStride == 0) iStride = iSize * getTypeSize(iType);
        }
    }

    GLvoid* wPtr = nullptr; GLint wSize=0, wStride=0, wType=0; GLint wVBO = 0; GLint wEnabled = 0; 
    if (weightLoc != -1 && weightLoc < 16) {
        wEnabled = g_vertexAttribs[weightLoc].enabled;
        if (wEnabled) {
            wPtr = (GLvoid*)g_vertexAttribs[weightLoc].pointer;
            wSize = g_vertexAttribs[weightLoc].size;
            wStride = g_vertexAttribs[weightLoc].stride;
            wType = g_vertexAttribs[weightLoc].type;
            wVBO = g_vertexAttribs[weightLoc].vbo;
            if (wStride == 0) wStride = wSize * getTypeSize(wType);
        }
    }

    uint8_t* pBase = (pVBO != 0 && g_vboShadow.count(pVBO)) ? (g_vboShadow[pVBO].data() + (uintptr_t)pPtr) : (uint8_t*)pPtr;
    uint8_t* cBase = (cVBO != 0 && g_vboShadow.count(cVBO)) ? (g_vboShadow[cVBO].data() + (uintptr_t)cPtr) : (uint8_t*)cPtr;
    uint8_t* tBase = (tVBO != 0 && g_vboShadow.count(tVBO)) ? (g_vboShadow[tVBO].data() + (uintptr_t)tPtr) : (uint8_t*)tPtr;
    uint8_t* iBase = (iVBO != 0 && g_vboShadow.count(iVBO)) ? (g_vboShadow[iVBO].data() + (uintptr_t)iPtr) : (uint8_t*)iPtr;
    uint8_t* wBaseES2 = (wVBO != 0 && g_vboShadow.count(wVBO)) ? (g_vboShadow[wVBO].data() + (uintptr_t)wPtr) : (uint8_t*)wPtr;

    uint8_t* mIdxBase = nullptr; uint8_t* wBase = nullptr;
    int mIdxStride = 0; int wStrideVal = 0;
    if (prog == 0 && g_matrixPaletteEnabled) {
        if (g_matrixIndexEnabled || g_matrixIndexPointer != nullptr) {
            mIdxBase = (g_matrixIndexVBO != 0 && g_vboShadow.count(g_matrixIndexVBO)) ? (g_vboShadow[g_matrixIndexVBO].data() + (uintptr_t)g_matrixIndexPointer) : (uint8_t*)g_matrixIndexPointer;
            mIdxStride = g_matrixIndexStride;
            if (mIdxStride == 0) mIdxStride = g_matrixIndexSize * getTypeSize(g_matrixIndexType);
        }
        if (g_weightEnabled || g_weightPointer != nullptr) {
            wBase = (g_weightVBO != 0 && g_vboShadow.count(g_weightVBO)) ? (g_vboShadow[g_weightVBO].data() + (uintptr_t)g_weightPointer) : (uint8_t*)g_weightPointer;
            wStrideVal = g_weightStride;
            if (wStrideVal == 0) wStrideVal = g_weightSize * getTypeSize(g_weightType);
        }
    }

    size_t pVboSize = (pVBO != 0 && g_vboShadow.count(pVBO)) ? g_vboShadow[pVBO].size() : 0;
    size_t cVboSize = (cVBO != 0 && g_vboShadow.count(cVBO)) ? g_vboShadow[cVBO].size() : 0;
    size_t tVboSize = (tVBO != 0 && g_vboShadow.count(tVBO)) ? g_vboShadow[tVBO].size() : 0;
    size_t iboSize  = (ibo != 0 && g_vboShadow.count(ibo)) ? g_vboShadow[ibo].size() : 0;

    static std::vector<SWVertex> extractedVerts;
    extractedVerts.resize(count);
    int validVertsCount = 0;
    for (int i = 0; i < count; i++) {
        int idx = first + i;
        if (ibo != 0) {
            if (iboPtr) {
                size_t offset = (uintptr_t)indices + i * (type == GL_UNSIGNED_BYTE ? 1 : 2);
                if (offset + (type == GL_UNSIGNED_BYTE ? 1 : 2) <= iboSize) {
                    idx = (type == GL_UNSIGNED_BYTE) ? iboPtr[offset] : *(uint16_t*)(iboPtr + offset);
                } else continue;
            } else continue;
        } else if (indices != nullptr) {
            idx = (type == GL_UNSIGNED_BYTE) ? ((uint8_t*)indices)[i] : ((uint16_t*)indices)[i];
        }
        
        auto halfToFloat = [](uint16_t h) -> float {
            union { uint32_t u; float f; } r;
            int s = (h >> 15) & 1; int e = (h >> 10) & 31; int f = h & 1023;
            if (e == 0) { r.u = (s << 31); return r.f; }
            if (e == 31) { r.u = (s << 31) | 0x7f800000 | (f << 13); return r.f; }
            r.u = (s << 31) | ((e + 112) << 23) | (f << 13);
            return r.f;
        };
        auto readG = [&halfToFloat](uint8_t* p, GLenum t, int o) -> float {
            if (!p) return 0.0f;
            if (t == GL_FLOAT) { float v; memcpy(&v, p + o * 4, 4); return v; }
            if (t == 0x8D61) { uint16_t v; memcpy(&v, p + o * 2, 2); return halfToFloat(v); }
            if (t == 0x140C) { int32_t v; memcpy(&v, p + o * 4, 4); return v * 0.00001525878f; }
            if (t == GL_SHORT) { int16_t v; memcpy(&v, p + o * 2, 2); return v; }
            if (t == 0x1403) { uint16_t v; memcpy(&v, p + o * 2, 2); return v; }
            if (t == GL_UNSIGNED_BYTE) return p[o];
            return 0.0f;
        };
        auto readC = [&halfToFloat](uint8_t* p, GLenum t, int o) -> float {
            if (!p) return 1.0f;
            if (t == GL_FLOAT) { float v; memcpy(&v, p + o * 4, 4); return v; }
            if (t == GL_UNSIGNED_BYTE) return p[o] * 0.003921568f;
            if (t == 0x140C) { int32_t v; memcpy(&v, p + o * 4, 4); return v * 0.00001525878f; }
            if (t == GL_SHORT) { int16_t v; memcpy(&v, p + o * 2, 2); return v * 0.00003051757f; }
            if (t == 0x1403) { uint16_t v; memcpy(&v, p + o * 2, 2); return v * 0.00001525878f; }
            if (t == 0x8D61) { uint16_t v; memcpy(&v, p + o * 2, 2); return halfToFloat(v); }
            return 1.0f;
        };

        float px = 0, py = 0, pz = 0, pw = 1;
        if (posLoc != -1 && posLoc < 16) {
            px = g_vertexAttribs[posLoc].constantValue[0];
            py = g_vertexAttribs[posLoc].constantValue[1];
            pz = g_vertexAttribs[posLoc].constantValue[2];
            pw = g_vertexAttribs[posLoc].constantValue[3];
        }
        if (pBase) {
            uint8_t* v = pBase + idx * pStride;
            px = readG(v, pType, 0); if (pSize > 1) py = readG(v, pType, 1); if (pSize > 2) pz = readG(v, pType, 2); if (pSize > 3) pw = readG(v, pType, 3);
        }
        
        float cr = 1, cg = 1, cb = 1, ca = 1;
        int activeColorLoc = (colorLoc != -1) ? colorLoc : 1;
        if (activeColorLoc < 16) {
            cr = g_vertexAttribs[activeColorLoc].constantValue[0];
            cg = g_vertexAttribs[activeColorLoc].constantValue[1];
            cb = g_vertexAttribs[activeColorLoc].constantValue[2];
            ca = g_vertexAttribs[activeColorLoc].constantValue[3];
        }
        if (cBase && cEnabled) {
            uint8_t* v = cBase + idx * cStride;
            cr = readC(v, cType, 0); if (cSize > 1) cg = readC(v, cType, 1); if (cSize > 2) cb = readC(v, cType, 2); if (cSize > 3) ca = readC(v, cType, 3);
        }

        float tu = 0, tv = 0;
        int activeTexLoc = (texLoc != -1) ? texLoc : (2 + g_clientActiveTexture);
        if (activeTexLoc < 16) {
            tu = g_vertexAttribs[activeTexLoc].constantValue[0];
            tv = g_vertexAttribs[activeTexLoc].constantValue[1];
        }
        if (tBase && tEnabled) {
            uint8_t* v = tBase + idx * tStride;
            tu = readG(v, tType, 0); if (tSize > 1) tv = readG(v, tType, 1);
        }

        if (prog > 0 && hasTexMat) {
            float tempU = tu * texMat[0] + tv * texMat[4] + texMat[12];
            float tempV = tu * texMat[1] + tv * texMat[5] + texMat[13];
            tu = tempU; tv = tempV;
        } else if (prog == 0) {
            const float* tm = g_textureStack.back().data();
            float tempU = tu * tm[0] + tv * tm[4] + tm[12];
            float tempV = tu * tm[1] + tv * tm[5] + tm[13];
            tu = tempU; tv = tempV;
        }

        // --- ES 2.0 SKINNING ---
        if (prog > 0 && paletteLoc != -1) {
            int num_matrices = iSize > 0 ? iSize : 1; 
            if (num_matrices > 4) num_matrices = 4;
            
            float w_val[4] = {0, 0, 0, 0};
            int m_idx[4] = {0, 0, 0, 0};
            float current_sum = 0.0f;
            
            for (int w_i = 0; w_i < num_matrices; w_i++) {
                if (wEnabled && wBaseES2) {
                    if (w_i < wSize) {
                        uint8_t* wPtr_curr = wBaseES2 + idx * wStride;
                        w_val[w_i] = readC(wPtr_curr, wType, w_i);
                        current_sum += w_val[w_i];
                    } else if (w_i == wSize) {
                        w_val[w_i] = 1.0f - current_sum;
                    } else {
                        w_val[w_i] = 0.0f;
                    }
                } else if (weightLoc != -1 && weightLoc < 16) {
                    w_val[w_i] = g_vertexAttribs[weightLoc].constantValue[w_i];
                } else {
                    w_val[w_i] = (w_i == 0) ? 1.0f : 0.0f;
                }
                
                if (iEnabled && iBase && iSize > 0) {
                    uint8_t* idxPtr = iBase + idx * iStride;
                    m_idx[w_i] = (int)readG(idxPtr, iType, w_i);
                } else if (idxLoc != -1 && idxLoc < 16) {
                    m_idx[w_i] = (int)g_vertexAttribs[idxLoc].constantValue[w_i];
                } else {
                    m_idx[w_i] = 0;
                }
            }
            
            float total_weight = 0.0f;
            for (int w_i = 0; w_i < num_matrices; w_i++) {
                if (w_val[w_i] < 0.0f) w_val[w_i] = 0.0f;
                total_weight += w_val[w_i];
            }
            
            if (total_weight > 0.001f) {
                for (int w_i = 0; w_i < num_matrices; w_i++) w_val[w_i] /= total_weight;
            } else {
                w_val[0] = 1.0f;
                for (int w_i = 1; w_i < num_matrices; w_i++) w_val[w_i] = 0.0f;
            }
            
            float blended_m[16] = {0};
            for (int w_i = 0; w_i < num_matrices; w_i++) {
                if (w_val[w_i] > 0.0001f && m_idx[w_i] != 255) {
                    int matIdx = m_idx[w_i];
                    
                    // Если шейдер использует vec4 массив, индексы в VBO лежат умноженные на 3 (0, 3, 6, 9... 72...)
                    if (isPackedVec4) {
                        if (matIdx > 0 && matIdx % 3 == 0) matIdx /= 3;
                    }
                    
                    const float* palM = nullptr;
                    if (matIdx >= 0 && matIdx < paletteSize) palM = &bonePalette[matIdx * 16];
                    else if (paletteSize > 0) palM = &bonePalette[0]; // Безопасный фолбек на root
                    else {
                        static const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                        palM = id;
                    }
                    
                    for (int k = 0; k < 16; k++) blended_m[k] += palM[k] * w_val[w_i];
                }
            }
            
            blended_m[3] = 0.0f; blended_m[7] = 0.0f; blended_m[11] = 0.0f; blended_m[15] = 1.0f;
            
            float spx = px * blended_m[0] + py * blended_m[4] + pz * blended_m[8] + pw * blended_m[12];
            float spy = px * blended_m[1] + py * blended_m[5] + pz * blended_m[9] + pw * blended_m[13];
            float spz = px * blended_m[2] + py * blended_m[6] + pz * blended_m[10] + pw * blended_m[14];
            float spw = pw; // W не должно меняться костями
            px = spx; py = spy; pz = spz; pw = spw;
        }

        float v_mvp[16];
        if (prog == 0 && g_matrixPaletteEnabled) {
            int num_matrices = g_matrixIndexSize > 0 ? g_matrixIndexSize : 1; 
            if (num_matrices > 4) num_matrices = 4;
            
            float w_val[4] = {0, 0, 0, 0};
            int m_idx[4] = {0, 0, 0, 0};
            float current_sum = 0.0f;
            
            for (int w_i = 0; w_i < num_matrices; w_i++) {
                if (g_weightEnabled && wBase) {
                    if (w_i < g_weightSize) {
                        uint8_t* wPtr = wBase + idx * wStrideVal;
                        w_val[w_i] = readC(wPtr, g_weightType, w_i);
                        current_sum += w_val[w_i];
                    } else if (w_i == g_weightSize) {
                        w_val[w_i] = 1.0f - current_sum;
                    } else {
                        w_val[w_i] = 0.0f;
                    }
                } else {
                    w_val[w_i] = (w_i == 0) ? 1.0f : 0.0f;
                }
                
                if (g_matrixIndexEnabled && mIdxBase && g_matrixIndexSize > 0) {
                    uint8_t* idxPtr = mIdxBase + idx * mIdxStride;
                    m_idx[w_i] = (int)readG(idxPtr, g_matrixIndexType, w_i);
                } else {
                    m_idx[w_i] = (w_i == 0) ? g_currentPaletteMatrix : 0;
                }
            }
            
            float total_weight = 0.0f;
            for (int w_i = 0; w_i < num_matrices; w_i++) {
                if (w_val[w_i] < 0.0f) w_val[w_i] = 0.0f;
                total_weight += w_val[w_i];
            }
            
            if (total_weight > 0.001f) {
                for (int w_i = 0; w_i < num_matrices; w_i++) w_val[w_i] /= total_weight;
            } else {
                w_val[0] = 1.0f;
                for (int w_i = 1; w_i < num_matrices; w_i++) w_val[w_i] = 0.0f;
            }
            
            float blended_m[16] = {0};
            for (int w_i = 0; w_i < num_matrices; w_i++) {
                if (w_val[w_i] > 0.0001f) {
                    int matIdx = m_idx[w_i];
                    if (matIdx >= 32 && (matIdx % 3 == 0 || matIdx % 4 == 0)) matIdx /= (matIdx % 4 == 0 ? 4 : 3);
                    
                    const float* palM = nullptr;
                    if (matIdx >= 0 && matIdx < 32) palM = g_paletteStacks[matIdx].back().data();
                    else palM = g_paletteStacks[0].back().data(); // Безопасный фолбек
                    
                    for (int k = 0; k < 16; k++) blended_m[k] += palM[k] * w_val[w_i];
                }
            }
            
            blended_m[3] = 0.0f; blended_m[7] = 0.0f; blended_m[11] = 0.0f; blended_m[15] = 1.0f;
            
            const float* p = g_projectionStack.back().data();
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    v_mvp[c * 4 + r] = p[0 * 4 + r] * blended_m[c * 4 + 0] +
                                       p[1 * 4 + r] * blended_m[c * 4 + 1] +
                                       p[2 * 4 + r] * blended_m[c * 4 + 2] +
                                       p[3 * 4 + r] * blended_m[c * 4 + 3];
                }
            }
        } else {
            memcpy(v_mvp, mvp, 16 * sizeof(float));
        }

        float clipX = px * v_mvp[0] + py * v_mvp[4] + pz * v_mvp[8] + pw * v_mvp[12];
        float clipY = px * v_mvp[1] + py * v_mvp[5] + pz * v_mvp[9] + pw * v_mvp[13];
        float clipZ = px * v_mvp[2] + py * v_mvp[6] + pz * v_mvp[10] + pw * v_mvp[14];
        float clipW = px * v_mvp[3] + py * v_mvp[7] + pz * v_mvp[11] + pw * v_mvp[15];
        
        SWVertex outV;
        outV.x = clipX;
        outV.y = clipY;
        outV.z = clipZ;
        outV.w = clipW;
        outV.invW = 0.0f; // Вычислим после клиппинга
        
        if (prog > 0) {
            outV.r = cr * diffuseUniform[0] * 255.0f;
            outV.g = cg * diffuseUniform[1] * 255.0f;
            outV.b = cb * diffuseUniform[2] * 255.0f;
            outV.a = ca * diffuseUniform[3] * 255.0f;
        } else {
            outV.r = cr * 255.0f; outV.g = cg * 255.0f; outV.b = cb * 255.0f; outV.a = ca * 255.0f;
        }
        outV.u = tu; outV.v = tv;
        extractedVerts[validVertsCount++] = outV;
    }
    extractedVerts.resize(validVertsCount);
    
    // --- WIREEFRAME RADAR (Отрисовка границ геометрии) ---
    if (g_onScreenDebugOverlay && !extractedVerts.empty()) {
        float bMinX = extractedVerts[0].x, bMinY = extractedVerts[0].y;
        float bMaxX = extractedVerts[0].x, bMaxY = extractedVerts[0].y;
        for (const auto& v : extractedVerts) {
            if (v.x < bMinX) bMinX = v.x; if (v.x > bMaxX) bMaxX = v.x;
            if (v.y < bMinY) bMinY = v.y; if (v.y > bMaxY) bMaxY = v.y;
        }
        
        // Защита от бесконечного цикла, если координаты улетели в стратосферу
        int ix0 = (bMinX < -100) ? -100 : (bMinX > g_surfaceWidth + 100 ? g_surfaceWidth + 100 : (int)bMinX);
        int iy0 = (bMinY < -100) ? -100 : (bMinY > g_surfaceHeight + 100 ? g_surfaceHeight + 100 : (int)bMinY);
        int ix1 = (bMaxX < -100) ? -100 : (bMaxX > g_surfaceWidth + 100 ? g_surfaceWidth + 100 : (int)bMaxX);
        int iy1 = (bMaxY < -100) ? -100 : (bMaxY > g_surfaceHeight + 100 ? g_surfaceHeight + 100 : (int)bMaxY);

        // Рисуем пурпурную рамку вокруг обрабатываемой геометрии
        CPUDrawLine(ix0, iy0, ix1, iy0, 0xFFFF00FF);
        CPUDrawLine(ix0, iy1, ix1, iy1, 0xFFFF00FF);
        CPUDrawLine(ix0, iy0, ix0, iy1, 0xFFFF00FF);
        CPUDrawLine(ix1, iy0, ix1, iy1, 0xFFFF00FF);
    }
    // -----------------------------------------------------

    auto edgeFunction = [](const SWVertex& a, const SWVertex& b, const SWVertex& c) {
        return (double)(c.x - a.x) * (double)(b.y - a.y) - (double)(c.y - a.y) * (double)(b.x - a.x);
    };

    if (drawMode == GL_POINTS) {
        for (size_t i = 0; i < extractedVerts.size(); i++) {
            SWVertex v = extractedVerts[i];
            if (v.w <= 0.0001f) continue;
            float cx = v.x / v.w, cy = v.y / v.w, cz = v.z / v.w;
            if (g_gameViewportH > g_gameViewportW && targetW > targetH) { float t = cx; cx = -cy; cy = t; }
            v.x = (cx + 1.0f) * 0.5f * targetW;
            v.y = (1.0f - cy) * 0.5f * targetH;
            v.z = (cz + 1.0f) * 0.5f;

            int px = (int)v.x, py = (int)v.y;
            if (px >= 0 && px < targetW && py >= 0 && py < targetH) {
                int bufIdx = py * targetW + px;
                bool depthPass = true;
                if (g_depthTestEnabled || prog > 0) {
                    if (g_depthFunc == 0x0201) depthPass = (v.z < (*targetDepthBuf)[bufIdx]); // GL_LESS
                    else if (g_depthFunc == 0x0203) depthPass = (v.z <= (*targetDepthBuf)[bufIdx]); // GL_LEQUAL
                    else if (g_depthFunc == 0x0202) depthPass = (std::abs(v.z - (*targetDepthBuf)[bufIdx]) < 0.00001f); // GL_EQUAL
                    else if (g_depthFunc == 0x0200) depthPass = false; // GL_NEVER
                    else if (g_depthFunc == 0x0204) depthPass = (v.z > (*targetDepthBuf)[bufIdx]); // GL_GREATER
                    else if (g_depthFunc == 0x0206) depthPass = (v.z >= (*targetDepthBuf)[bufIdx]); // GL_GEQUAL
                    else if (g_depthFunc == 0x0205) depthPass = (std::abs(v.z - (*targetDepthBuf)[bufIdx]) >= 0.00001f); // GL_NOTEQUAL
                    else depthPass = true; // GL_ALWAYS
                }
                if (depthPass) {
                    if (g_depthMask || prog > 0) (*targetDepthBuf)[bufIdx] = v.z;
                    uint8_t r = (uint8_t)(v.r * 255.0f);
                    uint8_t g = (uint8_t)(v.g * 255.0f);
                    uint8_t b = (uint8_t)(v.b * 255.0f);
                    uint8_t a = (uint8_t)(v.a * 255.0f);
                    (*targetColorBuf)[bufIdx] = (a << 24) | (b << 16) | (g << 8) | r;
                }
            }
        }
        SyncLog("[RENDER] Отрисовано " + std::to_string(count) + " точек.");
        return;
    }

    size_t step = (drawMode == GL_TRIANGLES) ? 3 : 1;
    size_t endIdx = (drawMode == GL_TRIANGLES) ? extractedVerts.size() : (extractedVerts.size() < 3 ? 0 : extractedVerts.size() - 2);

    bool useTex = (g_texture2DEnabled || prog > 0) && tEnabled && g_cpuActiveTexture != 0;
    const uint32_t* activeTexPtr = nullptr;
    int activeTexW = 0, activeTexH = 0;
    bool isFboTexture = false;
    if (useTex) {
        auto it = g_cpuTextures.find(g_cpuActiveTexture);
        if (it != g_cpuTextures.end() && !it->second.empty()) {
            activeTexPtr = it->second.data();
            activeTexW = g_cpuTexW[g_cpuActiveTexture];
            activeTexH = g_cpuTexH[g_cpuActiveTexture];
            isFboTexture = g_fboTextures.count(g_cpuActiveTexture) && g_fboTextures[g_cpuActiveTexture];
        } else {
            useTex = false;
        }
    }

    if (g_logRenderDebug) {
        bool isSpamOn = (g_spamFiltersMask & (1 << 5)) != 0;
        static int draw_cnt = 0; draw_cnt++;
        if (!isSpamOn || draw_cnt <= 30 || draw_cnt % 120 == 0) {
            SyncLog("[DRAW-DEBUG] Prog: " + std::to_string(prog) + " drawMode: " + std::to_string(drawMode) + " count: " + std::to_string(count));
            SyncLog("[DRAW-DEBUG] Attributes -> PosLoc: " + std::to_string(posLoc) + " (En: " + std::to_string(posEnabled) + "), ColLoc: " + std::to_string(colorLoc) + " (En: " + std::to_string(cEnabled) + "), TexLoc: " + std::to_string(texLoc) + " (En: " + std::to_string(tEnabled) + ")");
            SyncLog("[DRAW-DEBUG] Texture Setup -> useTex: " + std::to_string(useTex) + " ActiveTexID: " + std::to_string(g_cpuActiveTexture) + " TexSize: " + std::to_string(activeTexW) + "x" + std::to_string(activeTexH) + " FBO: " + std::to_string(g_lastActiveFBO));
            
            if (tEnabled && texLoc != -1) {
                SyncLog("[DRAW-DEBUG] TexAttr -> VBO: " + std::to_string(g_vertexAttribs[texLoc].vbo) + " Stride: " + std::to_string(tStride) + " Type: " + std::to_string(tType));
            }

            if (!extractedVerts.empty()) {
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "[DRAW-DEBUG] Vert0 Raw: x=%g y=%g u=%g v=%g", (double)extractedVerts[0].x, (double)extractedVerts[0].y, (double)extractedVerts[0].u, (double)extractedVerts[0].v);
                SyncLog(dbg);
            }
        }
    }

    auto toScreen = [targetW, targetH](SWVertex& v) {
        float w = v.w; if (w < 0.0001f) w = 0.0001f;
        float cx = v.x / w, cy = v.y / w, cz = v.z / w;
        if (g_gameViewportH > g_gameViewportW && targetW > targetH) { float t = cx; cx = -cy; cy = t; }
        v.x = (cx + 1.0f) * 0.5f * targetW;
        v.y = (1.0f - cy) * 0.5f * targetH;
        v.z = (cz + 1.0f) * 0.5f;
        v.invW = 1.0f / w;
        v.u *= v.invW; v.v *= v.invW;
    };
    
    auto clipEdge = [](const SWVertex& a, const SWVertex& b, SWVertex& out) {
        float t = (0.01f - a.w) / (b.w - a.w);
        out.x = a.x + t * (b.x - a.x); out.y = a.y + t * (b.y - a.y);
        out.z = a.z + t * (b.z - a.z); out.w = 0.01f;
        out.u = a.u + t * (b.u - a.u); out.v = a.v + t * (b.v - a.v);
        out.r = a.r + t * (b.r - a.r); out.g = a.g + t * (b.g - a.g);
        out.b = a.b + t * (b.b - a.b); out.a = a.a + t * (b.a - a.a);
    };

    std::vector<SWVertex> clippedPolys;
    for (size_t i = 0; i < endIdx; i += step) {
        SWVertex inV[3] = { extractedVerts[drawMode == GL_TRIANGLE_FAN ? 0 : i], extractedVerts[i+1], extractedVerts[i+2] };
        if (inV[0].w <= 0.01f && inV[1].w <= 0.01f && inV[2].w <= 0.01f) continue;
        
        bool flipWinding = (drawMode == 0x0005 /*GL_TRIANGLE_STRIP*/ && (i % 2) != 0);
        SWVertex outV[4]; int numOut = 0;
        
        if (inV[0].w >= 0.01f && inV[1].w >= 0.01f && inV[2].w >= 0.01f) {
            outV[0] = inV[0]; outV[1] = inV[1]; outV[2] = inV[2]; numOut = 3;
        } else {
            // Алгоритм Сазерленда-Ходжмана для плоскости W=0.01
            for (int k = 0; k < 3; k++) {
                SWVertex curr = inV[k], prev = inV[(k + 2) % 3];
                bool currIn = curr.w >= 0.01f, prevIn = prev.w >= 0.01f;
                if (currIn != prevIn) clipEdge(prev, curr, outV[numOut++]);
                if (currIn) outV[numOut++] = curr;
            }
        }
        
        // Превращаем обрезанный полигон (3-4 вершины) обратно в треугольники
        for (int tri = 1; tri < numOut - 1; tri++) {
            SWVertex tv0 = outV[0], tv1 = outV[tri], tv2 = outV[tri + 1];
            toScreen(tv0); toScreen(tv1); toScreen(tv2);
            
            tv0.x = std::round(tv0.x * 16.0f) / 16.0f; tv0.y = std::round(tv0.y * 16.0f) / 16.0f;
            tv1.x = std::round(tv1.x * 16.0f) / 16.0f; tv1.y = std::round(tv1.y * 16.0f) / 16.0f;
            tv2.x = std::round(tv2.x * 16.0f) / 16.0f; tv2.y = std::round(tv2.y * 16.0f) / 16.0f;

            double area = edgeFunction(tv0, tv1, tv2);
            if (std::abs(area) < 0.001) continue;
            
            bool isCCW = area > 0;
            if (flipWinding) isCCW = !isCCW;
            bool isFront = (g_frontFace == 0x0901) ? isCCW : !isCCW;
            if (g_cullFaceEnabled) {
                if (g_cullFaceMode == 0x0405 && !isFront) continue;
                if (g_cullFaceMode == 0x0404 && isFront) continue;
                if (g_cullFaceMode == 0x0408) continue;
            }
            if (area < 0) { std::swap(tv1, tv2); }
            clippedPolys.push_back(tv0); clippedPolys.push_back(tv1); clippedPolys.push_back(tv2);
        }
    }

    for (size_t polyIdx = 0; polyIdx < clippedPolys.size(); polyIdx += 3) {
        SWVertex v0 = clippedPolys[polyIdx];
        SWVertex v1 = clippedPolys[polyIdx + 1];
        SWVertex v2 = clippedPolys[polyIdx + 2];
        double area = edgeFunction(v0, v1, v2);

        int minX = (int)(v0.x < v1.x ? (v0.x < v2.x ? v0.x : v2.x) : (v1.x < v2.x ? v1.x : v2.x)); if (minX < 0) minX = 0;
        int minY = (int)(v0.y < v1.y ? (v0.y < v2.y ? v0.y : v2.y) : (v1.y < v2.y ? v1.y : v2.y)); if (minY < 0) minY = 0;
        int maxX = (int)(v0.x > v1.x ? (v0.x > v2.x ? v0.x : v2.x) : (v1.x > v2.x ? v1.x : v2.x)) + 1; if (maxX >= targetW) maxX = targetW - 1;
        int maxY = (int)(v0.y > v1.y ? (v0.y > v2.y ? v0.y : v2.y) : (v1.y > v2.y ? v1.y : v2.y)) + 1; if (maxY >= targetH) maxY = targetH - 1;

        SWVertex p_start; p_start.x = minX + 0.5f; p_start.y = minY + 0.5f;
        double e0_row = edgeFunction(v1, v2, p_start);
        double e1_row = edgeFunction(v2, v0, p_start);
        double e2_row = edgeFunction(v0, v1, p_start);

        double stepX0 = v2.y - v1.y, stepX1 = v0.y - v2.y, stepX2 = v1.y - v0.y;
        double stepY0 = -(v2.x - v1.x), stepY1 = -(v0.x - v2.x), stepY2 = -(v1.x - v0.x);
        double invArea = 1.0 / area;

        // Вычисляем линейные шаги атрибутов по X и Y (Градиенты)
        double stepW0_x = stepX0 * invArea, stepW1_x = stepX1 * invArea, stepW2_x = stepX2 * invArea;
        double stepW0_y = stepY0 * invArea, stepW1_y = stepY1 * invArea, stepW2_y = stepY2 * invArea;

        double dz_dx = (double)v0.z * stepW0_x + (double)v1.z * stepW1_x + (double)v2.z * stepW2_x;
        double dz_dy = (double)v0.z * stepW0_y + (double)v1.z * stepW1_y + (double)v2.z * stepW2_y;
        float dinvW_dx = v0.invW * stepW0_x + v1.invW * stepW1_x + v2.invW * stepW2_x;
        float dinvW_dy = v0.invW * stepW0_y + v1.invW * stepW1_y + v2.invW * stepW2_y;
        
        float dr_dx = v0.r * stepW0_x + v1.r * stepW1_x + v2.r * stepW2_x;
        float dr_dy = v0.r * stepW0_y + v1.r * stepW1_y + v2.r * stepW2_y;
        float dg_dx = v0.g * stepW0_x + v1.g * stepW1_x + v2.g * stepW2_x;
        float dg_dy = v0.g * stepW0_y + v1.g * stepW1_y + v2.g * stepW2_y;
        float db_dx = v0.b * stepW0_x + v1.b * stepW1_x + v2.b * stepW2_x;
        float db_dy = v0.b * stepW0_y + v1.b * stepW1_y + v2.b * stepW2_y;
        float da_dx = v0.a * stepW0_x + v1.a * stepW1_x + v2.a * stepW2_x;
        float da_dy = v0.a * stepW0_y + v1.a * stepW1_y + v2.a * stepW2_y;
        
        float du_dx = v0.u * stepW0_x + v1.u * stepW1_x + v2.u * stepW2_x;
        float du_dy = v0.u * stepW0_y + v1.u * stepW1_y + v2.u * stepW2_y;
        float dv_dx = v0.v * stepW0_x + v1.v * stepW1_x + v2.v * stepW2_x;
        float dv_dy = v0.v * stepW0_y + v1.v * stepW1_y + v2.v * stepW2_y;

        // Стартовые значения атрибутов для начала цикла (угол minX, minY)
        double w0_start = e0_row * invArea, w1_start = e1_row * invArea, w2_start = e2_row * invArea;
        double z_row = (double)v0.z * w0_start + (double)v1.z * w1_start + (double)v2.z * w2_start;
        float invW_row = v0.invW * w0_start + v1.invW * w1_start + v2.invW * w2_start;
        float r_row = v0.r * w0_start + v1.r * w1_start + v2.r * w2_start;
        float g_row = v0.g * w0_start + v1.g * w1_start + v2.g * w2_start;
        float b_row = v0.b * w0_start + v1.b * w1_start + v2.b * w2_start;
        float a_row = v0.a * w0_start + v1.a * w1_start + v2.a * w2_start;
        float u_row = v0.u * w0_start + v1.u * w1_start + v2.u * w2_start;
        float v_row = v0.v * w0_start + v1.v * w1_start + v2.v * w2_start;

        // Переводим шаги градиентов цветов в Fixed-Point 16.16
        int dr_dx_fixed = (int)(dr_dx * 65536.0f);
        int dg_dx_fixed = (int)(dg_dx * 65536.0f);
        int db_dx_fixed = (int)(db_dx * 65536.0f);
        int da_dx_fixed = (int)(da_dx * 65536.0f);

        bool bias0 = (stepX0 > 0.0) || (stepX0 == 0.0 && stepY0 < 0.0);
        bool bias1 = (stepX1 > 0.0) || (stepX1 == 0.0 && stepY1 < 0.0);
        bool bias2 = (stepX2 > 0.0) || (stepX2 == 0.0 && stepY2 < 0.0);

        // --- HOISTING (Вынос логики и флагов из цикла) ---
        bool opt_tex = useTex;
        bool opt_isFboTex = isFboTexture;
        int texMaskW = activeTexW - 1;
        int texMaskH = activeTexH - 1;
        // Предрасчет: является ли текстура Power-Of-Two (256, 512 и т.д.)
        bool isPOT = (activeTexW > 0 && (activeTexW & texMaskW) == 0) && (activeTexH > 0 && (activeTexH & texMaskH) == 0);
        
        bool opt_blend = g_blendEnabled;
        bool opt_depth = g_depthTestEnabled;
        bool opt_dmask = g_depthMask;
        GLenum opt_depthFunc = g_depthFunc;
        bool opt_fullmask = (!g_blendEnabled && g_colorMask[0] && g_colorMask[1] && g_colorMask[2] && g_colorMask[3]);
        bool opt_prog = prog > 0;
        GLenum opt_alphaFunc = g_alphaFunc;
        int opt_iAlphaRef = (int)(g_alphaRef * 255.0f); // Вынесли умножение альфы из горячего цикла!
        float fTexW = (float)activeTexW;
        float fTexH = (float)activeTexH;
        
        int fastBlendMode = 0; 
        if (opt_blend) {
            if (g_blendSrc == GL_SRC_ALPHA && g_blendDst == GL_ONE_MINUS_SRC_ALPHA) fastBlendMode = 1; // Standard Alpha
            else if (g_blendSrc == GL_SRC_ALPHA && g_blendDst == GL_ONE) fastBlendMode = 2; // Additive
        }

        for (int y = minY; y <= maxY; y++, e0_row += stepY0, e1_row += stepY1, e2_row += stepY2, z_row += dz_dy, invW_row += dinvW_dy, r_row += dr_dy, g_row += dg_dy, b_row += db_dy, a_row += da_dy, u_row += du_dy, v_row += dv_dy) {
            double e0 = e0_row, e1 = e1_row, e2 = e2_row;
            double z = z_row; float invW = invW_row, u = u_row, v = v_row;
            
            // Задаем стартовые значения Fixed-Point цветов для текущей строки
            int r_fixed = (int)(r_row * 65536.0f);
            int g_fixed = (int)(g_row * 65536.0f);
            int b_fixed = (int)(b_row * 65536.0f);
            int a_fixed = (int)(a_row * 65536.0f);
            
            float* depthPtr = &(*targetDepthBuf)[y * targetW + minX];
            uint32_t* colorPtr = &(*targetColorBuf)[y * targetW + minX];
            
            bool inside = false;
            for (int x = minX; x <= maxX; x++, e0 += stepX0, e1 += stepX1, e2 += stepX2, z += dz_dx, invW += dinvW_dx, r_fixed += dr_dx_fixed, g_fixed += dg_dx_fixed, b_fixed += db_dx_fixed, a_fixed += da_dx_fixed, u += du_dx, v += dv_dx, depthPtr++, colorPtr++) {
                if ((e0 > 0.0 || (e0 == 0.0 && bias0)) && 
                    (e1 > 0.0 || (e1 == 0.0 && bias1)) && 
                    (e2 > 0.0 || (e2 == 0.0 && bias2))) {
                    inside = true;
                    if (opt_depth) {
                        if (opt_depthFunc == 0x0201) { if (z >= *depthPtr) continue; } // GL_LESS
                        else if (opt_depthFunc == 0x0203) { if (z > *depthPtr) continue; } // GL_LEQUAL
                        else if (opt_depthFunc == 0x0202) { if (std::abs(z - *depthPtr) >= 0.00001) continue; } // GL_EQUAL
                        else if (opt_depthFunc == 0x0200) continue; // GL_NEVER
                        else if (opt_depthFunc == 0x0204) { if (z <= *depthPtr) continue; } // GL_GREATER
                        else if (opt_depthFunc == 0x0206) { if (z < *depthPtr) continue; } // GL_GEQUAL
                        else if (opt_depthFunc == 0x0205) { if (std::abs(z - *depthPtr) < 0.00001) continue; } // GL_NOTEQUAL
                    }
                    if (invW <= 0.00001f) continue;
                    if (opt_dmask) *depthPtr = (float)z;
                    
                    // Аффинные цвета теперь интерполируются через целочисленную арифметику 16.16
                    int ir = r_fixed >> 16;
                    int ig = g_fixed >> 16;
                    int ib = b_fixed >> 16;
                    int ia = a_fixed >> 16;

                    if (opt_tex) {
                        // ИДЕАЛЬНАЯ ПЕРСПЕКТИВА: Newton-Raphson давал погрешности (искажения на полу вдали).
                        // Используем точное деление. На современных ARM это работает достаточно быстро.
                        float realW = 1.0f / invW;
                        
                        int tx, ty;
                        if (isPOT) {
                            // Исправлен wrapping отрицательных UV координат. Без std::floor каст отрицательного
                            // float обрезается к нулю, создавая мертвую зону (от -0.99 до 0.99 = 0), ломающую тайлинг.
                            tx = (int)(std::floor(u * realW * fTexW)) & texMaskW;
                            ty = (int)(std::floor(v * realW * fTexH)) & texMaskH;
                        } else {
                            // Фолбек для non-POT текстур
                            float realU = (u * realW);
                            float realV = (v * realW);
                            realU = realU - std::floor(realU);
                            realV = realV - std::floor(realV);
                            tx = (int)(realU * fTexW);
                            ty = (int)(realV * fTexH);
                        }
                        
                        if (opt_isFboTex) ty = activeTexH - 1 - ty;
                        if (ty < 0) ty = 0; else if (ty >= activeTexH) ty = activeTexH - 1; // Защита
                        
                        uint32_t texColor = activeTexPtr[ty * activeTexW + tx];
                        ir = (ir * (texColor & 0xFF)) >> 8;
                        ig = (ig * ((texColor >> 8) & 0xFF)) >> 8;
                        ib = (ib * ((texColor >> 16) & 0xFF)) >> 8;
                        ia = (ia * ((texColor >> 24) & 0xFF)) >> 8;
                    }

                    if (opt_prog) {
                        if (ia <= 0) continue;
                    } else if (opt_alphaFunc != GL_ALWAYS) {
                        if (opt_alphaFunc == GL_GREATER) { if (ia <= opt_iAlphaRef) continue; }
                        else if (opt_alphaFunc == GL_GEQUAL) { if (ia < opt_iAlphaRef) continue; }
                        else if (opt_alphaFunc == GL_LESS) { if (ia >= opt_iAlphaRef) continue; }
                        else if (opt_alphaFunc == GL_LEQUAL) { if (ia > opt_iAlphaRef) continue; }
                        else if (opt_alphaFunc == GL_EQUAL) { if (ia != opt_iAlphaRef) continue; }
                        else if (opt_alphaFunc == GL_NOTEQUAL) { if (ia == opt_iAlphaRef) continue; }
                        else if (opt_alphaFunc == GL_NEVER) continue;
                    }

                    if (ir > 255) ir = 255; else if (ir < 0) ir = 0;
                    if (ig > 255) ig = 255; else if (ig < 0) ig = 0;
                    if (ib > 255) ib = 255; else if (ib < 0) ib = 0;
                    if (ia > 255) ia = 255; else if (ia < 0) ia = 0;

                    // Отсекаем полностью прозрачные пиксели (экономит пропускную способность памяти на чтение/запись)
                    if (ia == 0 && fastBlendMode > 0) continue;

                    if (opt_fullmask) {
                        *colorPtr = (ia << 24) | (ib << 16) | (ig << 8) | ir;
                        continue;
                    }

                    uint32_t bg = *colorPtr;
                    int br = bg & 0xFF;
                    int bg_g = (bg >> 8) & 0xFF;
                    int bb = (bg >> 16) & 0xFF;
                    
                    if (fastBlendMode == 1) { // Standard Alpha (90% случаев)
                        int invA = 255 - ia;
                        ir = (ir * ia + br * invA) >> 8;
                        ig = (ig * ia + bg_g * invA) >> 8;
                        ib = (ib * ia + bb * invA) >> 8;
                    } else if (fastBlendMode == 2) { // Additive Glow
                        ir = (ir * ia + br * 255) >> 8;
                        ig = (ig * ia + bg_g * 255) >> 8;
                        ib = (ib * ia + bb * 255) >> 8;
                        if (ir > 255) ir = 255;
                        if (ig > 255) ig = 255;
                        if (ib > 255) ib = 255;
                    } else if (opt_blend) { // Fallback для специфичных блендов
                        int srcFactor = 255, dstFactor = 0;
                        if (g_blendSrc == GL_SRC_ALPHA) srcFactor = ia;
                        else if (g_blendSrc == GL_ONE_MINUS_SRC_ALPHA) srcFactor = 255 - ia;
                        else if (g_blendSrc == GL_ZERO) srcFactor = 0;
                        else if (g_blendSrc == GL_DST_COLOR) srcFactor = br;
                        
                        if (g_blendDst == GL_ONE_MINUS_SRC_ALPHA) dstFactor = 255 - ia;
                        else if (g_blendDst == GL_ONE) dstFactor = 255;
                        else if (g_blendDst == GL_SRC_ALPHA) dstFactor = ia;
                        
                        ir = (ir * srcFactor + br * dstFactor) >> 8;
                        ig = (ig * srcFactor + bg_g * dstFactor) >> 8;
                        ib = (ib * srcFactor + bb * dstFactor) >> 8;
                        
                        if (ir > 255) ir = 255;
                        if (ig > 255) ig = 255;
                        if (ib > 255) ib = 255;
                    }

                    uint8_t finalR = g_colorMask[0] ? ir : br;
                    uint8_t finalG = g_colorMask[1] ? ig : bg_g;
                    uint8_t finalB = g_colorMask[2] ? ib : bb;
                    uint8_t finalA = g_colorMask[3] ? ia : ((bg >> 24) & 0xFF);
                    
                    *colorPtr = (finalA << 24) | (finalB << 16) | (finalG << 8) | finalR;
                } else if (inside) {
                    break; // ВАЖНО: Треугольник выпуклый. Раз мы вышли за его границу, дальше по оси X пусто.
                }
            }
        }
    }

    SyncLog("[RENDER] Отрисовано " + std::to_string(count/3) + " полигонов (Rasterized).");
}


extern "C" void MegaDebug_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    SyncLog("[GL-TRACE] glDrawArrays(mode=" + std::to_string(mode) + ", first=" + std::to_string(first) + ", count=" + std::to_string(count) + ")");
    g_frameHasDraw = true;
    if (g_gpuOffloadMask & 16) {
        int targetW = g_surfaceWidth;
        int targetH = g_surfaceHeight;
        if (g_lastActiveFBO != 0 && g_fboColorTex.count(g_lastActiveFBO)) {
            GLuint tex = g_fboColorTex[g_lastActiveFBO];
            if (tex != 0 && g_cpuTexW.count(tex) && g_cpuTexW[tex] > 0) {
                targetW = g_cpuTexW[tex];
                targetH = g_cpuTexH[tex];
            }
        }
        bool needRot = (g_gameViewportH > g_gameViewportW && targetW > targetH);
        GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        if (prog > 0) {
            GLint rotLoc = glGetUniformLocation(prog, "u_damn_rot");
            if (rotLoc != -1) glUniform1f(rotLoc, needRot ? 1.0f : 0.0f);
        }
        glDrawArrays(mode, first, count);
    } else {
        CPUExtractAndDraw(mode, first, count, nullptr, 0);
    }
}
extern "C" void MegaDebug_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    SyncLog("[GL-TRACE] glDrawElements(mode=" + std::to_string(mode) + ", count=" + std::to_string(count) + ", type=" + std::to_string(type) + ")");
    g_frameHasDraw = true;
    if (g_gpuOffloadMask & 16) {
        int targetW = g_surfaceWidth;
        int targetH = g_surfaceHeight;
        if (g_lastActiveFBO != 0 && g_fboColorTex.count(g_lastActiveFBO)) {
            GLuint tex = g_fboColorTex[g_lastActiveFBO];
            if (tex != 0 && g_cpuTexW.count(tex) && g_cpuTexW[tex] > 0) {
                targetW = g_cpuTexW[tex];
                targetH = g_cpuTexH[tex];
            }
        }
        bool needRot = (g_gameViewportH > g_gameViewportW && targetW > targetH);
        GLint prog = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        if (prog > 0) {
            GLint rotLoc = glGetUniformLocation(prog, "u_damn_rot");
            if (rotLoc != -1) glUniform1f(rotLoc, needRot ? 1.0f : 0.0f);
        }
        glDrawElements(mode, count, type, indices);
    } else {
        CPUExtractAndDraw(mode, 0, count, indices, type);
    }
}
extern "C" void MegaDebug_glUseProgram(GLuint program) {
    SyncLog("[GL-TRACE] glUseProgram(prog=" + std::to_string(program) + ")");
    glUseProgram(program);
}
extern "C" void MegaDebug_glEnable(GLenum cap) {
    if (cap == GL_BLEND) g_blendEnabled = true;
    else if (cap == GL_DEPTH_TEST) g_depthTestEnabled = true;
    else if (cap == GL_TEXTURE_2D) g_texture2DEnabled = true;
    else if (cap == 0x0B44) g_cullFaceEnabled = true; // GL_CULL_FACE
    else if (cap == 0x8840) g_matrixPaletteEnabled = true; // GL_MATRIX_PALETTE_OES
    if (g_gpuOffloadMask & 32) glEnable(cap);
}
extern "C" void MegaDebug_glDisable(GLenum cap) {
    if (cap == GL_BLEND) g_blendEnabled = false;
    else if (cap == GL_DEPTH_TEST) g_depthTestEnabled = false;
    else if (cap == GL_TEXTURE_2D) g_texture2DEnabled = false;
    else if (cap == 0x0B44) g_cullFaceEnabled = false;
    else if (cap == 0x8840) g_matrixPaletteEnabled = false;
    if (g_gpuOffloadMask & 32) glDisable(cap);
}
extern "C" void MegaDebug_glCullFace(GLenum mode) { g_cullFaceMode = mode; if (g_gpuOffloadMask & 32) glCullFace(mode); }
extern "C" void MegaDebug_glFrontFace(GLenum mode) { g_frontFace = mode; if (g_gpuOffloadMask & 32) glFrontFace(mode); }
extern "C" void MegaDebug_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    g_blendSrc = sfactor; g_blendDst = dfactor;
    if (g_gpuOffloadMask & 32) glBlendFunc(sfactor, dfactor);
}
extern "C" void MegaDebug_glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    g_blendSrc = srcRGB; g_blendDst = dstRGB;
    if (g_gpuOffloadMask & 32) glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
}
extern "C" void MegaDebug_glDepthMask(GLboolean flag) {
    g_depthMask = (flag != GL_FALSE);
    if (g_gpuOffloadMask & 32) glDepthMask(flag);
}
extern "C" void MegaDebug_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    g_colorMask[0] = (r != GL_FALSE); g_colorMask[1] = (g != GL_FALSE);
    g_colorMask[2] = (b != GL_FALSE); g_colorMask[3] = (a != GL_FALSE);
    if (g_gpuOffloadMask & 32) glColorMask(r, g, b, a);
}
extern "C" void MegaDebug_glDepthFunc(GLenum func) {
    g_depthFunc = func;
    if (g_gpuOffloadMask & 32) glDepthFunc(func);
}
extern "C" const GLubyte* MegaDebug_glGetString(GLenum name) {
    if (name == GL_VERSION) {
        const char* customVer = (g_activeESVersion == 1) ? "OpenGL ES-CM 1.1 Apple" : "OpenGL ES 2.0 Apple";
        LogToJava(std::string("[GL-TRACE] glGetString(GL_VERSION) intercepted -> ") + customVer);
        return (const GLubyte*)customVer;
    }
    if (name == GL_RENDERER) {
        const char* customRen = "PowerVR SGX 535";
        LogToJava(std::string("[GL-TRACE] glGetString(GL_RENDERER) intercepted -> ") + customRen);
        return (const GLubyte*)customRen;
    }
    if (name == GL_EXTENSIONS) {
        const char* customExt = "GL_OES_matrix_palette GL_OES_draw_texture GL_EXT_texture_filter_anisotropic GL_APPLE_texture_2D_limited_npot GL_OES_framebuffer_object GL_OES_element_index_uint";
        LogToJava("[GL-TRACE] glGetString(GL_EXTENSIONS) intercepted -> Added ES 1.1 Skinning Extensions (VAO disabled)");
        return (const GLubyte*)customExt;
    }
    const GLubyte* res = glGetString(name);
    LogToJava(std::string("[GL-TRACE] glGetString(") + std::to_string(name) + ") -> " + (res ? (const char*)res : "NULL"));
    return res;
}
extern "C" GLenum MegaDebug_glGetError() {
    GLenum err = glGetError();
    if (err != 0) SyncLog("[GL-TRACE] glGetError() -> 0x" + std::to_string(err));
    return err;
}
extern "C" void wrap_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (value && count > 0) g_uniformShadowFloat[location].assign(value, value + count * 16);
    glUniformMatrix4fv(location, count, transpose, value);
}

extern "C" void wrap_glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
    if (value && count > 0) g_uniformShadowFloat[location].assign(value, value + count * 4);
    glUniform4fv(location, count, value);
}

extern "C" void wrap_glUniform1i(GLint location, GLint v0) {
    LogToJava("[GL-UNIFORM] glUniform1i loc=" + std::to_string(location) + " val=" + std::to_string(v0));
    glUniform1i(location, v0);
}

extern "C" void wrap_glGetFloatv(GLenum pname, GLfloat *data) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    if (pname == 0x0BA2) { // GL_VIEWPORT
        glGetFloatv(pname, data);
        LogToJava(">>>>>>>> [SIZE-CRITICAL] glGetFloatv(GL_VIEWPORT) <<<<<<<< Caller: " + GetModuleInfoForAddress(lr));
        return;
    }
    glGetFloatv(pname, data);
}

extern "C" void wrap_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava(">>>>>>>> [SIZE-CRITICAL] glScissor(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(width) + ", " + std::to_string(height) + ") <<<<<<<< Caller: " + GetModuleInfoForAddress(lr));
    glScissor(x, y, width, height);
}

extern "C" void MegaDebug_glGetIntegerv(GLenum pname, GLint *data) {
    if (!data) return;
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    if (pname == 0x0BA2) { // GL_VIEWPORT
        glGetIntegerv(pname, data);
        LogToJava(">>>>>>>> [SIZE-CRITICAL] glGetIntegerv(GL_VIEWPORT) <<<<<<<< Caller: " + GetModuleInfoForAddress(lr) + " | Отдаем: " + std::to_string(data[0]) + ", " + std::to_string(data[1]) + ", " + std::to_string(data[2]) + ", " + std::to_string(data[3]));
        return;
    }
    if (pname == 0x0D33) { // GL_MAX_TEXTURE_SIZE
        glGetIntegerv(pname, data);
        LogToJava(">>>>>>>> [SIZE-CRITICAL] glGetIntegerv(GL_MAX_TEXTURE_SIZE) <<<<<<<< Caller: " + GetModuleInfoForAddress(lr) + " | Отдаем: " + std::to_string(data[0]));
        return;
    }
    if (pname == 0x8D42 || pname == 0x8D43) { // GL_RENDERBUFFER_WIDTH / HEIGHT
        LogToJava(">>>>>>>> [SIZE-CRITICAL] ВНИМАНИЕ: glGetIntegerv запросил WIDTH/HEIGHT (вместо glGetRenderbufferParameteriv) <<<<<<<< Caller: " + GetModuleInfoForAddress(lr));
    }
    if (pname == 0x8894) { *data = g_boundBuffers[0x8892]; return; } // GL_ARRAY_BUFFER_BINDING
    if (pname == 0x8895) { *data = g_boundBuffers[0x8893]; return; } // GL_ELEMENT_ARRAY_BUFFER_BINDING
    if (pname == 0x8842) { *data = 32; return; } // GL_MAX_PALETTE_MATRICES_OES
    if (pname == 0x8843) { *data = g_currentPaletteMatrix; return; } // GL_CURRENT_PALETTE_MATRIX_OES
    if (pname == 0x0BA0) { *data = g_matrixMode; return; } // GL_MATRIX_MODE
    if (pname == 0x8B47) { *data = 4; return; }  // GL_MAX_VERTEX_UNITS_OES
    if (pname == 0x84E2) { *data = 8; return; }  // GL_MAX_TEXTURE_UNITS
    if (pname == 0x0D31) { *data = 8; return; }  // GL_MAX_LIGHTS
    if (pname == 0x0D32) { *data = 6; return; }  // GL_MAX_CLIP_PLANES
    if (pname == 0x0D36) { *data = 32; return; } // GL_MAX_MODELVIEW_STACK_DEPTH
    if (pname == 0x0D38) { *data = 2; return; }  // GL_MAX_PROJECTION_STACK_DEPTH
    if (pname == 0x0D39) { *data = 2; return; }  // GL_MAX_TEXTURE_STACK_DEPTH
    glGetIntegerv(pname, data);
}
extern "C" GLint MegaDebug_glGetUniformLocation(GLuint program, const GLchar *name) {
    GLint loc = glGetUniformLocation(program, name);
    SyncLog("[GL-TRACE] glGetUniformLocation(prog=" + std::to_string(program) + ", name=" + (name ? name : "null") + ") -> " + std::to_string(loc));
    return loc;
}
extern "C" GLint MegaDebug_glGetAttribLocation(GLuint program, const GLchar *name) {
    GLint loc = glGetAttribLocation(program, name);
    SyncLog("[GL-TRACE] glGetAttribLocation(prog=" + std::to_string(program) + ", name=" + (name ? name : "null") + ") -> " + std::to_string(loc));
    return loc;
}
extern "C" void MegaDebug_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    SyncLog("[GL-TRACE] glGetShaderInfoLog intercepted. Game requested bufSize=" + std::to_string(bufSize));
    if (length) *length = 0;
    if (infoLog && bufSize > 0) infoLog[0] = '\0';
}
extern "C" void MegaDebug_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    SyncLog("[GL-TRACE] glGetProgramInfoLog intercepted. Game requested bufSize=" + std::to_string(bufSize));
    if (length) *length = 0;
    if (infoLog && bufSize > 0) infoLog[0] = '\0';
}

extern "C" void Stub_glRenderbufferStorageMultisampleAPPLE(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) { Stub_glRenderbufferStorage(target, internalformat, width, height); }


// ==========================================
// STB_TRUETYPE TEXT GENERATION & CACHING
// ==========================================
void UpdateTextCache(void* view, const std::string& text, float logicalH) {
    if (!g_fontLoaded) {
        LogToJava("HLE_ERROR: ОШИБКА РЕНДЕРА ТЕКСТА! Шрифт не загружен. Убедитесь, что файл 'Roboto-VariableFont_wdth,wght.ttf' лежит в папке setup/ внутри рабочей директории!");
        return;
    }
    if (text.empty()) return;
    
    // Переводим логическую высоту (320p) в физические пиксели устройства для четкости
    float pixelHeight = (logicalH / 320.0f) * g_surfaceHeight;
    if (pixelHeight < 10) pixelHeight = 10; 

    auto& cache = g_uiTextCache[view];
    // Если текст и физический размер не изменились — используем старый кэш
    if (cache.text == text && std::abs(cache.pixelHeight - pixelHeight) < 1.0f) return;

    float scale = stbtt_ScaleForPixelHeight(&g_fontInfo, pixelHeight);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&g_fontInfo, &ascent, &descent, &lineGap);
    ascent *= scale; descent *= scale;
    
    std::vector<int> codepoints;
    for (size_t i = 0; i < text.length(); ) {
        unsigned char c = text[i];
        if (c < 0x80) { codepoints.push_back(c); i++; }
        else if ((c & 0xE0) == 0xC0) { if (i + 1 < text.length()) { codepoints.push_back(((c & 0x1F) << 6) | (text[i+1] & 0x3F)); i += 2; } else break; }
        else if ((c & 0xF0) == 0xE0) { if (i + 2 < text.length()) { codepoints.push_back(((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F)); i += 3; } else break; }
        else if ((c & 0xF8) == 0xF0) { if (i + 3 < text.length()) { codepoints.push_back(((c & 0x07) << 18) | ((text[i+1] & 0x3F) << 12) | ((text[i+2] & 0x3F) << 6) | (text[i+3] & 0x3F)); i += 4; } else break; }
        else { i++; }
    }
    
    // Проход 1: Узнаем точную общую ширину строки в пикселях с учетом Bounding Box
    float xpos_measure = 0;
    float max_x = 0;
    int total_lines = 1;
    for (int c : codepoints) {
        if (c == '\n') {
            if (xpos_measure > max_x) max_x = xpos_measure;
            xpos_measure = 0;
            total_lines++;
            continue;
        }
        int advance, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_fontInfo, c, &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&g_fontInfo, c, scale, scale, &x0, &y0, &x1, &y1);
        if (xpos_measure + x1 > max_x) max_x = xpos_measure + x1;
        xpos_measure += advance * scale;
    }
    if (xpos_measure > max_x) max_x = xpos_measure;
    
    int lineHeight = ascent - descent + lineGap;
    int texW = (int)std::ceil(max_x) + 4; // +4 для надежного отступа
    int texH = lineHeight * total_lines + std::abs(descent) + 10; // Запас для букв с нижними хвостами (y, g, p, q)
    if (texW <= 0 || texH <= 0) return;
    
    // Выделяем 1-байтную память (Alpha-канал)
    std::vector<unsigned char> bitmap(texW * texH, 0);
    
    // Проход 2: Рендерим буквы в буфер
    float xpos = 0;
    float ypos = 0;
    for (int c : codepoints) {
        if (c == '\n') {
            xpos = 0;
            ypos += lineHeight;
            continue;
        }
        int advance, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_fontInfo, c, &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&g_fontInfo, c, scale, scale, &x0, &y0, &x1, &y1);
        
        int charW = x1 - x0; int charH = y1 - y0;
        int startX = (int)xpos + x0; int startY = (int)ypos + ascent + y0;
        
        // Рисуем букву с безопасной обрезкой, если она вылезает за границы
        if (startX >= 0 && startY >= 0 && startX + charW <= texW && startY + charH <= texH) {
            stbtt_MakeCodepointBitmap(&g_fontInfo, bitmap.data() + startX + startY * texW, charW, charH, texW, scale, scale, c);
        } else if (startX >= 0 && startY >= 0) {
            int safeW = (startX + charW > texW) ? (texW - startX) : charW;
            int safeH = (startY + charH > texH) ? (texH - startY) : charH;
            if (safeW > 0 && safeH > 0) {
                stbtt_MakeCodepointBitmap(&g_fontInfo, bitmap.data() + startX + startY * texW, safeW, safeH, texW, scale, scale, c);
            }
        }
        xpos += advance * scale;
    }
    
    cache.width = texW; cache.height = texH;
    cache.text = text; cache.pixelHeight = pixelHeight;
    cache.bitmap = bitmap;
}

// ==========================================
// CUSTOM HLE ES 2.0 UI RENDERER
// ==========================================

void DrawViewRecursive(void* view, float parentX, float parentY) {
    if (!view || !g_views.count(view)) return;
    auto& v = g_views[view];
    if (v.hidden) return;

    float x = parentX + v.frame[0], y = parentY + v.frame[1];
    float w = v.frame[2], h = v.frame[3];
    std::string type = v.type;

    if (type == "UIButton") {
        float btnAlpha = v.alpha;
        if (btnAlpha > 0.0f) {
            bool pressed = v.buttonPressed;
            // Черная обводка для всех системных кнопок
            CPUDrawSolidRect(x, y, w, h, 0.0f, 0.0f, 0.0f, btnAlpha, 8.0f);
            
            // Внутренний фон (чуть темнеет при нажатии, иначе 100% белый)
            if (pressed) { 
                CPUDrawSolidRect(x + 1.5f, y + 1.5f, w - 3.0f, h - 3.0f, 0.7f, 0.7f, 0.7f, btnAlpha, 6.5f);
            } else {
                CPUDrawSolidRect(x + 1.5f, y + 1.5f, w - 3.0f, h - 3.0f, 1.0f, 1.0f, 1.0f, btnAlpha, 6.5f);
            }
        }
    } else if (type == "UISwitch") {
        bool isOn = v.switchState;
        CPUDrawSolidRect(x, y, w, h, 0.2f, 0.8f, 0.2f, 1.0f);
        float sqW = w / 2.0f; float sqX = isOn ? (x + w - sqW) : x;
        CPUDrawSolidRect(sqX, y, sqW, h, 1.0f, 1.0f, 1.0f, 1.0f);
    } else if (type == "UIActivityIndicatorView") {
        CPUDrawSolidRect(x + w/2.0f - 10, y + h/2.0f - 10, 20, 20, 1.0f, 0.5f, 0.0f, 1.0f, 5.0f);
    } else if (type == "UIImageView") {
        void* uiImage = g_dictionaries[view][(void*)0x1111];
        if (uiImage) {
            uint32_t cgImg = ((uint32_t*)uiImage)[1];
            CPUDrawImage(x, y, w, h, (void*)cgImg, v.alpha);
        }
    } else if (v.hasBg) {
        HLEColor c = v.bgColor;
        float finalAlpha = c.a * v.alpha;
        if (finalAlpha > 0.0f) {
            CPUDrawSolidRect(x, y, w, h, c.r, c.g, c.b, finalAlpha, v.cornerRadius);
        }
    }

    std::string text = v.text;
    if (!text.empty()) {
        float textLogicalH = 14.0f;
        if (type == "UIButton") textLogicalH = h * 0.4f;
        else if (type == "UINavigationBar") textLogicalH = h * 0.4f;
        else if (type == "UITextView") textLogicalH = 12.0f;
        else if (type == "UILabel") {
            if (h < 40.0f) textLogicalH = h * 0.6f;
            else textLogicalH = 14.0f; 
        }
        if (textLogicalH > 32.0f) textLogicalH = 32.0f;
        if (textLogicalH < 8.0f) textLogicalH = 8.0f;

        UpdateTextCache(view, text, textLogicalH);
        auto& cache = g_uiTextCache[view];
        
        if (!cache.bitmap.empty()) {
            float drawW = cache.width * (textLogicalH / cache.pixelHeight);
            float drawH = cache.height * (textLogicalH / cache.pixelHeight);
            
            if (type != "UITextView" && w > 10.0f && drawW > w - 4.0f && h < 40.0f) {
                float scaleDown = (w - 4.0f) / drawW;
                drawW *= scaleDown;
                drawH *= scaleDown;
            }
            
            float drawX = x + (w > drawW ? (w - drawW) / 2.0f : 0.0f); // По центру по умолчанию
            float drawY = y + (h > drawH ? (h - drawH) / 2.0f : 0.0f);
            
            if (v.textAlignment != -1) {
                if (v.textAlignment == 0) drawX = x + 5.0f; // Лево
                else if (v.textAlignment == 2) drawX = x + w - drawW - 5.0f; // Право
            } else if (type == "UITextView") {
                drawX = x + 5.0f;
                drawY = y + 5.0f;
            }
            
            float tr = 0, tg = 0, tb = 0;
            if (v.hasTextCol) {
                tr = v.textColor.r; tg = v.textColor.g; tb = v.textColor.b;
            }
            CPUDrawText(drawX, drawY, drawW, drawH, tr, tg, tb, cache);
        }
    }

    if (type == "UITableViewCell") {
        CPUDrawLine((int)x, (int)(y + h - 1), (int)(x + w), (int)(y + h - 1), 0xFF888888);
    }

    for (void* child : v.children) {
        DrawViewRecursive(child, x, y);
    }
}

void RenderHLEUI() {
    void* activeView = g_presentedView ? g_presentedView : g_mainView;
    if (!activeView) return;
    DrawViewRecursive(activeView, 0.0f, 0.0f);
}



// ==========================================
// HLE OBJECTIVE-C RUNTIME
// ==========================================
std::string GetObjCClassName(void* obj) {
    if (!obj || (uintptr_t)obj < 0x1000) return "nil";
    uint32_t isa = 0;
    if (!SafeRead32((uintptr_t)obj, &isa)) return "InvalidObj";
    if (isa == 0xDEADBEEF) return ((HLEClass*)obj)->className;
    if (isa > 0x1000) {
        uint32_t cls0 = 0;
        if (!SafeRead32(isa, &cls0)) return "InvalidIsa";
        if (cls0 == 0xDEADBEEF) return std::string(((HLEClass*)isa)->className) + "(instance)";
        uint32_t cls4 = 0;
        if (SafeRead32(isa + 16, &cls4)) {
            uint32_t data_ptr = cls4 & ~3;
            if (data_ptr > 0x1000) {
                uint32_t name_ptr = 0;
                if (SafeRead32(data_ptr + 16, &name_ptr) && name_ptr > 0x1000) {
                    if (isValidString((const char*)name_ptr)) return std::string((const char*)name_ptr);
                }
            }
        }
    }
    std::stringstream ss; ss << "UnknownClass_0x" << std::hex << isa; return ss.str();
}

void* FindMethodIMP(uint32_t class_ptr, const char* selName) {
    if (!class_ptr || class_ptr < 0x1000) return nullptr;
    uint32_t* cls = (uint32_t*)class_ptr; 
    // Если мы дошли до HLE-класса (заглушки), значит в нативном коде реализации нет
    if (cls[0] == 0xDEADBEEF) return nullptr;
    uint32_t data_ptr = cls[4] & ~3; if (!data_ptr || data_ptr < 0x1000) return nullptr;
    uint32_t* ro = (uint32_t*)data_ptr; uint32_t methodList_ptr = ro[5]; 
    if (methodList_ptr && methodList_ptr > 0x1000) {
        uint32_t* mlist = (uint32_t*)methodList_ptr; uint32_t count = mlist[1];
        if (count < 10000) { 
            uint32_t* methods = mlist + 2; 
            for (uint32_t i = 0; i < count; i++) {
                uint32_t m_name_ptr = methods[i*3 + 0]; uint32_t m_imp = methods[i*3 + 2];
                if (isValidString((const char*)m_name_ptr)) { if (strcmp((const char*)m_name_ptr, selName) == 0) return (void*)m_imp; }
            }
        }
    }
    uint32_t super_class = cls[1]; if (super_class && super_class != class_ptr) return FindMethodIMP(super_class, selName);
    return nullptr;
}

void* GetNSValuePtr(void* nsvalue) { return (void*)((uint32_t*)nsvalue)[1]; }

// --- HELPER: Поиск ближайшего системного класса (HLE) в дереве наследования ---
std::string GetBaseSystemClassName(uint32_t class_ptr) {
    if (!class_ptr || class_ptr < 0x1000) return "Unknown";
    
    int depth = 0; // Защита от бесконечного цикла, если дерево сломано
    uint32_t current = class_ptr;
    
    while (current > 0x1000 && depth++ < 20) {
        uint32_t* cls = (uint32_t*)current;
        // Если наткнулись на HLE-класс (наш системный маркер)
        if (cls[0] == 0xDEADBEEF) {
            return ((HLEClass*)current)->className;
        }
        // Шагаем к родителю
        uint32_t super_class = cls[1];
        if (!super_class || super_class == current) break;
        current = super_class;
    }
    return "NSObject"; // Фолбек, если дошли до конца и ничего не поняли
}
// ------------------------------------------------------------------------------

extern "C" void* wrap_Block_copy(const void* aBlock);
extern "C" uint64_t Stub_mach_absolute_time();

extern "C" uint64_t Stub_objc_msgSend(void* self, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8);
extern "C" uint64_t Stub_objc_msgSendSuper2(void* super_struct, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8);
extern "C" void* Stub_objc_msgSend_stret(void* ret_addr, void* self, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7);
extern "C" void* Stub_objc_msgSendSuper2_stret(void* ret_addr, void* super_struct, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7);

float g_fpu_args[4] = {0};
float g_fpu_ret[4] = {0};
int g_fpu_ret_flag = 0;

uint64_t Impl_objc_msgSend(void* self, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    float saved_s[4] = {g_fpu_args[0], g_fpu_args[1], g_fpu_args[2], g_fpu_args[3]};
    static void* s_lazyGlView = nullptr;
    static void* s_lastMoviePlayer = nullptr;
    if (!self) {
        static int nil_count = 0;
        if (nil_count++ < 30 && op) LogToJava("OBJC-CALL: [nil " + std::string(op) + "] -> Silent 0");
        
        if (op && (strcmp(op, "bounds") == 0 || strcmp(op, "frame") == 0 || strcmp(op, "applicationFrame") == 0)) {
            uint32_t lr = (uint32_t)__builtin_return_address(0);
            LogToJava(">>>>>>>> [SIZE-CRITICAL] [nil " + std::string(op) + "] <<<<<<<< Caller: " + GetModuleInfoForAddress(lr) + " (Игра получит нули!)");
        }
        
        if (op && (strcmp(op, "startAnimation") == 0 || strcmp(op, "setAnimationInterval:") == 0)) {
            LogToJava("HLE_CRITICAL: Перехват [nil " + std::string(op) + "]! Восстанавливаем EAGLView из пепла NIB-а...");
            if (!s_lazyGlView) {
                uint32_t glViewClassAddr = 0;
                for (auto const& pair : g_appSymbols) {
                    if (pair.first.find("_OBJC_CLASS_$_") == 0 && FindMethodIMP(pair.second, "startAnimation") && FindMethodIMP(pair.second, "drawView")) {
                        glViewClassAddr = pair.second; break;
                    }
                }
                if (!glViewClassAddr) {
                    for (auto const& pair : g_appSymbols) {
                        if (pair.first.find("_OBJC_CLASS_$_") == 0 && FindMethodIMP(pair.second, "startAnimation")) {
                            glViewClassAddr = pair.second; break;
                        }
                    }
                }
                if (glViewClassAddr) {
                    s_lazyGlView = (void*)Stub_objc_msgSend((void*)glViewClassAddr, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    float x = 0, y = 0, w = (float)g_surfaceWidth, h = (float)g_surfaceHeight;
                    uint32_t px, py, pw, ph; memcpy(&px, &x, 4); memcpy(&py, &y, 4); memcpy(&pw, &w, 4); memcpy(&ph, &h, 4);
                    if (FindMethodIMP(((uint32_t*)s_lazyGlView)[0], "initWithCoder:")) {
                        s_lazyGlView = (void*)Stub_objc_msgSend(s_lazyGlView, "initWithCoder:", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        g_views[s_lazyGlView].frame = {0.0f, 0.0f, (float)g_surfaceWidth, (float)g_surfaceHeight};
                        Stub_objc_msgSend(s_lazyGlView, "setFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                    } else {
                        s_lazyGlView = (void*)Stub_objc_msgSend(s_lazyGlView, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                    }
                    g_mainView = s_lazyGlView;
                    LogToJava("HLE_CRITICAL: EAGLView воссоздан успешно!");
                }
            }
            if (s_lazyGlView) {
                return Stub_objc_msgSend(s_lazyGlView, op, a1, a2, a3, a4, a5, a6, a7, a8);
            }
        }
        return 0;
    }
    if (!isValidString(op)) return 0;
    std::string cName = GetObjCClassName(self);
    
    char ptrStr[32]; snprintf(ptrStr, sizeof(ptrStr), "0x%lx", (unsigned long)(uintptr_t)self);
    LogToJava(std::string("OBJC-CALL: [(") + cName + "*) " + ptrStr + " " + std::string(op) + "]");

    // === ЖЕСТКИЙ ПЕРЕХВАТ РЕКЛАМЫ И АНАЛИТИКИ ===
    if (cName.find("Flurry") != std::string::npos || cName.find("Tapjoy") != std::string::npos ||
        cName.find("MGGameDesc") != std::string::npos || cName == "UIWebView") {
        static int ad_spam_count = 0;
        if (ad_spam_count++ < 50) {
            LogToJava("OBJC-HIJACKED: [" + cName + " " + std::string(op) + "] -> Заглушен (Возвращаем self)");
        }
        return (uint64_t)(uintptr_t)self;
    }
    // ===========================================

    // === ОТЛАДКА ПЕРЕД GAMECENTER И СЕТЬЮ ===
    if (cName == "GameCenterManager" && (strcmp(op, "sharedInstance") == 0 || strcmp(op, "alloc") == 0 || strcmp(op, "init") == 0)) {
        LogToJava("[GC-DEBUG] Перехват инициализации GameCenterManager: " + std::string(op));
    }
    if (cName == "GameCenterManager") {
        LogToJava("[GC-TRACE] Вызов в GameCenterManager: " + std::string(op));
    }
    if (cName == "Reachability") {
        LogToJava("[REACH-DEBUG] Обращение к сети (Reachability): " + std::string(op));
    }
    // ===========================================

    // === ОБРАБОТКА БЛОКОВ (BLOCKS) ===
    if (strcmp(op, "copy") == 0) {
        if (cName.find("__NSConcreteStackBlock") != std::string::npos || 
            cName.find("__NSConcreteGlobalBlock") != std::string::npos || 
            cName.find("__NSConcreteMallocBlock") != std::string::npos) {
            return (uint64_t)(uintptr_t)wrap_Block_copy(self);
        }
    }
    // ===========================================

    // === БАЗОВЫЕ МЕТОДЫ NSOBJECT (Глобальный перехват) ===
    if (strcmp(op, "respondsToSelector:") == 0) {
        const char* targetSel = (const char*)a1;
        if (!targetSel) return 0;
        
        uint32_t checkIsa = ((uint32_t*)self)[0];
        
        // Если проверяем нативный класс, ищем метод в его таблице или у родителей
        if (checkIsa > 0x1000 && checkIsa != 0xDEADBEEF && ((uint32_t*)checkIsa)[0] != 0xDEADBEEF) {
            if (FindMethodIMP(checkIsa, targetSel)) {
                return 1;
            }
        }
        
        // Проверка базовых методов NSObject (они не присутствуют в бинарнике игры)
        if (strcmp(targetSel, "respondsToSelector:") == 0 || 
            strcmp(targetSel, "class") == 0 || 
            strcmp(targetSel, "retain") == 0 || 
            strcmp(targetSel, "release") == 0 || 
            strcmp(targetSel, "autorelease") == 0 ||
            strcmp(targetSel, "dealloc") == 0 ||
            strcmp(targetSel, "performSelectorOnMainThread:withObject:waitUntilDone:") == 0 ||
            strcmp(targetSel, "performSelector:withObject:afterDelay:") == 0 ||
            strcmp(targetSel, "scale") == 0 ||
            strcmp(targetSel, "displayLinkWithTarget:selector:") == 0) {
            LogToJava(std::string(">>>>>>>> [RESPONDS-TRACE] [") + cName + " respondsToSelector:@" + targetSel + "] -> 1 (HLE Base)");
            return 1;
        }
        
        // Если дошли сюда, метод реально отсутствует или это HLE без реализации
        LogToJava(std::string(">>>>>>>> [RESPONDS-TRACE] [") + cName + " respondsToSelector:@" + targetSel + "] -> 0 (NOT FOUND)");
        return 0;
    }
    if (strcmp(op, "conformsToProtocol:") == 0) {
        LogToJava(std::string("OBJC-TRACE: [") + cName + " conformsToProtocol:] -> 1 (STUB)");
        return 1;
    }
    // =====================================================

    // === ГЛОБАЛЬНЫЙ ФИЛЬТР БАЗОВЫХ МЕТОДОВ УПРАВЛЕНИЯ ПАМЯТЬЮ ===
    if (strcmp(op, "retain") == 0 || strcmp(op, "release") == 0 || strcmp(op, "autorelease") == 0 || 
        strcmp(op, "dealloc") == 0 || strcmp(op, "copy") == 0 || strcmp(op, "mutableCopy") == 0) {
        return (uint64_t)(uintptr_t)self; 
    }

    // ==========================================
    // ГЛОБАЛЬНЫЙ ПЕРЕХВАТ UI ДЛЯ ВСЕХ КЛАССОВ (Даже нативных EAGLView)
    // ==========================================
    if (strncmp(op, "performSelector", 15) == 0) {
        if (strcmp(op, "performSelector:") == 0) return Stub_objc_msgSend(self, (const char*)a1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (strcmp(op, "performSelector:withObject:") == 0) return Stub_objc_msgSend(self, (const char*)a1, a2, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (strcmp(op, "performSelector:withObject:withObject:") == 0) return Stub_objc_msgSend(self, (const char*)a1, a2, a3, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    if (strcmp(op, "performSelectorInBackground:withObject:") == 0) {
        struct ThreadArgs { void* target; const char* sel; void* arg; };
        ThreadArgs* targs = new ThreadArgs{self, (const char*)a1, a2};
        pthread_t t;
        pthread_create(&t, nullptr, [](void* p) -> void* {
            ThreadArgs* args = (ThreadArgs*)p;
            Stub_objc_msgSend(args->target, args->sel, args->arg, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            delete args;
            return nullptr;
        }, targs);
        return 0;
    }
    if (strcmp(op, "performSelectorOnMainThread:withObject:waitUntilDone:") == 0) {
        pthread_mutex_lock(&g_mainQueueMutex);
        g_mainQueue.push_back({self, (const char*)a1, a2});
        pthread_mutex_unlock(&g_mainQueueMutex);
        return 0;
    }
    if (strcmp(op, "performSelector:withObject:afterDelay:") == 0) {
        double delay = 0.0;
        uint32_t parts[2] = { (uint32_t)a3, (uint32_t)a4 };
        memcpy(&delay, parts, 8);
        if (delay <= 0.0 || delay > 100.0 || std::isnan(delay)) delay = 0.5;

        struct DelayArgs { void* target; const char* sel; void* arg; double delay; };
        DelayArgs* dargs = new DelayArgs{self, (const char*)a1, a2, delay};

        pthread_t t;
        pthread_create(&t, nullptr, [](void* p) -> void* {
            DelayArgs* args = (DelayArgs*)p;
            usleep((int)(args->delay * 1000000.0));
            pthread_mutex_lock(&g_mainQueueMutex);
            g_mainQueue.push_back({args->target, args->sel, args->arg});
            pthread_mutex_unlock(&g_mainQueueMutex);
            delete args;
            return nullptr;
        }, dargs);
        return 0;
    }
    if (strcmp(op, "addSubview:") == 0) {
        if (a1) {
            std::string childClass = GetObjCClassName(a1);
            std::string parentClass = GetObjCClassName(self);
            
            // --- UI BLOCKER: ЗАПРЕЩАЕМ ОВЕРЛЕЯМ ПЕРЕКРЫВАТЬ ЭКРАН ---
            if (childClass.find("Applifier") != std::string::npos || 
                childClass.find("WebView") != std::string::npos ||
                childClass.find("Flurry") != std::string::npos ||
                (parentClass == "EAGLView" && childClass == "EAGLView")) {
                LogToJava("HLE-UI: Блокируем addSubview: для оверлея " + childClass + " поверх " + parentClass);
                return 0; 
            }
            // --------------------------------------------------------

            if (g_views.count(a1) && g_views[a1].parent) {
                auto& siblings = g_views[g_views[a1].parent].children;
                for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                    if (*it == a1) { siblings.erase(it); break; }
                }
            }
            g_views[a1].parent = self;
            g_views[self].children.push_back(a1);
            if (g_views.count(self) && g_views[self].type == "UIWindow") {
                g_mainView = a1;
                LogToJava("HLE: [UIWindow addSubview:] перехвачен глобально! g_mainView обновлен.");
                
                for (auto const& pair : g_viewControllersViews) {
                    if (pair.second == a1) {
                        void* vc = pair.first;
                        uint32_t vcIsa = ((uint32_t*)vc)[0];
                        if (vcIsa > 0x1000) {
                            void* impVDA = FindMethodIMP(vcIsa, "viewDidAppear:");
                            if (impVDA) {
                                typedef void (*VDAFunc)(void*, const char*, uint32_t);
                                ((VDAFunc)impVDA)(vc, "viewDidAppear:", 1);
                            } else {
                                Stub_objc_msgSend(vc, "viewDidAppear:", (void*)1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            }
                        }
                    }
                }
            }
        }
        return 0;
    }
    if (strcmp(op, "removeFromSuperview") == 0) {
        if (g_views.count(self) && g_views[self].parent) {
            auto& siblings = g_views[g_views[self].parent].children;
            for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                if (*it == self) { siblings.erase(it); break; }
            }
            g_views[self].parent = nullptr;
        }
        return 0;
    }
    if (strcmp(op, "subviews") == 0) {
        uint32_t* arrInst = (uint32_t*)calloc(1, 32);
        arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
        if (g_views.count(self)) {
            for (void* child : g_views[self].children) {
                g_arrays[arrInst].push_back(child);
            }
        }
        return (uint64_t)(uintptr_t)arrInst;
    }
    if (strcmp(op, "setFrame:") == 0) {
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        float x = saved_s[0], y = saved_s[1], w = saved_s[2], h = saved_s[3];
        uint32_t ix, iy, iw, ih; memcpy(&ix, &x, 4); memcpy(&iy, &y, 4); memcpy(&iw, &w, 4); memcpy(&ih, &h, 4);
        LogToJava(">>>>>>>> [SIZE-CRITICAL] setFrame: вызван для " + GetObjCClassName(self) + " <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr));
        LogToJava("  Устанавливают (FPU Floats): x=" + std::to_string(x) + " y=" + std::to_string(y) + " w=" + std::to_string(w) + " h=" + std::to_string(h));
        char rawBuf[128]; snprintf(rawBuf, sizeof(rawBuf), "  Raw HEX: s0=0x%08X, s1=0x%08X, s2=0x%08X, s3=0x%08X", ix, iy, iw, ih);
        LogToJava(rawBuf);
        g_views[self].frame = {x, y, w, h}; 
        uint32_t isa = 0;
        if (SafeRead32((uintptr_t)self, &isa) && isa > 0x1000 && isa != 0xDEADBEEF && ((uint32_t*)isa)[0] != 0xDEADBEEF) {
            void* imp = FindMethodIMP(isa, op);
            if (imp) {
#if defined(__arm__)
                asm volatile (
                    "vldr s0, [%0]\n"
                    "vldr s1, [%0, #4]\n"
                    "vldr s2, [%0, #8]\n"
                    "vldr s3, [%0, #12]\n"
                    : : "r"(saved_s) : "s0", "s1", "s2", "s3"
                );
#endif
                typedef uint64_t (*MethodType)(void*, const char*, void*, void*, void*, void*, void*, void*, void*, void*);
                return ((MethodType)imp)(self, op, a1, a2, a3, a4, a5, a6, a7, a8);
            }
        }
        return 0;
    }
    if (strcmp(op, "setBounds:") == 0) {
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        float bx = saved_s[0], by = saved_s[1], bw = saved_s[2], bh = saved_s[3];
        LogToJava("[SIZE-TRACE] setBounds: вызван для " + GetObjCClassName(self) + ". Caller: " + GetModuleInfoForAddress(lr) + " | Устанавливают (FPU): x=" + std::to_string(bx) + " y=" + std::to_string(by) + " w=" + std::to_string(bw) + " h=" + std::to_string(bh));
        if (g_views[self].frame.size() >= 4) { g_views[self].frame[2] = bw; g_views[self].frame[3] = bh; }
        else g_views[self].frame = {0, 0, bw, bh};
        uint32_t isa = 0;
        if (SafeRead32((uintptr_t)self, &isa) && isa > 0x1000 && isa != 0xDEADBEEF && ((uint32_t*)isa)[0] != 0xDEADBEEF) {
            void* imp = FindMethodIMP(isa, op);
            if (imp) {
#if defined(__arm__)
                asm volatile (
                    "vldr s0, [%0]\n"
                    "vldr s1, [%0, #4]\n"
                    "vldr s2, [%0, #8]\n"
                    "vldr s3, [%0, #12]\n"
                    : : "r"(saved_s) : "s0", "s1", "s2", "s3"
                );
#endif
                typedef uint64_t (*MethodType)(void*, const char*, void*, void*, void*, void*, void*, void*, void*, void*);
                return ((MethodType)imp)(self, op, a1, a2, a3, a4, a5, a6, a7, a8);
            }
        }
        return 0;
    }
    if (strcmp(op, "setTransform:") == 0 || strcmp(op, "bringSubviewToFront:") == 0) return 0;
    if (strcmp(op, "sizeToFit") == 0) {
        if (g_views.count(self)) {
            float estW = g_views[self].text.empty() ? 100.0f : g_views[self].text.length() * 8.0f;
            g_views[self].frame[2] = estW; g_views[self].frame[3] = 30.0f;
        }
        return 0;
    }
    if (strcmp(op, "becomeFirstResponder") == 0 || strcmp(op, "resignFirstResponder") == 0) return 0;
    if (strcmp(op, "initWithFrame:") == 0) {
        float x, y, w, h; memcpy(&x, &a1, 4); memcpy(&y, &a2, 4); memcpy(&w, &a3, 4); memcpy(&h, &a4, 4);
        g_views[self].frame = {x, y, w, h}; 
    }
    if (strcmp(op, "setCenter:") == 0) {
        float cx, cy; memcpy(&cx, &a1, 4); memcpy(&cy, &a2, 4);
        if (g_views[self].frame.size() >= 4) {
            g_views[self].frame[0] = cx - g_views[self].frame[2]/2.0f;
            g_views[self].frame[1] = cy - g_views[self].frame[3]/2.0f;
        } else g_views[self].frame = {cx - 40, cy - 40, 80, 80};
        return 0;
    }
    if (strcmp(op, "setTitle:forState:") == 0 || strcmp(op, "setText:") == 0 || strcmp(op, "setTitle:") == 0) {
        g_views[self].text = GetNSString(a1); return 0;
    }
    if (strcmp(op, "text") == 0) return (uint64_t)(uintptr_t)CreateNSString(g_views[self].text);
    if (strcmp(op, "addTarget:action:forControlEvents:") == 0) {
        ButtonHit bh; bh.button = self; bh.target = a1; bh.action = a2 ? (const char*)a2 : ""; g_buttons.push_back(bh); return 0;
    }
    if (strcmp(op, "setBackgroundColor:") == 0) {
        g_views[self].uiColorObj = a1;
        if (g_uiColors.count(a1)) { g_views[self].bgColor = g_uiColors[a1]; g_views[self].hasBg = true; }
        return 0;
    }
    if (strcmp(op, "backgroundColor") == 0) return (uint64_t)(uintptr_t)g_views[self].uiColorObj;
    if (strcmp(op, "setTextColor:") == 0) {
        if (g_uiColors.count(a1)) { g_views[self].textColor = g_uiColors[a1]; g_views[self].hasTextCol = true; }
        return 0;
    }
    if (strcmp(op, "layer") == 0) return (uint64_t)(uintptr_t)self;
    if (strcmp(op, "setCornerRadius:") == 0) {
        float r; uint32_t v = (uint32_t)a1; memcpy(&r, &v, 4); g_views[self].cornerRadius = r; return 0;
    }
    if (strcmp(op, "setAutoresizingMask:") == 0) return 0;
    if (strcmp(op, "setHidden:") == 0) { g_views[self].hidden = (a1 != nullptr); return 0; }
    if (strcmp(op, "isHidden") == 0) return g_views[self].hidden ? 1 : 0;
    if (strcmp(op, "setTag:") == 0) { g_dictionaries[self][(void*)0x1234] = a1; return 0; }
    if (strcmp(op, "tag") == 0) return (uint64_t)(uintptr_t)g_dictionaries[self][(void*)0x1234];
    if (strcmp(op, "setUserInteractionEnabled:") == 0) { g_views[self].userInteraction = (a1 != nullptr); return 0; }
    if (strcmp(op, "isUserInteractionEnabled") == 0) return g_views[self].userInteraction ? 1 : 0;
    if (strcmp(op, "setExclusiveTouch:") == 0) { g_views[self].exclusiveTouch = (a1 != nullptr); return 0; }
    if (strcmp(op, "setFont:") == 0) { g_views[self].font = a1; return 0; }
    if (strcmp(op, "font") == 0) {
        if (!g_views[self].font) {
            uint32_t* fontInst = (uint32_t*)calloc(1, 32); 
            fontInst[0] = g_hleClasses.count("UIFont") ? (uint32_t)g_hleClasses["UIFont"] : 0xDEADBEEF;
            g_views[self].font = fontInst;
        }
        return (uint64_t)(uintptr_t)g_views[self].font;
    }
    if (strcmp(op, "setTextAlignment:") == 0) { g_views[self].textAlignment = (int)(uintptr_t)a1; return 0; }
    if (strcmp(op, "setLineBreakMode:") == 0) { g_views[self].lineBreakMode = (int)(uintptr_t)a1; return 0; }
    if (strcmp(op, "lineBreakMode") == 0) return g_views[self].lineBreakMode;
    if (strcmp(op, "setClipsToBounds:") == 0 || strcmp(op, "setMultipleTouchEnabled:") == 0 || strcmp(op, "setModalTransitionStyle:") == 0 || strcmp(op, "setContentScaleFactor:") == 0) return 0;
    if (strcmp(op, "contentScaleFactor") == 0 || strcmp(op, "scale") == 0) { 
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        LogToJava(">>>>>>>> [SIZE-CRITICAL] " + std::string(op) + " <<<<<<<< Caller: " + GetModuleInfoForAddress(lr) + " | Class: " + cName);
        float s = 1.0f; 
        g_fpu_ret[0] = s; g_fpu_ret_flag = 1;
        uint32_t ret; memcpy(&ret, &s, 4); return ret; 
    }
    if (strcmp(op, "size") == 0) {
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        float w = (float)g_surfaceWidth;
        float h = (float)g_surfaceHeight;
        LogToJava(">>>>>>>> [SIZE-CRITICAL] NORMAL MSG_SEND size <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr) + " | Class: " + cName);
        LogToJava("  Returning (FPU s0, s1): w=" + std::to_string(w) + " h=" + std::to_string(h));
        
        g_fpu_ret[0] = w;
        g_fpu_ret[1] = h;
        g_fpu_ret_flag = 1;
        
        uint32_t iw, ih; memcpy(&iw, &w, 4); memcpy(&ih, &h, 4);
        return ((uint64_t)ih << 32) | iw; 
    }
    if (strcmp(op, "setOpaque:") == 0) { g_views[self].opaque = (a1 != nullptr); return 0; }
    if (strcmp(op, "setAlpha:") == 0) {
        float a; uint32_t v = (uint32_t)a1; memcpy(&a, &v, 4); g_views[self].alpha = a; return 0;
    }

    if (strcmp(op, "setDrawableProperties:") == 0) {
        g_layerDrawableProperties[self] = a1; // Сохраняем NSDictionary настроек EGL
        return 0;
    }
    if (strcmp(op, "setContentSize:") == 0 || strcmp(op, "setNumberOfLines:") == 0) {
        return 0;
    }
    if (strcmp(op, "setContentOffset:animated:") == 0 || strcmp(op, "setContentOffset:") == 0) {
        return 0;
    }
    if (strcmp(op, "startAnimating") == 0 || strcmp(op, "stopAnimating") == 0) {
        return 0;
    }

    if (strcmp(op, "frame") == 0 || strcmp(op, "bounds") == 0 || strcmp(op, "applicationFrame") == 0) {
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        float w = (float)g_surfaceWidth;
        float h = (float)g_surfaceHeight;
        float x = 0.0f;
        float y = 0.0f;
        if (cName.find("UIScreen") != std::string::npos) {
            w = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
            h = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
        } else if (g_views.count(self)) { 
            x = g_views[self].frame[0]; y = g_views[self].frame[1];
            w = g_views[self].frame[2]; h = g_views[self].frame[3]; 
        }
        
        float rect[4] = {x, y, w, h};
        LogToJava(">>>>>>>> [SIZE-CRITICAL] NORMAL MSG_SEND " + std::string(op) + " <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr));
        LogToJava("  Class: " + cName + " | ptr: " + ptrStr);
        LogToJava("  Returning: x=" + std::to_string(x) + " y=" + std::to_string(y) + " w=" + std::to_string(w) + " h=" + std::to_string(h));
        
        g_fpu_ret[0] = rect[0];
        g_fpu_ret[1] = rect[1];
        g_fpu_ret[2] = rect[2];
        g_fpu_ret[3] = rect[3];
        g_fpu_ret_flag = 1;
        
        uint32_t* r = (uint32_t*)rect;
        return ((uint64_t)r[1] << 32) | r[0];
    }

    uint32_t* self_ptr = (uint32_t*)self;
    uint32_t isa = 0;
    if ((uintptr_t)self >= 0x1000 && (uintptr_t)self % 4 == 0) {
        if (!SafeRead32((uintptr_t)self, &isa)) {
            LogToJava("OBJC-ERROR: Unreadable self pointer " + std::to_string((uintptr_t)self));
            return 0;
        }
    } else {
        LogToJava("OBJC-ERROR: Invalid/Unaligned self pointer " + std::to_string((uintptr_t)self));
        return 0;
    }

    // Ветка 1: Классы HLE
    if (isa == 0xDEADBEEF) {
        HLEClass* hleCls = (HLEClass*)self;
        std::string clsName = hleCls->className;
        
        if (strcmp(op, "alloc") == 0 || strcmp(op, "new") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; 
            if (clsName == "UISwitch") { g_views[inst].type = "UISwitch"; g_views[inst].frame = {0, 0, 51, 31}; }
            else if (clsName == "UIViewController") {} // Без типа, это не View
            else if (clsName.find("UI") == 0) g_views[inst].type = clsName;
            return (uint64_t)(uintptr_t)inst;
        }
        if (strcmp(op, "buttonWithType:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; 
            g_views[inst].type = "UIButton"; return (uint64_t)(uintptr_t)inst;
        }
        
        // Обработка цветов
        if (clsName == "UIColor") {
            if (strcmp(op, "colorWithWhite:alpha:") == 0) {
                float w = 0.0f, a = 1.0f;
                uint32_t vW = (uint32_t)a1, vA = (uint32_t)a2;
                memcpy(&w, &vW, 4); memcpy(&a, &vA, 4);
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                g_uiColors[inst] = {w, w, w, a};
                return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "colorWithRed:green:blue:alpha:") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                static int color_calls = 0;
                if (color_calls++ == 0) {
                    // Самый первый вызов - это фон HardUITestViewController
                    g_uiColors[inst] = {0.1f, 0.1f, 0.15f, 1.0f};
                } else {
                    // Все остальные вызовы в этом приложении — это круги мультитача!
                    g_uiColors[inst] = {(rand()%100)/100.0f, (rand()%100)/100.0f, (rand()%100)/100.0f, 0.7f};
                }
                return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "clearColor") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                g_uiColors[inst] = {0, 0, 0, 0}; return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "whiteColor") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                g_uiColors[inst] = {1, 1, 1, 1}; return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "darkGrayColor") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                // Экран MultiTouchView должен пропускать куб (полная прозрачность)
                g_uiColors[inst] = {0.0f, 0.0f, 0.0f, 0.0f}; 
                return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "blackColor") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                g_uiColors[inst] = {0, 0, 0, 1}; return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "greenColor") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                g_uiColors[inst] = {0.0f, 1.0f, 0.0f, 1.0f}; return (uint64_t)(uintptr_t)inst;
            }
            if (strncmp(op, "colorWith", 9) == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                g_uiColors[inst] = {1, 1, 1, 1}; return (uint64_t)(uintptr_t)inst;
            }
        }

        if (strcmp(op, "dictionary") == 0 || strcmp(op, "dictionaryWithCapacity:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (strcmp(op, "valueWithPointer:") == 0 || strcmp(op, "valueWithNonretainedObject:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; inst[1] = (uint32_t)a1; return (uint64_t)(uintptr_t)inst;
        }
        if (strcmp(op, "stringWithUTF8String:") == 0) {
            const char* cstr = a1 ? (const char*)a1 : "";
            if (strstr(cstr, "pngConf") || strstr(cstr, "jungle")) {
                LogToJava(std::string("OBJC-DEBUG: [stringWithUTF8String:] Вход: [") + cstr + "]");
                LogToJava("OBJC-DEBUG: HEX: " + DumpHexToString(cstr, strlen(cstr) + 8));
            }
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; inst[2] = (uint32_t)a1; return (uint64_t)(uintptr_t)inst;
        }

        // Foundation HLE Additions
        if (strcmp(op, "array") == 0 || strcmp(op, "arrayWithCapacity:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
            return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSNumber" && strcmp(op, "numberWithBool:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; inst[1] = (uint32_t)a1;
            return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSNumber" && strcmp(op, "numberWithInt:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; inst[1] = (uint32_t)a1; inst[2] = 9; // kCFNumberIntType
            return (uint64_t)(uintptr_t)inst;
        }
        if (strcmp(op, "arrayWithObjects:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
            if (a1) g_arrays[inst].push_back(a1); else return (uint64_t)(uintptr_t)inst;
            if (a2) g_arrays[inst].push_back(a2); else return (uint64_t)(uintptr_t)inst;
            if (a3) g_arrays[inst].push_back(a3); else return (uint64_t)(uintptr_t)inst;
            if (a4) g_arrays[inst].push_back(a4); else return (uint64_t)(uintptr_t)inst;
            if (a5) g_arrays[inst].push_back(a5); else return (uint64_t)(uintptr_t)inst;
            return (uint64_t)(uintptr_t)inst;
        }
        if (strcmp(op, "dictionaryWithObjectsAndKeys:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
            if (a1) g_dictionariesHLE[inst][GetNSString(a2)] = a1; else return (uint64_t)(uintptr_t)inst;
            if (a3) g_dictionariesHLE[inst][GetNSString(a4)] = a3; else return (uint64_t)(uintptr_t)inst;
            if (a5) g_dictionariesHLE[inst][GetNSString(a6)] = a5; else return (uint64_t)(uintptr_t)inst;
            if (a7) g_dictionariesHLE[inst][GetNSString(a8)] = a7; else return (uint64_t)(uintptr_t)inst;
            return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSFileManager" && strcmp(op, "defaultManager") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSUserDefaults" && strcmp(op, "standardUserDefaults") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSURLCache" && strcmp(op, "setSharedURLCache:") == 0) return 0;
        if (clsName == "NSURLRequest" && strcmp(op, "requestWithURL:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; inst[1] = (uint32_t)a1; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSDictionary" && strcmp(op, "dictionaryWithContentsOfFile:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary");
            return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSDate") {
            if (strcmp(op, "date") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "timeIntervalSince1970") == 0) {
                double t = (double)Stub_mach_absolute_time() / 1000000000.0;
                memcpy(g_fpu_ret, &t, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &t, 8); return ret;
            }
        }
        if (clsName == "NSKeyedUnarchiver") {
            if (strcmp(op, "unarchiveObjectWithFile:") == 0) {
                std::string path = GetNSString(a1);
                uint32_t* dictInst = (uint32_t*)calloc(1, 32); dictInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary");
                return (uint64_t)(uintptr_t)dictInst; // Возвращаем NSDictionary, так как игра ждет его для setM_dictAchievementsToSend
            }
            if (strcmp(op, "unarchiveObjectWithData:") == 0) {
                uint32_t* dictInst = (uint32_t*)calloc(1, 32); dictInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary");
                return (uint64_t)(uintptr_t)dictInst;
            }
            // АНОМАЛИИ PopCap: Ошибочная посылка строковых сообщений классу из-за повреждения памяти
            if (strcmp(op, "stringByReplacingOccurrencesOfString:withString:") == 0) return (uint64_t)(uintptr_t)CreateNSString("");
            if (strcmp(op, "intValue") == 0) return 0;
        }
        if (clsName == "NSKeyedArchiver" && strcmp(op, "archivedDataWithRootObject:") == 0) {
            uint32_t* nsData = (uint32_t*)calloc(1, 32);
            nsData[0] = g_hleClasses.count("NSData") ? (uint32_t)g_hleClasses["NSData"] : 0xDEADBEEF;
            return (uint64_t)(uintptr_t)nsData;
        }
        if (clsName == "NSArray" && strcmp(op, "arrayWithContentsOfFile:") == 0) {
            std::string path = GetNSString(a1);
            uint32_t* arrInst = (uint32_t*)calloc(1, 32); arrInst[0] = (uint32_t)self;
            std::ifstream in(path);
            if (in.is_open()) {
                std::string line;
                while (std::getline(in, line)) {
                    size_t start = line.find("<string>"); size_t end = line.find("</string>");
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string val = line.substr(start + 8, end - (start + 8));
                        g_arrays[arrInst].push_back(CreateNSString(val));
                    }
                }
            }
            return (uint64_t)(uintptr_t)arrInst;
        }
        
        if (clsName == "NSNotificationCenter" && strcmp(op, "defaultCenter") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "UIApplication" && strcmp(op, "sharedApplication") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "UIPasteboard") {
            if (strcmp(op, "generalPasteboard") == 0 || strcmp(op, "pasteboardWithName:create:") == 0 || strcmp(op, "pasteboardWithUniqueName") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
            }
        }
        if (clsName == "SKPaymentQueue") {
            if (strcmp(op, "defaultQueue") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "addTransactionObserver:") == 0) return 0;
        }
// --- AVFoundation & NSBundle HLE Class Methods ---
        if (clsName == "UINib" && strcmp(op, "nibWithNibName:bundle:") == 0) {
            std::string nibName = GetNSString(a1);
            LogToJava("HLE: Запрошено создание UINib для: " + nibName);
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSBundle" && strcmp(op, "mainBundle") == 0) {
            LogToJava("HLE-TRACE: Выделение нового NSBundle mainBundle");
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSURL" && strcmp(op, "fileURLWithPath:") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; inst[1] = (uint32_t)a1; // Сохраняем указатель на NSString
            return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "AVAudioSession" && strcmp(op, "sharedInstance") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        // ------------------------------------------------

        if (clsName == "GKLocalPlayer" && strcmp(op, "localPlayer") == 0) {
            static uint32_t* playerInst = nullptr;
            if (!playerInst) { 
                playerInst = (uint32_t*)calloc(1, 32); 
                playerInst[0] = g_hleClasses.count("GKLocalPlayer") ? (uint32_t)g_hleClasses["GKLocalPlayer"] : 0xDEADBEEF; 
            }
            return (uint64_t)(uintptr_t)playerInst;
        }
        if (clsName == "GKAchievement") {
            if (strcmp(op, "loadAchievementsWithCompletionHandler:") == 0) {
                LogToJava("HLE: GKAchievement loadAchievementsWithCompletionHandler: вызван. Выполняем блок...");
                if (a1) {
                    uint32_t* block = (uint32_t*)a1;
                    typedef void (*BlockInvoke)(void*, void*, void*);
                    BlockInvoke invoke = (BlockInvoke)block[3];
                    if (invoke) {
                        uint32_t* arrInst = (uint32_t*)calloc(1, 32); 
                        arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
                        invoke(a1, arrInst, nullptr);
                    }
                }
                return 0;
            }
        }
        if (clsName == "UIAccelerometer" && strcmp(op, "sharedAccelerometer") == 0) {
            static uint32_t* accelInst = nullptr;
            if (!accelInst) {
                accelInst = (uint32_t*)calloc(1, 32);
                accelInst[0] = g_hleClasses.count("UIAccelerometer") ? (uint32_t)g_hleClasses["UIAccelerometer"] : 0xDEADBEEF;
            }
            return (uint64_t)(uintptr_t)accelInst;
        }
        if (clsName == "NSLocale" && strcmp(op, "preferredLanguages") == 0) {
            uint32_t* arrInst = (uint32_t*)calloc(1, 32);
            arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
            g_arrays[arrInst].push_back(CreateNSString("en"));
            return (uint64_t)(uintptr_t)arrInst;
        }
        if (clsName == "UIDevice" && strcmp(op, "currentDevice") == 0) {
            static uint32_t* devInst = nullptr;
            if (!devInst) { devInst = (uint32_t*)calloc(1, 32); devInst[0] = (uint32_t)g_hleClasses["UIDevice"]; }
            return (uint64_t)(uintptr_t)devInst;
        }
        if (clsName == "NSMutableString") {
            if (strcmp(op, "stringWithCapacity:") == 0) {
                return (uint64_t)(uintptr_t)CreateNSString("");
            }
            if (strcmp(op, "appendFormat:") == 0) {
                return (uint64_t)(uintptr_t)CreateNSString(""); // Защита от вызова инстанс-метода у класса
            }
        }
        if (clsName == "NSString" && strcmp(op, "stringWithString:") == 0) {
            return (uint64_t)(uintptr_t)CreateNSString(GetNSString(a1));
        }
        if (clsName == "NSString" && strcmp(op, "stringByReplacingOccurrencesOfString:withString:") == 0) {
            // Игра ошибочно вызывает это у класса из-за мусорного указателя. Отдаем пустую строку, чтобы разорвать цепь.
            return (uint64_t)(uintptr_t)CreateNSString(""); 
        }
        if (clsName == "NSString" && strcmp(op, "stringWithCString:encoding:") == 0) {
            const char* cstr = a1 ? (const char*)a1 : "";
            if (strstr(cstr, "pngConf") || strstr(cstr, "jungle")) {
                LogToJava(std::string("OBJC-DEBUG: [stringWithCString:encoding:] Вход: [") + cstr + "]");
                LogToJava("OBJC-DEBUG: HEX: " + DumpHexToString(cstr, strlen(cstr) + 8));
            }
            return (uint64_t)(uintptr_t)CreateNSString(cstr);
        }
        if (clsName == "NSString" && strcmp(op, "stringWithContentsOfFile:encoding:error:") == 0) {
            std::string path = GetNSString(a1);
            std::ifstream in(path);
            if (in.is_open()) {
                std::stringstream buffer; buffer << in.rdbuf();
                return (uint64_t)(uintptr_t)CreateNSString(buffer.str());
            }
            return 0;
        }
        if (clsName == "NSString" && strcmp(op, "stringWithFormat:") == 0) {
            std::string fmt = GetNSString(a1);
            void* args[] = {a2, a3, a4, a5};
            int argIdx = 0; std::string res = "";
            for (size_t i = 0; i < fmt.length(); i++) {
                if (fmt[i] == '%' && i + 1 < fmt.length()) {
                    char type = fmt[i+1];
                    void* arg = argIdx < 4 ? args[argIdx++] : nullptr;
                    if (type == '@') res += GetNSString(arg);
                    else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
                    else if (type == 's') res += arg ? (const char*)arg : "null";
                    else { res += "%"; res += type; }
                    i++;
                } else res += fmt[i];
            }
            return (uint64_t)(uintptr_t)CreateNSString(res);
        }

        if (clsName == "EAGLContext" && strcmp(op, "setCurrentContext:") == 0) {
            g_currentEAGLContext = a1;
            LogToJava("HLE_DEBUG: [EAGLContext setCurrentContext:] - Игра привязала контекст!");
            return 1;
        }
        if (clsName == "EAGLContext" && strcmp(op, "currentContext") == 0) {
            return (uint64_t)(uintptr_t)g_currentEAGLContext;
        }
        if (clsName == "EAGLContext" && strcmp(op, "initWithAPI:") == 0) {
            LogToJava("HLE_DEBUG: [EAGLContext initWithAPI:] - Создание контекста API: " + std::to_string((int)(uintptr_t)a1));
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "EAGLContext" && strcmp(op, "sharegroup") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); 
            inst[0] = g_hleClasses.count("EAGLSharegroup") ? (uint32_t)g_hleClasses["EAGLSharegroup"] : 0xDEADBEEF; 
            return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "UIScreen" && strcmp(op, "mainScreen") == 0) {
            LogToJava("HLE-TRACE: Выделение нового UIScreen mainScreen");
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSRunLoop" && strcmp(op, "currentRunLoop") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "NSRunLoop" && strcmp(op, "mainRunLoop") == 0) {
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }
        if (clsName == "UIFont") {
            if (strcmp(op, "boldSystemFontOfSize:") == 0 || strcmp(op, "systemFontOfSize:") == 0 || strcmp(op, "fontWithName:size:") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
            }
        }
        if (clsName == "UIImage" && strcmp(op, "imageNamed:") == 0) {
            std::string imgName = GetNSString(a1);
            std::string fullPath = g_appBundlePath + "/" + imgName;
            int w, h, channels;
            uint8_t* data = stbi_load(fullPath.c_str(), &w, &h, &channels, 4);
            if (data) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                HLE_CGImage* cgImg = new HLE_CGImage{w, h, 32, data};
                inst[1] = (uint32_t)cgImg;
                return (uint64_t)(uintptr_t)inst;
            }
            LogToJava("HLE_ERROR: Не удалось загрузить картинку: " + fullPath);
            return 0;
        }
        if (clsName == "UIView" && strcmp(op, "setAnimationsEnabled:") == 0) return 0;
        if (clsName == "NSTimer") {
            if (strcmp(op, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:") == 0 ||
                strcmp(op, "timerWithTimeInterval:target:selector:userInfo:repeats:") == 0) {
                double interval = 0.0; uint32_t parts[2] = { (uint32_t)a1, (uint32_t)a2 }; memcpy(&interval, parts, 8);
                void* target = a3; const char* sel = (const char*)a4; bool repeats = (a6 != nullptr);
                LogToJava("HLE: NSTimer запущен! Интервал: " + std::to_string(interval) + " сел: " + sel);
                
                if (interval > 0.0 && interval <= 0.05) {
                    LogToJava("HLE: NSTimer распознан как Render Loop (>= 20 FPS). Отключаем IDLE_LOOP очистку.");
                    g_renderingStarted = true;
                }
                
                struct TimerArgs { void* target; const char* sel; double interval; bool repeats; };
                TimerArgs* targs = new TimerArgs{target, sel, interval, repeats};
                pthread_t t; pthread_create(&t, nullptr, [](void* p) -> void* {
                    TimerArgs* args = (TimerArgs*)p;
                    do {
                        usleep((int)(args->interval * 1000000.0));
                        pthread_mutex_lock(&g_mainQueueMutex);
                        g_mainQueue.push_back({args->target, args->sel, nullptr});
                        pthread_mutex_unlock(&g_mainQueueMutex);
                    } while(args->repeats);
                    delete args; return nullptr;
                }, targs);
                uint32_t* inst = (uint32_t*)calloc(1, 16); inst[0] = (uint32_t)self;
                return (uint64_t)(uintptr_t)inst;
            }
        }
        if (clsName == "CADisplayLink" && strcmp(op, "displayLinkWithTarget:selector:") == 0) {
            g_displayLinkTarget = a1; g_displayLinkSelector = (const char*)a2;
            uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
        }

        // --- HLE CLASS METHODS FIX ---
        if (clsName == "NSThread") {
            if (strcmp(op, "isMultiThreaded") == 0) return 1; // YES
            if (strcmp(op, "currentThread") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); 
                inst[0] = (uint32_t)self; 
                return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "isMainThread") == 0) return pthread_equal(pthread_self(), g_iosMainThread) ? 1 : 0;
            if (strcmp(op, "sleepForTimeInterval:") == 0) {
                double delay = 0; uint32_t parts[2] = { (uint32_t)a1, (uint32_t)a2 }; memcpy(&delay, parts, 8);
                if (delay <= 0.0 || delay > 10.0 || std::isnan(delay)) delay = 1.5;
                usleep((int)(delay * 1000000));
                return 0;
            }
        }
        if (clsName == "NSURL") {
            if (strcmp(op, "URLWithString:") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 16); inst[0] = (uint32_t)g_hleClasses["NSURL"]; inst[1] = (uint32_t)a1; return (uint64_t)(uintptr_t)inst;
            }
        }
        if (clsName == "NSURLRequest" || clsName == "NSMutableURLRequest") {
            if (strcmp(op, "requestWithURL:cachePolicy:timeoutInterval:") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
            }
        }
        if (clsName == "NSURLConnection") {
            if (strcmp(op, "sendSynchronousRequest:returningResponse:error:") == 0) {
                LogToJava("HLE: [NSURLConnection sendSynchronousRequest...] -> Отклонено (Offline Mode)");
                // Возвращаем 0 (nil), что симулирует ошибку соединения
                return 0;
            }
        }
        if (clsName == "NSMutableData" || clsName == "NSData") {
            if (strcmp(op, "data") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self; return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "dataWithBytes:length:") == 0) {
                uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)self;
                uint8_t* src = (uint8_t*)a1; uint32_t len = (uint32_t)(uintptr_t)a2;
                if (src && len > 0) {
                    uint8_t* binData = (uint8_t*)malloc(len);
                    memcpy(binData, src, len);
                    inst[1] = (uint32_t)binData;
                    inst[2] = len;
                }
                return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "bytes") == 0 || strcmp(op, "length") == 0) return 0;
        }
        if (clsName == "NSData") {
            if (strcmp(op, "dataWithContentsOfFile:") == 0) {
                std::string path = GetNSString(a1);
                std::ifstream in(path, std::ios::binary);
                if (in.is_open()) {
                    std::stringstream buffer; buffer << in.rdbuf();
                    return (uint64_t)(uintptr_t)CreateNSString(buffer.str());
                }
                return 0;
            }
        }
        // -----------------------------

        if (strcmp(op, "class") == 0) return (uint64_t)(uintptr_t)self;
        
        char ptrStr[32]; snprintf(ptrStr, sizeof(ptrStr), "0x%lx", (unsigned long)(uintptr_t)self);
        LogToJava(std::string("OBJC-TODO: Unimplemented HLE Class Method +[(") + clsName + "*) " + ptrStr + " " + std::string(op) + "]");
        return (uint64_t)(uintptr_t)self; 
    } 
    // Ветка 2: Экземпляры HLE
    else if (isa > 0x1000 && ((uint32_t*)isa)[0] == 0xDEADBEEF) {
        HLEClass* hleCls = (HLEClass*)isa; std::string clsName = hleCls->className;
        
        // --- Обработка цветов для UI Элементов ---
        // (Перемещено в глобальный перехват)
        // -----------------------------------------

        // --- HLE UIWindow & NIB Methods ---
        if (clsName == "UIWindow") {
            if (strcmp(op, "makeKeyAndVisible") == 0) {
                LogToJava("HLE: [UIWindow makeKeyAndVisible] вызван. Экран готов к прорисовке!");
                if (g_mainView) {
                    uint32_t viewIsa = ((uint32_t*)g_mainView)[0];
                    if (FindMethodIMP(viewIsa, "layoutSubviews")) {
                        LogToJava("HLE: Автоматический вызов [g_mainView layoutSubviews]");
                        Stub_objc_msgSend(g_mainView, "layoutSubviews", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    }
                }
                return 0;
            }
            if (strcmp(op, "setRootViewController:") == 0) {
                g_mainView = (void*)Stub_objc_msgSend(a1, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                LogToJava("HLE: [UIWindow setRootViewController:] вызван!");
                
                uint32_t vcIsa = ((uint32_t*)a1)[0];
                if (vcIsa > 0x1000 && ((uint32_t*)vcIsa)[0] == 0xDEADBEEF) {
                    HLEClass* hleCls = (HLEClass*)vcIsa;
                    if (strcmp(hleCls->className, "MPMoviePlayerViewController") == 0) {
                        LogToJava("HLE: Auto-playing Root MPMoviePlayerViewController!");
                        Stub_objc_msgSend(a1, "play", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    } else if (strcmp(hleCls->className, "UINavigationController") == 0) {
                        Stub_objc_msgSend(a1, "viewDidAppear:", (void*)1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    }
                } else {
                    void* impVDA = FindMethodIMP(vcIsa, "viewDidAppear:");
                    if (impVDA) {
                        LogToJava("HLE: Автоматический вызов [RootVC viewDidAppear:]");
                        typedef void (*VDAFunc)(void*, const char*, uint32_t);
                        ((VDAFunc)impVDA)(a1, "viewDidAppear:", 1);
                    }
                }
                return 0;
            }
        }
        if (clsName == "UINib" && strcmp(op, "instantiateWithOwner:options:") == 0) {
            LogToJava("HLE: Запрос на инстанциацию UINib (заглушка). Возвращаем пустой NSArray.");
            uint32_t* arrInst = (uint32_t*)calloc(1, 32);
            arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
            return (uint64_t)(uintptr_t)arrInst;
        }
        if (clsName == "NSBundle" && strcmp(op, "loadNibNamed:owner:options:") == 0) {
            std::string nibName = GetNSString(a1);
            LogToJava("HLE: Игра пытается загрузить NIB файл напрямую: " + nibName + ". Возвращаем пустой NSArray.");
            uint32_t* arrInst = (uint32_t*)calloc(1, 32);
            arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
            return (uint64_t)(uintptr_t)arrInst;
        }

        // --- AVFoundation & NSBundle HLE Instance Methods ---
        if (clsName == "NSURL") {
            if (strcmp(op, "initFileURLWithPath:") == 0 || strcmp(op, "initWithString:") == 0) {
                self_ptr[1] = (uint32_t)a1; // Сохраняем указатель на строку (NSString)
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "host") == 0) {
                std::string urlStr = self_ptr[1] ? GetNSString((void*)self_ptr[1]) : "";
                size_t pos = urlStr.find("://");
                if (pos != std::string::npos) {
                    size_t end = urlStr.find("/", pos + 3);
                    std::string hostStr = urlStr.substr(pos + 3, end == std::string::npos ? std::string::npos : end - (pos + 3));
                    return (uint64_t)(uintptr_t)CreateNSString(hostStr);
                }
                return (uint64_t)(uintptr_t)CreateNSString("localhost");
            }
            if (strcmp(op, "UTF8String") == 0) {
                if (self_ptr[1]) {
                    uint32_t* nsStr = (uint32_t*)self_ptr[1];
                    return (uint64_t)nsStr[2];
                }
                return (uint64_t)(uintptr_t)"";
            }
            if (strcmp(op, "URLByAppendingPathComponent:") == 0) {
                std::string base = self_ptr[1] ? GetNSString((void*)self_ptr[1]) : "";
                std::string comp = GetNSString(a1);
                if (!base.empty() && base.back() != '/') base += "/";
                uint32_t* inst = (uint32_t*)calloc(1, 16); 
                inst[0] = (uint32_t)g_hleClasses["NSURL"]; 
                inst[1] = (uint32_t)CreateNSString(base + comp);
                return (uint64_t)(uintptr_t)inst;
            }
        }
        if (clsName == "NSBundle" && strcmp(op, "executablePath") == 0) {
            return (uint64_t)(uintptr_t)CreateNSString(g_execPath);
        }
        if (clsName == "NSBundle" && (strcmp(op, "bundlePath") == 0 || strcmp(op, "resourcePath") == 0)) {
            return (uint64_t)(uintptr_t)CreateNSString(g_appBundlePath);
        }
        if (clsName == "NSBundle" && strcmp(op, "infoDictionary") == 0) {
            uint32_t* dictInst = (uint32_t*)calloc(1, 32); dictInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary"); return (uint64_t)(uintptr_t)dictInst;
        }
        if (clsName == "NSBundle" && strcmp(op, "objectForInfoDictionaryKey:") == 0) {
            std::string key = GetNSString(a1);
            if (key == "CFBundleVersion" || key == "CFBundleShortVersionString") return (uint64_t)(uintptr_t)CreateNSString("1.0");
            return (uint64_t)(uintptr_t)CreateNSString("1");
        }
        if (clsName == "NSBundle" && strcmp(op, "pathForResource:ofType:") == 0) {
            std::string name = GetNSString(a1);
            std::string ext = GetNSString(a2);
            std::string fullPath = g_appBundlePath + "/" + name + (ext.empty() ? "" : "." + ext);
            struct stat buffer;
            if (stat(fullPath.c_str(), &buffer) == 0) {
                LogToJava("HLE_DEBUG: [NSBundle pathForResource] НАЙДЕН: " + fullPath);
                return (uint64_t)(uintptr_t)CreateNSString(fullPath);
            }
            LogToJava("HLE_DEBUG: [NSBundle pathForResource] НЕ НАЙДЕН: " + fullPath);
            return 0; // На iOS этот метод возвращает nil, если файл не существует
        }
        if (clsName == "UIPasteboard") {
            if (strcmp(op, "string") == 0) return (uint64_t)(uintptr_t)CreateNSString("");
            if (strcmp(op, "setString:") == 0) return 0;
            if (strcmp(op, "dataForPasteboardType:") == 0) {
                uint32_t* nsData = (uint32_t*)calloc(1, 32);
                nsData[0] = g_hleClasses.count("NSData") ? (uint32_t)g_hleClasses["NSData"] : 0xDEADBEEF;
                return (uint64_t)(uintptr_t)nsData;
            }
            if (strcmp(op, "setData:forPasteboardType:") == 0) return 0;
            if (strcmp(op, "valueForPasteboardType:") == 0) return 0;
            if (strcmp(op, "setValue:forPasteboardType:") == 0) return 0;
        }
        if (clsName == "AVAudioSession") {
            if (strcmp(op, "setCategory:error:") == 0 || strcmp(op, "setActive:error:") == 0) return 1;
            if (strcmp(op, "setDelegate:") == 0) return 0;
            if (strcmp(op, "setPreferredHardwareSampleRate:error:") == 0) return 1;
            if (strcmp(op, "setPreferredIOBufferDuration:error:") == 0) return 1;
            if (strcmp(op, "currentHardwareSampleRate") == 0) {
                double sr = 44100.0;
                uint64_t ret; memcpy(&ret, &sr, 8);
                return ret;
            }
        }
        if (clsName == "AVAudioPlayer") {
            if (strcmp(op, "initWithContentsOfURL:error:") == 0) {
                uint32_t* urlInst = (uint32_t*)a1;
                std::string path = "Unknown";
                if (urlInst && urlInst[1]) path = GetNSString((void*)urlInst[1]);
                if (path == "" || path == "Unknown" || path.find("/") == std::string::npos) path = g_appBundlePath + "/test.mp3"; 
                LogToJava("HLE_DEBUG: Подготовка Audio с путем: " + path);
                AudioInitToJava(self, path);
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "initWithData:error:") == 0) {
                // Игры часто грузят короткие звуки напрямую из памяти.
                // В идеале тут надо писать a1 (NSData) во временный файл, но пока спасаем от краша
                LogToJava("HLE_DEBUG: Подготовка Audio из NSData (TODO: Сохранение во врем. файл)");
                AudioInitToJava(self, g_appBundlePath + "/test.mp3");
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "prepareToPlay") == 0) return 1;
            if (strcmp(op, "setNumberOfLoops:") == 0) {
                int loops = (int)(uintptr_t)a1;
                AudioSetLoopingToJava(self, loops < 0);
                return 0;
            }
            if (strcmp(op, "play") == 0) { AudioPlayToJava(self); return 1; }
            if (strcmp(op, "pause") == 0) { AudioPauseToJava(self); return 0; }
            if (strcmp(op, "stop") == 0) { AudioStopToJava(self); return 0; }
            if (strcmp(op, "isPlaying") == 0) { return AudioIsPlayingToJava(self) ? 1 : 0; }
            if (strcmp(op, "setVolume:") == 0) {
                float vol = 1.0f; 
#if defined(__arm__)
                asm volatile ("vmov.f32 %0, s0" : "=t"(vol));
#else
                uint32_t val = (uint32_t)a1; memcpy(&vol, &val, 4);
#endif
                AudioSetVolumeToJava(self, vol); return 0;
            }
            if (strcmp(op, "volume") == 0) { 
                float v = 1.0f; 
                g_fpu_ret[0] = v; g_fpu_ret_flag = 1;
                uint32_t ret; memcpy(&ret, &v, 4); return ret; 
            }
            if (strcmp(op, "duration") == 0) {
                double d = (double)AudioGetDurationToJava(self);
                memcpy(g_fpu_ret, &d, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &d, 8); return ret; // NSTimeInterval (double)
            }
            if (strcmp(op, "currentTime") == 0) {
                double c = (double)AudioGetCurrentTimeToJava(self);
                memcpy(g_fpu_ret, &c, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &c, 8); return ret; // NSTimeInterval (double)
            }
            if (strcmp(op, "setCurrentTime:") == 0) {
                double time = 0.0; 
#if defined(__arm__)
                asm volatile ("vmov.f64 %0, d0" : "=w"(time));
#else
                uint32_t parts[2] = { (uint32_t)a1, (uint32_t)a2 }; memcpy(&time, parts, 8);
#endif
                AudioSetCurrentTimeToJava(self, (float)time); return 0;
            }
            if (strcmp(op, "setDelegate:") == 0) return 0;
            if (strcmp(op, "dealloc") == 0) { AudioReleaseToJava(self); return (uint64_t)(uintptr_t)self; }
        }
        // ----------------------------------------------------

        
        if (clsName == "GKLocalPlayer" || cName == "GKLocalPlayer") {
            static bool s_isAuthenticated = false;
            
            if (strcmp(op, "isAuthenticated") == 0) {
                LogToJava(std::string("HLE_DEBUG: [GKLocalPlayer isAuthenticated] -> ") + (s_isAuthenticated ? "1 (YES)" : "0 (NO)"));
                return s_isAuthenticated ? 1 : 0; 
            }
            if (strcmp(op, "playerID") == 0) {
                LogToJava("[GK-DEBUG] Запрошен playerID, возвращаем G:1234567890");
                return (uint64_t)(uintptr_t)CreateNSString("G:1234567890");
            }
            if (strcmp(op, "alias") == 0) return (uint64_t)(uintptr_t)CreateNSString("DamnPlayer");
            if (strcmp(op, "authenticateWithCompletionHandler:") == 0) {
                LogToJava("HLE: authenticateWithCompletionHandler: вызван. Пробуем выполнить блок и меняем статус на авторизован...");
                s_isAuthenticated = true; // Успешно авторизуемся для последующих проверок
                if (a1) {
                    uint32_t* block = (uint32_t*)a1;
                    typedef void (*BlockInvoke)(void*, void*);
                    // В ABI Apple Blocks указатель на функцию (invoke) лежит со смещением 12 байт (индекс 3)
                    BlockInvoke invoke = (BlockInvoke)block[3]; 
                    if (invoke) {
                        invoke(a1, nullptr); // Дергаем коллбек, передаем nil вместо NSError
                    }
                }
                return 0;
            }
        }
        if (clsName == "UIDevice") {
            if (strcmp(op, "systemVersion") == 0) return (uint64_t)(uintptr_t)CreateNSString("5.0");
            if (strcmp(op, "model") == 0) return (uint64_t)(uintptr_t)CreateNSString("iPhone");
            if (strcmp(op, "name") == 0) return (uint64_t)(uintptr_t)CreateNSString("DamnDevice");
            if (strcmp(op, "UTF8String") == 0) return (uint64_t)(uintptr_t)"DamnDevice";
            if (strcmp(op, "uniqueGlobalDeviceIdentifier") == 0) return (uint64_t)(uintptr_t)CreateNSString("DamnDevice-1234-5678");
            if (strcmp(op, "userInterfaceIdiom") == 0) return 0; // 0 = Phone
            if (strcmp(op, "orientation") == 0) return 3; // 3 = UIDeviceOrientationLandscapeRight
            if (strcmp(op, "beginGeneratingDeviceOrientationNotifications") == 0) return 0;
            if (strcmp(op, "isGeneratingDeviceOrientationNotifications") == 0) return 0;
            if (strcmp(op, "uniqueIdentifier") == 0) return (uint64_t)(uintptr_t)CreateNSString("DamnWrapper-0000-1111");
        }
        if (clsName == "UIScreen") {
            if (strcmp(op, "displayLinkWithTarget:selector:") == 0) {
                g_displayLinkTarget = a1; g_displayLinkSelector = (const char*)a2;
                uint32_t* inst = (uint32_t*)calloc(1, 32); 
                inst[0] = g_hleClasses.count("CADisplayLink") ? (uint32_t)g_hleClasses["CADisplayLink"] : 0xDEADBEEF;
                return (uint64_t)(uintptr_t)inst;
            }
            if (strcmp(op, "currentMode") == 0) {
                uint32_t* modeInst = (uint32_t*)calloc(1, 32); modeInst[0] = (uint32_t)g_hleClasses["UIScreenMode"];
                return (uint64_t)(uintptr_t)modeInst;
            }
            if (strcmp(op, "scale") == 0) {
                uint32_t lr = (uint32_t)__builtin_return_address(0);
                LogToJava(">>>>>>>> [SIZE-CRITICAL] [UIScreen scale] <<<<<<<< Caller: " + GetModuleInfoForAddress(lr));
                float s = 1.0f; 
                g_fpu_ret[0] = s; g_fpu_ret_flag = 1;
                uint32_t ret; memcpy(&ret, &s, 4); return ret; 
            }
        }
        if (clsName == "UIScreenMode") {
            if (strcmp(op, "size") == 0) {
                float w = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
                float h = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
                g_fpu_ret[0] = w; g_fpu_ret[1] = h; g_fpu_ret_flag = 1;
                uint32_t iw, ih; memcpy(&iw, &w, 4); memcpy(&ih, &h, 4);
                return ((uint64_t)ih << 32) | iw; 
            }
        }

        if (clsName == "UIApplication") {
            if (strcmp(op, "statusBarOrientation") == 0) return 3; // UIDeviceOrientationLandscapeRight
            if (strcmp(op, "delegate") == 0) return (uint64_t)(uintptr_t)g_appDelegateInstance;
            if (strcmp(op, "keyWindow") == 0) {
                static uint32_t* dummyWindow = nullptr;
                if (!dummyWindow) {
                    dummyWindow = (uint32_t*)calloc(1, 32);
                    dummyWindow[0] = g_hleClasses.count("UIWindow") ? (uint32_t)g_hleClasses["UIWindow"] : 0xDEADBEEF;
                    g_views[dummyWindow].type = "UIWindow";
                }
                return (uint64_t)(uintptr_t)dummyWindow;
            }
            if (strcmp(op, "setStatusBarHidden:") == 0 || strcmp(op, "setStatusBarHidden:animated:") == 0) return 0;
            if (strcmp(op, "setStatusBarOrientation:animated:") == 0) return 0;
            if (strcmp(op, "setIdleTimerDisabled:") == 0) return 0;
            if (strcmp(op, "registerForRemoteNotificationTypes:") == 0) return 0;
            if (strcmp(op, "beginReceivingRemoteControlEvents") == 0) return 0;
            if (strcmp(op, "beginIgnoringInteractionEvents") == 0) { g_ignoringInteractionEvents++; return 0; }
            if (strcmp(op, "endIgnoringInteractionEvents") == 0) { if (g_ignoringInteractionEvents > 0) g_ignoringInteractionEvents--; return 0; }
            if (strcmp(op, "isIgnoringInteractionEvents") == 0) { return g_ignoringInteractionEvents > 0 ? 1 : 0; }
        }
        if (clsName == "UIButton") {
            if (strcmp(op, "setImage:forState:") == 0) return 0;
        }
        if (clsName == "UITextField") {
            if (strcmp(op, "setDelegate:") == 0) return 0;
            if (strcmp(op, "setClearsOnBeginEditing:") == 0) return 0;
            if (strcmp(op, "setClearButtonMode:") == 0) return 0;
            if (strcmp(op, "setKeyboardType:") == 0) return 0;
            if (strcmp(op, "setAutocorrectionType:") == 0) return 0;
        }
        if (clsName == "UIViewController") {
            if (strcmp(op, "view") == 0) {
                if (g_viewControllersViews.find(self) == g_viewControllersViews.end()) {
                    uint32_t* uiview_cls = (uint32_t*)ResolveSymbol("OBJC_CLASS_$_UIView");
                    void* view = (void*)Stub_objc_msgSend(uiview_cls, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    float x = 0, y = 0, w = (float)g_surfaceWidth, h = (float)g_surfaceHeight;
                    uint32_t px, py, pw, ph; memcpy(&px, &x, 4); memcpy(&py, &y, 4); memcpy(&pw, &w, 4); memcpy(&ph, &h, 4);
                    view = (void*)Stub_objc_msgSend(view, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                    g_views[view].type = "UIView";
                    g_viewControllersViews[self] = view;
                }
                return (uint64_t)(uintptr_t)g_viewControllersViews[self];
            }
            if (strcmp(op, "setView:") == 0) {
                g_viewControllersViews[self] = a1; return 0;
            }
        }
        if (clsName == "UIImageView" || clsName == "UIWebView") {
            if (strcmp(op, "setImage:") == 0) { g_dictionaries[self][(void*)0x1111] = a1; return 0; }
            if (strcmp(op, "image") == 0) return (uint64_t)(uintptr_t)g_dictionaries[self][(void*)0x1111];
            if (strcmp(op, "setContentMode:") == 0) return 0;
        }
        if (clsName == "UIActivityIndicatorView") {
            if (strcmp(op, "setActivityIndicatorViewStyle:") == 0) return 0;
        }
        if (clsName == "UIImage") {
            if (strcmp(op, "initWithData:") == 0) {
                uint32_t* nsData = (uint32_t*)a1;
                if (nsData && nsData[1] && nsData[2] > 0) {
                    uint8_t* binData = (uint8_t*)nsData[1];
                    uint32_t size = nsData[2];
                    int w, h, channels;
                    uint8_t* decoded = stbi_load_from_memory(binData, size, &w, &h, &channels, 4);
                    if (decoded) {
                        HLE_CGImage* cgImg = new HLE_CGImage{w, h, 32, decoded};
                        self_ptr[1] = (uint32_t)cgImg;
                        LogToJava("HLE: Успешно декодирован PNG из NSData (" + std::to_string(w) + "x" + std::to_string(h) + ")");
                    } else {
                        LogToJava("HLE_ERROR: Ошибка декодирования PNG из памяти!");
                    }
                }
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "CGImage") == 0) return (uint64_t)self_ptr[1];
        }
        if (clsName == "UITextView") {
            if (strcmp(op, "setEditable:") == 0) {
                g_views[self].textViewEditable = (a1 != nullptr);
                return 0;
            }
            if (strcmp(op, "scrollRangeToVisible:") == 0) return 0;
            if (strcmp(op, "setDelegate:") == 0) return 0;
        }
            if (clsName == "UIAccelerometer") {
        if (strcmp(op, "setDelegate:") == 0) {
            g_accelerometerDelegate = a1;
            return 0;
        }
        if (strcmp(op, "setUpdateInterval:") == 0) return 0;
    }
    if (clsName == "CMMotionManager") {
        if (strcmp(op, "setAccelerometerUpdateInterval:") == 0) return 0;
        if (strcmp(op, "startAccelerometerUpdates") == 0) return 0;
        if (strcmp(op, "accelerometerData") == 0) {
            static uint32_t* accelDataInst = nullptr;
            if (!accelDataInst) {
                accelDataInst = (uint32_t*)calloc(1, 32);
                accelDataInst[0] = g_hleClasses.count("CMAccelerometerData") ? (uint32_t)g_hleClasses["CMAccelerometerData"] : 0xDEADBEEF;
            }
            return (uint64_t)(uintptr_t)accelDataInst;
        }
    }
    if (clsName == "NSNotification") {
            if (strcmp(op, "name") == 0) return self_ptr[1];
            if (strcmp(op, "object") == 0) return self_ptr[2];
            if (strcmp(op, "userInfo") == 0) return self_ptr[3];
        }
        if (clsName == "NSNotificationCenter") {
            if (strcmp(op, "addObserver:selector:name:object:") == 0) {
                std::string notifName = GetNSString(a3);
                ObserverInfo obs; obs.observer = a1; obs.selector = (const char*)a2; obs.object = a4;
                g_notifications[notifName].push_back(obs);
                LogToJava("HLE: Зарегистрирован обсервер NSNotificationCenter: " + notifName + " sel: " + obs.selector);
                
                // Защита от гонки потоков: запускаем таймер прелоада ТОЛЬКО после регистрации обсервера
                if (notifName == "MPMoviePlayerContentPreloadDidFinishNotification") {
                    struct PreloadArgs { void* observer; std::string sel; void* player; };
                    PreloadArgs* args = new PreloadArgs{a1, obs.selector, a4 ? a4 : s_lastMoviePlayer};
                    pthread_t t;
                    pthread_create(&t, nullptr, [](void* p) -> void* {
                        PreloadArgs* a = (PreloadArgs*)p;
                        usleep(150000); // 150ms (Даем Java время прожевать MediaPlayer)
                        uint32_t* notifInst = (uint32_t*)calloc(1, 32);
                        notifInst[0] = g_hleClasses.count("NSNotification") ? (uint32_t)g_hleClasses["NSNotification"] : 0xDEADBEEF;
                        notifInst[1] = (uint32_t)CreateNSString("MPMoviePlayerContentPreloadDidFinishNotification");
                        notifInst[2] = (uint32_t)a->player;
                        
                        pthread_mutex_lock(&g_mainQueueMutex);
                        // Используем strdup, чтобы избежать повисшего указателя c_str() при реаллокации std::vector
                        g_mainQueue.push_back({a->observer, strdup(a->sel.c_str()), (void*)notifInst});
                        pthread_mutex_unlock(&g_mainQueueMutex);
                        
                        delete a;
                        return nullptr;
                    }, args);
                }
                return 0;
            }
            if (strcmp(op, "removeObserver:name:object:") == 0) {
                std::string notifName = a2 ? GetNSString(a2) : "";
                if (!notifName.empty()) {
                    auto& list = g_notifications[notifName];
                    for (auto it = list.begin(); it != list.end(); ) {
                        if (it->observer == a1 && (a3 == nullptr || it->object == a3)) {
                            it = list.erase(it);
                        } else {
                            ++it;
                        }
                    }
                } else {
                    for (auto& pair : g_notifications) {
                        auto& list = pair.second;
                        for (auto it = list.begin(); it != list.end(); ) {
                            if (it->observer == a1 && (a3 == nullptr || it->object == a3)) {
                                it = list.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                }
                LogToJava("HLE: Удален обсервер NSNotificationCenter (removeObserver:name:object:)");
                return 0;
            }
            if (strcmp(op, "removeObserver:") == 0) {
                for (auto& pair : g_notifications) {
                    auto& list = pair.second;
                    for (auto it = list.begin(); it != list.end(); ) {
                        if (it->observer == a1) it = list.erase(it);
                        else ++it;
                    }
                }
                LogToJava("HLE: Удален обсервер NSNotificationCenter (removeObserver:)");
                return 0;
            }
        }
        if (clsName == "SKPaymentQueue" && strcmp(op, "addTransactionObserver:") == 0) return 0;
        
        if (clsName == "NSIndexPath") {
            if (strcmp(op, "row") == 0) return self_ptr[1];
            if (strcmp(op, "section") == 0) return self_ptr[2];
        }
        if (clsName == "UITableView") {
            if (strcmp(op, "setDelegate:") == 0) { g_dictionaries[self][(void*)0x1007] = a1; return 0; }
            if (strcmp(op, "setDataSource:") == 0) { g_dictionaries[self][(void*)0x1008] = a1; return 0; }
            if (strcmp(op, "reloadData") == 0) {
                void* ds = g_dictionaries[self][(void*)0x1008];
                if (ds) {
                    uint32_t rows = Stub_objc_msgSend(ds, "tableView:numberOfRowsInSection:", self, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    float curY = 0;
                    for (uint32_t i = 0; i < rows; i++) {
                        uint32_t* indexPath = (uint32_t*)calloc(1, 32);
                        indexPath[0] = (uint32_t)g_hleClasses["NSIndexPath"];
                        indexPath[1] = i; // Store row
                        
                        void* cell = (void*)Stub_objc_msgSend(ds, "tableView:cellForRowAtIndexPath:", self, indexPath, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        if (cell) {
                            Stub_objc_msgSend(self, "addSubview:", cell, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            float x = 0, y = curY, w = (float)g_surfaceWidth, h = 50;
                            uint32_t px, py, pw, ph; memcpy(&px, &x, 4); memcpy(&py, &y, 4); memcpy(&pw, &w, 4); memcpy(&ph, &h, 4);
                            Stub_objc_msgSend(cell, "setFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                            curY += 50;
                            
                            uint32_t* btn = (uint32_t*)Stub_objc_msgSend((void*)g_hleClasses["UIButton"], "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend((void*)btn, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend((void*)btn, "addTarget:action:forControlEvents:", self, (void*)"didSelectFakeBtn:", (void*)1, nullptr, nullptr, nullptr, nullptr, nullptr);
                            g_dictionaries[(void*)btn][(void*)0x1009] = indexPath;
                            g_views[btn].alpha = 0.0f;
                            Stub_objc_msgSend(self, "addSubview:", btn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        }
                    }
                }
                return 0;
            }
            if (strcmp(op, "deselectRowAtIndexPath:animated:") == 0) return 0;
            if (strcmp(op, "didSelectFakeBtn:") == 0) {
                void* delegate = g_dictionaries[self][(void*)0x1007];
                if (delegate) {
                    void* indexPath = g_dictionaries[a1][(void*)0x1009];
                    Stub_objc_msgSend(delegate, "tableView:didSelectRowAtIndexPath:", self, indexPath, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                }
                return 0;
            }
            if (strcmp(op, "dequeueReusableCellWithIdentifier:") == 0) return 0;
        }
        if (clsName == "UITableViewCell") {
            if (strcmp(op, "initWithStyle:reuseIdentifier:") == 0) {
                g_views[self].type = "UITableViewCell";
                g_uiColors[self] = {1.0f, 1.0f, 1.0f, 1.0f}; // White background for iOS cells
                g_views[self].bgColor = g_uiColors[self]; g_views[self].hasBg = true;
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "textLabel") == 0) {
                void* label = g_dictionaries[self][(void*)0x1010];
                if (!label) {
                    label = (void*)Stub_objc_msgSend((void*)g_hleClasses["UILabel"], "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    float x = 15, y = 10, w = (float)g_surfaceWidth - 30, h = 30;
                    uint32_t px, py, pw, ph; memcpy(&px, &x, 4); memcpy(&py, &y, 4); memcpy(&pw, &w, 4); memcpy(&ph, &h, 4);
                    Stub_objc_msgSend(label, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                    g_uiColors[label] = {0.0f, 0.0f, 0.0f, 1.0f}; // Black text
                    g_views[label].textColor = g_uiColors[label]; g_views[label].hasTextCol = true;
                    Stub_objc_msgSend(self, "addSubview:", label, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    g_dictionaries[self][(void*)0x1010] = label;
                }
                return (uint64_t)(uintptr_t)label;
            }
            if (strcmp(op, "contentView") == 0) return (uint64_t)(uintptr_t)self;
            if (strcmp(op, "setAccessoryType:") == 0) return 0;
        }

        if (clsName == "UINavigationController") {
            if (strcmp(op, "initWithRootViewController:") == 0) {
                g_dictionaries[self][(void*)0x1007] = a1; 
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "navigationBar") == 0) {
                static uint32_t* dummyNavBar = nullptr;
                if (!dummyNavBar) { dummyNavBar = (uint32_t*)calloc(1, 32); dummyNavBar[0] = g_hleClasses.count("UINavigationBar") ? (uint32_t)g_hleClasses["UINavigationBar"] : 0xDEADBEEF; }
                return (uint64_t)(uintptr_t)dummyNavBar;
            }
            if (strcmp(op, "setBarStyle:") == 0) return 0;
            if (strcmp(op, "view") == 0) {
                void* containerView = g_dictionaries[self][(void*)0x1011];
                if (!containerView) {
                    uint32_t* uiview_cls = (uint32_t*)ResolveSymbol("OBJC_CLASS_$_UIView");
                    containerView = (void*)Stub_objc_msgSend(uiview_cls, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    float cx = 0, cy = 0, cw = (float)g_surfaceWidth, ch = (float)g_surfaceHeight;
                    uint32_t pcx, pcy, pcw, pch; memcpy(&pcx, &cx, 4); memcpy(&pcy, &cy, 4); memcpy(&pcw, &cw, 4); memcpy(&pch, &ch, 4);
                    Stub_objc_msgSend(containerView, "initWithFrame:", (void*)(uintptr_t)pcx, (void*)(uintptr_t)pcy, (void*)(uintptr_t)pcw, (void*)(uintptr_t)pch, nullptr, nullptr, nullptr, nullptr);
                    g_views[containerView].type = "UIView";
                    g_uiColors[containerView] = {1.0f, 1.0f, 1.0f, 1.0f};
                    g_views[containerView].bgColor = g_uiColors[containerView]; g_views[containerView].hasBg = true;

                    uint32_t* navBar = (uint32_t*)Stub_objc_msgSend(self, "navigationBar", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    float nx = 0, ny = 0, nw = (float)g_surfaceWidth, nh = 44;
                    uint32_t pnx, pny, pnw, pnh; memcpy(&pnx, &nx, 4); memcpy(&pny, &ny, 4); memcpy(&pnw, &nw, 4); memcpy(&pnh, &nh, 4);
                    Stub_objc_msgSend(navBar, "initWithFrame:", (void*)(uintptr_t)pnx, (void*)(uintptr_t)pny, (void*)(uintptr_t)pnw, (void*)(uintptr_t)pnh, nullptr, nullptr, nullptr, nullptr);
                    g_views[navBar].type = "UINavigationBar";
                    g_uiColors[navBar] = {0.85f, 0.85f, 0.85f, 1.0f};
                    g_views[navBar].bgColor = g_uiColors[navBar]; g_views[navBar].hasBg = true;
                    Stub_objc_msgSend(containerView, "addSubview:", navBar, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

                    void* rootVC = g_dictionaries[self][(void*)0x1007];
                    if (rootVC) {
                        void* rootVCView = (void*)Stub_objc_msgSend(rootVC, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        if (rootVCView) {
                            float rx = 0, ry = 44, rw = (float)g_surfaceWidth, rh = (float)g_surfaceHeight - 44;
                            uint32_t prx, pry, prw, prh; memcpy(&prx, &rx, 4); memcpy(&pry, &ry, 4); memcpy(&prw, &rw, 4); memcpy(&prh, &rh, 4);
                            Stub_objc_msgSend(rootVCView, "setFrame:", (void*)(uintptr_t)prx, (void*)(uintptr_t)pry, (void*)(uintptr_t)prw, (void*)(uintptr_t)prh, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend(containerView, "addSubview:", rootVCView, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        }
                    }
                    g_dictionaries[self][(void*)0x1011] = containerView;
                }
                return (uint64_t)(uintptr_t)containerView;
            }
            if (strcmp(op, "popViewControllerAnimated:") == 0) {
                void* containerView = g_dictionaries[self][(void*)0x1011];
                void* topVC = g_dictionaries[self][(void*)0x1012];
                void* rootVC = g_dictionaries[self][(void*)0x1007];
                if (containerView && topVC && topVC != rootVC) {
                    void* topView = (void*)Stub_objc_msgSend(topVC, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    if (topView) Stub_objc_msgSend(topView, "removeFromSuperview", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    
                    if (rootVC) {
                        void* rootView = (void*)Stub_objc_msgSend(rootVC, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        if (rootView) Stub_objc_msgSend(rootView, "setHidden:", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    }
                    g_dictionaries[self][(void*)0x1012] = rootVC;
                }
                return 0;
            }
            if (strcmp(op, "pushViewController:animated:") == 0) {
                void* newVC = a1;
                void* containerView = g_dictionaries[self][(void*)0x1011];
                if (containerView && newVC) {
                    void* newView = (void*)Stub_objc_msgSend(newVC, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    if (newView) {
                        void* currentVC = g_dictionaries[self][(void*)0x1012] ? g_dictionaries[self][(void*)0x1012] : g_dictionaries[self][(void*)0x1007];
                        if (currentVC) {
                            void* oldView = (void*)Stub_objc_msgSend(currentVC, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            if (oldView) Stub_objc_msgSend(oldView, "setHidden:", (void*)1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        }
                        
                        float rx = 0, ry = 44, rw = (float)g_surfaceWidth, rh = (float)g_surfaceHeight - 44;
                        uint32_t prx, pry, prw, prh; memcpy(&prx, &rx, 4); memcpy(&pry, &ry, 4); memcpy(&prw, &rw, 4); memcpy(&prh, &rh, 4);
                        Stub_objc_msgSend(newView, "setFrame:", (void*)(uintptr_t)prx, (void*)(uintptr_t)pry, (void*)(uintptr_t)prw, (void*)(uintptr_t)prh, nullptr, nullptr, nullptr, nullptr);
                        Stub_objc_msgSend(newView, "setHidden:", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        Stub_objc_msgSend(containerView, "addSubview:", newView, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        
                        uint32_t* navBar = (uint32_t*)Stub_objc_msgSend(self, "navigationBar", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        if (navBar) {
                            uint32_t* backBtn = (uint32_t*)Stub_objc_msgSend((void*)g_hleClasses["UIButton"], "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            float bx = 5, by = 7, bw = 60, bh = 30;
                            uint32_t pbx, pby, pbw, pbh; memcpy(&pbx, &bx, 4); memcpy(&pby, &by, 4); memcpy(&pbw, &bw, 4); memcpy(&pbh, &bh, 4);
                            Stub_objc_msgSend(backBtn, "initWithFrame:", (void*)(uintptr_t)pbx, (void*)(uintptr_t)pby, (void*)(uintptr_t)pbw, (void*)(uintptr_t)pbh, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend(backBtn, "setTitle:forState:", CreateNSString("< Back"), (void*)0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            g_uiColors[backBtn] = {0.2f, 0.4f, 0.8f, 1.0f};
                            g_views[backBtn].bgColor = g_uiColors[backBtn]; g_views[backBtn].hasBg = true;
                            g_views[backBtn].cornerRadius = 5.0f;
                            Stub_objc_msgSend(backBtn, "addTarget:action:forControlEvents:", self, (void*)"popViewControllerAnimated:", (void*)1, nullptr, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend(navBar, "addSubview:", backBtn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        }
                        
                        g_dictionaries[self][(void*)0x1012] = newVC;

                        uint32_t vcIsa = ((uint32_t*)newVC)[0];
                        if (vcIsa > 0x1000 && ((uint32_t*)vcIsa)[0] != 0xDEADBEEF) {
                            void* impVDA = FindMethodIMP(vcIsa, "viewDidAppear:");
                            if (impVDA) {
                                typedef void (*VDAFunc)(void*, const char*, uint32_t);
                                ((VDAFunc)impVDA)(newVC, "viewDidAppear:", 1);
                            }
                        }
                    }
                }
                return 0;
            }
            if (strcmp(op, "viewDidAppear:") == 0) {
                void* topVC = g_dictionaries[self][(void*)0x1012] ? g_dictionaries[self][(void*)0x1012] : g_dictionaries[self][(void*)0x1007];
                if (topVC) {
                    uint32_t topIsa = ((uint32_t*)topVC)[0];
                    if (topIsa > 0x1000 && ((uint32_t*)topIsa)[0] == 0xDEADBEEF) {
                        HLEClass* topCls = (HLEClass*)topIsa;
                        if (strcmp(topCls->className, "MPMoviePlayerViewController") == 0) {
                            Stub_objc_msgSend(topVC, "play", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        }
                    } else {
                        void* impVDA = FindMethodIMP(topIsa, "viewDidAppear:");
                        if (impVDA) {
                            typedef void (*VDAFunc)(void*, const char*, uint32_t);
                            ((VDAFunc)impVDA)(topVC, "viewDidAppear:", 1);
                        }
                    }
                }
                return 0;
            }
        }
        if (clsName == "MPMoviePlayerController" || clsName == "MPMoviePlayerViewController") {
            if (strcmp(op, "initWithContentURL:") == 0) {
                uint32_t* urlInst = (uint32_t*)a1;
                std::string path = "Unknown";
                if (urlInst && urlInst[1]) path = GetNSString((void*)urlInst[1]);
                
                if (path != "Unknown" && !path.empty()) {
                    if (path[0] != '/') {
                        path = g_appBundlePath + "/" + path;
                    } else {
                        struct stat buffer;
                        if (stat(path.c_str(), &buffer) != 0) {
                            std::string fileName = path.substr(path.find_last_of('/') + 1);
                            path = g_appBundlePath + "/" + fileName;
                        }
                    }
                }
                
                LogToJava("HLE_DEBUG: Подготовка Video с путем: " + path);
                VideoInitToJava(self, path);
                s_lastMoviePlayer = self;
                
                // Логика асинхронного уведомления перенесена в addObserver для защиты от гонки потоков!
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "moviePlayer") == 0) return (uint64_t)(uintptr_t)self;
            if (strcmp(op, "view") == 0) {
                if (g_viewControllersViews.find(self) == g_viewControllersViews.end()) {
                    uint32_t* uiview_cls = (uint32_t*)ResolveSymbol("OBJC_CLASS_$_UIView");
                    void* view = (void*)Stub_objc_msgSend(uiview_cls, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    g_viewControllersViews[self] = view;
                }
                return (uint64_t)(uintptr_t)g_viewControllersViews[self];
            }
            if (strcmp(op, "play") == 0) { VideoPlayToJava(self); return 0; }
            if (strcmp(op, "stop") == 0) { VideoStopToJava(self); return 0; }
            if (strcmp(op, "setControlStyle:") == 0) return 0;
            if (strcmp(op, "setMovieControlMode:") == 0) return 0;
        }
// --- Foundation HLE Extensions ---
        if (clsName == "NSUserDefaults") {
            if (strcmp(op, "dictionaryRepresentation") == 0) {
                uint32_t* dictInst = (uint32_t*)calloc(1, 32); 
                dictInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary");
                for (auto const& pair : g_userDefaults) {
                    g_dictionariesHLE[dictInst][pair.first] = pair.second;
                }
                return (uint64_t)(uintptr_t)dictInst;
            }
            if (strcmp(op, "objectForKey:") == 0 || strcmp(op, "valueForKey:") == 0) {
                std::string key = GetNSString(a1);
                LogToJava("[PREF-DEBUG] NSUserDefaults objectForKey/valueForKey: " + key);
                return (uint64_t)(uintptr_t)g_userDefaults[key];
            }
            if (strcmp(op, "setObject:forKey:") == 0) {
                std::string key = GetNSString(a2);
                LogToJava("[PREF-DEBUG] NSUserDefaults setObject: <" + GetObjCClassName(a1) + "> forKey: " + key);
                g_userDefaults[key] = a1; return 0;
            }
            if (strcmp(op, "integerForKey:") == 0) {
                void* obj = g_userDefaults[GetNSString(a1)];
                if (!obj) return 0;
                std::string s = GetNSString(obj);
                return s.empty() ? 0 : atoi(s.c_str());
            }
            if (strcmp(op, "boolForKey:") == 0) {
                void* obj = g_userDefaults[GetNSString(a1)];
                if (!obj) return 0;
                std::string s = GetNSString(obj);
                return (s == "1" || s == "true" || s == "YES") ? 1 : 0;
            }
            if (strcmp(op, "setInteger:forKey:") == 0) {
                int val = (int)(uintptr_t)a1;
                g_userDefaults[GetNSString(a2)] = CreateNSString(std::to_string(val));
                return 0;
            }
            if (strcmp(op, "setBool:forKey:") == 0) {
                bool val = (a1 != nullptr);
                g_userDefaults[GetNSString(a2)] = CreateNSString(val ? "1" : "0");
                return 0;
            }
            if (strcmp(op, "setValue:forKey:") == 0) {
                std::string key = GetNSString(a2);
                g_userDefaults[key] = a1; return 0;
            }
            if (strcmp(op, "synchronize") == 0) {
                LogToJava("[PREF-DEBUG] NSUserDefaults synchronize");
                SaveUserDefaults();
                return 1;
            }
        }
        if (clsName == "NSDictionary" || clsName == "NSMutableDictionary") {
            if (strcmp(op, "objectForKey:") == 0 || strcmp(op, "valueForKey:") == 0) {
                return (uint64_t)(uintptr_t)g_dictionariesHLE[self][GetNSString(a1)];
            }
            // АНОМАЛИИ PopCap: Посылка строковых/числовых сообщений словарю из-за type confusion
            if (strcmp(op, "isEqualToString:") == 0) return 0;
            if (strcmp(op, "intValue") == 0 || strcmp(op, "integerValue") == 0) return 0;
            if (strcmp(op, "stringByReplacingOccurrencesOfString:withString:") == 0) return (uint64_t)(uintptr_t)CreateNSString("");
            if (strcmp(op, "setObject:forKey:") == 0) {
                g_dictionariesHLE[self][GetNSString(a2)] = a1; 
                return 0;
            }
            if (strcmp(op, "removeObjectForKey:") == 0) {
                g_dictionariesHLE[self].erase(GetNSString(a1)); 
                return 0;
            }
            if (strcmp(op, "allKeys") == 0) {
                uint32_t* arrInst = (uint32_t*)calloc(1, 32); 
                arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
                for (auto const& pair : g_dictionariesHLE[self]) {
                    g_arrays[arrInst].push_back(CreateNSString(pair.first));
                }
                return (uint64_t)(uintptr_t)arrInst;
            }
            if (strcmp(op, "count") == 0) {
                return g_dictionariesHLE[self].size();
            }
            if (strcmp(op, "objectAtIndex:") == 0) {
                // Аномалия: игра вызывает метод массива у словаря.
                // Возвращаем N-ое значение, чтобы избежать краша.
                size_t idx = (size_t)(uintptr_t)a1;
                if (idx < g_dictionariesHLE[self].size()) {
                    auto it = g_dictionariesHLE[self].begin();
                    std::advance(it, idx);
                    return (uint64_t)(uintptr_t)it->second;
                }
                return 0;
            }
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "countByEnumeratingWithState:objects:count:") == 0) {
            NSFastEnumerationState* state = (NSFastEnumerationState*)a1;
            void** stackbuf = (void**)a2;
            unsigned long reqCount = (unsigned long)(uintptr_t)a3;
            
            if (state->state == 0) {
                static unsigned long mut = 0;
                state->mutationsPtr = &mut;
            }
            
            auto& vec = g_arrays[self];
            unsigned long startIdx = state->state;
            unsigned long total = vec.size();
            
            if (startIdx >= total) return 0;
            
            unsigned long returnCount = total - startIdx;
            if (returnCount > reqCount) returnCount = reqCount;
            
            state->itemsPtr = stackbuf;
            for (unsigned long i = 0; i < returnCount; i++) {
                stackbuf[i] = vec[startIdx + i];
            }
            state->state += returnCount;
            
            return returnCount;
        }
        if ((clsName == "NSDictionary" || clsName == "NSMutableDictionary") && strcmp(op, "countByEnumeratingWithState:objects:count:") == 0) {
            NSFastEnumerationState* state = (NSFastEnumerationState*)a1;
            void** stackbuf = (void**)a2;
            unsigned long reqCount = (unsigned long)(uintptr_t)a3;
            
            if (state->state == 0) {
                static unsigned long mut = 0;
                state->mutationsPtr = &mut;
            }
            
            auto& dict = g_dictionariesHLE[self];
            unsigned long startIdx = state->state;
            unsigned long total = dict.size();
            
            if (startIdx >= total) return 0;
            
            unsigned long returnCount = total - startIdx;
            if (returnCount > reqCount) returnCount = reqCount;
            
            state->itemsPtr = stackbuf;
            auto it = dict.begin();
            std::advance(it, startIdx);
            for (unsigned long i = 0; i < returnCount; i++) {
                stackbuf[i] = CreateNSString(it->first);
                ++it;
            }
            state->state += returnCount;
            
            return returnCount;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "objectEnumerator") == 0) {
            uint32_t* enumInst = (uint32_t*)calloc(1, 32);
            enumInst[0] = (uint32_t)g_hleClasses["NSEnumerator"];
            enumInst[1] = (uint32_t)self; // Указатель на массив
            enumInst[2] = 0; // Текущий индекс
            return (uint64_t)(uintptr_t)enumInst;
        }
        if (clsName == "NSEnumerator" && strcmp(op, "nextObject") == 0) {
            uint32_t* enumInst = (uint32_t*)self;
            void* arrPtr = (void*)enumInst[1];
            uint32_t idx = enumInst[2];
            if (arrPtr && g_arrays.count(arrPtr) && idx < g_arrays[arrPtr].size()) {
                enumInst[2] = idx + 1;
                return (uint64_t)(uintptr_t)g_arrays[arrPtr][idx];
            }
            return 0;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "count") == 0) {
            return g_arrays[self].size();
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "objectAtIndex:") == 0) {
            size_t idx = (size_t)(uintptr_t)a1;
            if (idx < g_arrays[self].size()) return (uint64_t)(uintptr_t)g_arrays[self][idx];
            return 0;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "lastObject") == 0) {
            if (!g_arrays[self].empty()) return (uint64_t)(uintptr_t)g_arrays[self].back();
            return 0;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "lowercaseString") == 0) {
            return (uint64_t)(uintptr_t)CreateNSString("");
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "ApplifierStringWithEnum:") == 0) {
            return (uint64_t)(uintptr_t)CreateNSString("");
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "containsObject:") == 0) {
            std::string target = GetNSString(a1);
            for (void* item : g_arrays[self]) {
                if (GetNSString(item) == target) return 1;
            }
            return 0;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "addObject:") == 0) {
            if (a1) g_arrays[self].push_back(a1);
            return 0;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "removeAllObjects") == 0) {
            g_arrays[self].clear();
            return 0;
        }
        if ((clsName == "NSArray" || clsName == "NSMutableArray") && strcmp(op, "removeObjectAtIndex:") == 0) {
            size_t idx = (size_t)(uintptr_t)a1;
            if (idx < g_arrays[self].size()) {
                g_arrays[self].erase(g_arrays[self].begin() + idx);
            }
            return 0;
        }
        if (clsName == "NSURLRequest" || clsName == "NSMutableURLRequest") {
            if (strcmp(op, "setHTTPMethod:") == 0 || strcmp(op, "setHTTPBody:") == 0) return 0;
            if (strcmp(op, "setValue:forHTTPHeaderField:") == 0) return 0;
        }
        if (clsName == "NSURLConnection") {
            if (strcmp(op, "initWithRequest:delegate:") == 0) {
                LogToJava("HLE: NSURLConnection initWithRequest:delegate: эмулируем ошибку сети...");
                void* delegate = a2;
                if (delegate) {
                    struct ConnArgs { void* target; void* conn; };
                    ConnArgs* cargs = new ConnArgs{delegate, self};
                    pthread_t t;
                    pthread_create(&t, nullptr, [](void* p) -> void* {
                        ConnArgs* args = (ConnArgs*)p;
                        usleep(100000); // Ждем 100ms
                        // Дергаем делегат игры, сообщая об ошибке (error = nil прокатит)
                        Stub_objc_msgSend(args->target, "connection:didFailWithError:", args->conn, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                        delete args;
                        return nullptr;
                    }, cargs);
                }
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "start") == 0 || strcmp(op, "cancel") == 0) return 0;
        }
        if (clsName == "NSBundle") {
            if (strcmp(op, "pathForResource:ofType:") == 0) {
                std::string res = GetNSString(a1); std::string ext = GetNSString(a2);
                std::string fullPath = g_appBundlePath + "/" + res + (ext.empty() ? "" : "." + ext);
                struct stat buffer;
                if (stat(fullPath.c_str(), &buffer) == 0) {
                    return (uint64_t)(uintptr_t)CreateNSString(fullPath);
                }
                return 0;
            }
        }
        if (clsName == "NSData") {
            if (strcmp(op, "initWithContentsOfFile:") == 0) {
                std::string path = GetNSString(a1);
                std::ifstream in(path, std::ios::binary | std::ios::ate);
                if (in.is_open()) {
                    std::streamsize size = in.tellg();
                    in.seekg(0, std::ios::beg);
                    uint8_t* binData = (uint8_t*)malloc(size);
                    in.read((char*)binData, size);
                    self_ptr[1] = (uint32_t)binData;
                    self_ptr[2] = (uint32_t)size;
                }
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "writeToFile:atomically:") == 0 || strcmp(op, "writeToFile:options:error:") == 0) {
                std::string path = GetNSString(a1);
                void* bytes = (void*)self_ptr[1];
                uint32_t size = self_ptr[2];
                if (bytes && size > 0 && !path.empty()) {
                    FILE* f = fopen(path.c_str(), "wb");
                    if (f) {
                        fwrite(bytes, 1, size, f);
                        fclose(f);
                        return 1;
                    }
                }
                return 0;
            }
            if (strcmp(op, "length") == 0) return self_ptr[2];
            if (strcmp(op, "bytes") == 0) return self_ptr[1];
        }
        if (clsName == "NSString") {
            if (strcmp(op, "initWithUTF8String:") == 0) {
                const char* cstr = a1 ? (const char*)a1 : "";
                self_ptr[2] = (uint32_t)strdup(cstr);
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "initWithCString:encoding:") == 0) {
                const char* cstr = a1 ? (const char*)a1 : "";
                self_ptr[2] = (uint32_t)strdup(cstr);
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "initWithString:") == 0) {
                std::string s = GetNSString(a1);
                self_ptr[2] = (uint32_t)strdup(s.c_str());
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "initWithFormat:") == 0) {
                std::string fmt = GetNSString(a1);
                void* args[] = {a2, a3, a4, a5};
                int argIdx = 0; std::string res = "";
                for (size_t i = 0; i < fmt.length(); i++) {
                    if (fmt[i] == '%' && i + 1 < fmt.length()) {
                        char type = fmt[i+1];
                        void* arg = argIdx < 4 ? args[argIdx++] : nullptr;
                        if (type == '@') res += GetNSString(arg);
                        else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
                        else if (type == 's') res += arg ? (const char*)arg : "null";
                        else { res += "%"; res += type; }
                        i++;
                    } else res += fmt[i];
                }
                self_ptr[2] = (uint32_t)strdup(res.c_str());
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "getCString:maxLength:encoding:") == 0) {
                char* buf = (char*)a1;
                size_t maxLen = (size_t)(uintptr_t)a2;
                std::string s = GetNSString(self);
                if (buf && maxLen > 0) {
                    strncpy(buf, s.c_str(), maxLen - 1);
                    buf[maxLen - 1] = '\0';
                    return 1; // YES
                }
                return 0; // NO
            }
            if (strcmp(op, "substringToIndex:") == 0) {
                std::string s = GetNSString(self);
                size_t idx = (size_t)(uintptr_t)a1;
                if (idx < s.length()) s = s.substr(0, idx);
                return (uint64_t)(uintptr_t)CreateNSString(s);
            }
            if (strcmp(op, "sizeWithFont:") == 0 || strcmp(op, "sizeWithFont:constrainedToSize:") == 0 || strcmp(op, "sizeWithFont:constrainedToSize:lineBreakMode:") == 0) {
                float w = 300.0f, h = 30.0f;
                uint32_t iw, ih; memcpy(&iw, &w, 4); memcpy(&ih, &h, 4);
                return ((uint64_t)ih << 32) | iw;
            }
            if (strcmp(op, "length") == 0) return GetNSString(self).length();
            if (strcmp(op, "UTF8String") == 0) {
            const char* cstr = (const char*)self_ptr[2];
            if (cstr && (strstr(cstr, "pngConf") || strstr(cstr, "jungle"))) {
                LogToJava(std::string("OBJC-DEBUG: [UTF8String] Запрошен C-string: [") + cstr + "]");
            }
            return (uint64_t)self_ptr[2];
        }
        if (strcmp(op, "fileSystemRepresentation") == 0) {
            const char* cstr = (const char*)self_ptr[2];
            if (cstr && (strstr(cstr, "pngConf") || strstr(cstr, "jungle"))) {
                LogToJava(std::string("OBJC-DEBUG: [fileSystemRepresentation] Запрошен C-string: [") + cstr + "]");
            }
            return (uint64_t)self_ptr[2];
        }
                    if (strcmp(op, "stringByAppendingPathComponent:") == 0) {
            std::string base = GetNSString(self);
            std::string comp = GetNSString(a1);
            if (!base.empty() && base.back() != '/') base += "/";
            std::string result = base + comp;
            if (result.find("pngConf") != std::string::npos || result.find("jungle") != std::string::npos) {
                LogToJava("OBJC-DEBUG: [stringByAppendingPathComponent] Собрана подозрительная строка!");
                LogToJava("OBJC-DEBUG: base: [" + base + "], len=" + std::to_string(base.length()) + " hex: " + DumpHexToString(base.c_str(), base.length()+4));
                LogToJava("OBJC-DEBUG: comp: [" + comp + "], len=" + std::to_string(comp.length()) + " hex: " + DumpHexToString(comp.c_str(), comp.length()+4));
                LogToJava("OBJC-DEBUG: result: [" + result + "]");
            }
            return (uint64_t)(uintptr_t)CreateNSString(result);
        }
        if (strcmp(op, "stringByAppendingString:") == 0) {
            std::string base = GetNSString(self);
            std::string comp = GetNSString(a1);
            std::string result = base + comp;
            if (result.find("pngConf") != std::string::npos || result.find("jungle") != std::string::npos) {
                LogToJava("OBJC-DEBUG: [stringByAppendingString] Собрана подозрительная строка!");
                LogToJava("OBJC-DEBUG: base: [" + base + "], len=" + std::to_string(base.length()) + " hex: " + DumpHexToString(base.c_str(), base.length()+4));
                LogToJava("OBJC-DEBUG: comp: [" + comp + "], len=" + std::to_string(comp.length()) + " hex: " + DumpHexToString(comp.c_str(), comp.length()+4));
                LogToJava("OBJC-DEBUG: result: [" + result + "]");
            }
            return (uint64_t)(uintptr_t)CreateNSString(result);
        }

            if (strcmp(op, "stringWithFormat:") == 0) {
                std::string fmt = GetNSString(a1);
                void* args[] = {a2, a3, a4, a5};
                int argIdx = 0; std::string res = "";
                for (size_t i = 0; i < fmt.length(); i++) {
                    if (fmt[i] == '%' && i + 1 < fmt.length()) {
                        char type = fmt[i+1];
                        void* arg = argIdx < 4 ? args[argIdx++] : nullptr;
                        if (type == '@') res += GetNSString(arg);
                        else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
                        else if (type == 's') res += arg ? (const char*)arg : "null";
                        else { res += "%"; res += type; }
                        i++;
                    } else res += fmt[i];
                }
                return (uint64_t)(uintptr_t)CreateNSString(res);
            }
            if (strcmp(op, "stringByReplacingOccurrencesOfString:withString:") == 0) {
                std::string s = GetNSString(self);
                std::string target = GetNSString(a1);
                std::string replacement = GetNSString(a2);
                if (!target.empty()) {
                    size_t pos = 0;
                    while ((pos = s.find(target, pos)) != std::string::npos) {
                        s.replace(pos, target.length(), replacement);
                        pos += replacement.length();
                    }
                }
                return (uint64_t)(uintptr_t)CreateNSString(s);
            }
            if (strcmp(op, "stringByStandardizingPath") == 0) {
                return (uint64_t)(uintptr_t)self;
            }
            if (strcmp(op, "appendFormat:") == 0) {
                std::string base = GetNSString(self);
                std::string fmt = GetNSString(a1);
                void* args[] = {a2, a3, a4, a5};
                int argIdx = 0; std::string res = "";
                for (size_t i = 0; i < fmt.length(); i++) {
                    if (fmt[i] == '%' && i + 1 < fmt.length()) {
                        char type = fmt[i+1];
                        void* arg = argIdx < 4 ? args[argIdx++] : nullptr;
                        if (type == '@') res += GetNSString(arg);
                        else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
                        else if (type == 's') res += arg ? (const char*)arg : "null";
                        else { res += "%"; res += type; }
                        i++;
                    } else res += fmt[i];
                }
                self_ptr[2] = (uint32_t)strdup((base + res).c_str());
                return 0;
            }
            if (strcmp(op, "stringByAppendingFormat:") == 0) {
                std::string base = GetNSString(self);
                std::string fmt = GetNSString(a1);
                void* args[] = {a2, a3, a4, a5};
                int argIdx = 0; std::string res = "";
                for (size_t i = 0; i < fmt.length(); i++) {
                    if (fmt[i] == '%' && i + 1 < fmt.length()) {
                        char type = fmt[i+1];
                        void* arg = argIdx < 4 ? args[argIdx++] : nullptr;
                        if (type == '@') res += GetNSString(arg);
                        else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
                        else if (type == 's') res += arg ? (const char*)arg : "null";
                        else { res += "%"; res += type; }
                        i++;
                    } else res += fmt[i];
                }
                return (uint64_t)(uintptr_t)CreateNSString(base + res);
            }
                        if (strcmp(op, "writeToFile:atomically:encoding:error:") == 0 || strcmp(op, "writeToFile:atomically:") == 0) {
                std::string content = GetNSString(self);
                std::string path = GetNSString(a1);
                std::ofstream f(path); if(f) { f << content; f.close(); return 1; }
                return 0;
            }

            if (strcmp(op, "isEqualToString:") == 0) {
                return GetNSString(self) == GetNSString(a1) ? 1 : 0;
            }
            if (strcmp(op, "compare:options:") == 0 || strcmp(op, "compare:") == 0) {
                std::string s1 = GetNSString(self);
                std::string s2 = GetNSString(a1);
                if (s1 < s2) return (uint64_t)-1;
                if (s1 > s2) return 1;
                return 0;
            }
            if (strcmp(op, "cStringUsingEncoding:") == 0) {
                const char* cstr = (const char*)self_ptr[2];
                if (cstr && (strstr(cstr, "pngConf") || strstr(cstr, "jungle"))) {
                    LogToJava(std::string("OBJC-DEBUG: [cStringUsingEncoding:] Запрошен C-string: [") + cstr + "]");
                }
                return (uint64_t)self_ptr[2];
            }
            if (strcmp(op, "dataUsingEncoding:") == 0) {
                std::string s = GetNSString(self);
                uint32_t* nsData = (uint32_t*)calloc(1, 32);
                nsData[0] = g_hleClasses.count("NSData") ? (uint32_t)g_hleClasses["NSData"] : 0xDEADBEEF;
                if (!s.empty()) {
                    uint8_t* binData = (uint8_t*)malloc(s.length());
                    memcpy(binData, s.c_str(), s.length());
                    nsData[1] = (uint32_t)binData;
                    nsData[2] = (uint32_t)s.length();
                }
                return (uint64_t)(uintptr_t)nsData;
            }
            if (strcmp(op, "hasPrefix:") == 0) {
                std::string s = GetNSString(self);
                std::string prefix = GetNSString(a1);
                if (prefix.empty()) return 1;
                return (s.find(prefix) == 0) ? 1 : 0;
            }
            if (strcmp(op, "hasSuffix:") == 0) {
                std::string s = GetNSString(self);
                std::string suffix = GetNSString(a1);
                if (suffix.empty() || s.length() < suffix.length()) return 0;
                return (s.compare(s.length() - suffix.length(), suffix.length(), suffix) == 0) ? 1 : 0;
            }
            if (strcmp(op, "uppercaseString") == 0) {
                std::string s = GetNSString(self);
                for (auto& c : s) c = toupper(c);
                return (uint64_t)(uintptr_t)CreateNSString(s);
            }
            if (strcmp(op, "lowercaseString") == 0) {
                std::string s = GetNSString(self);
                for (auto& c : s) c = tolower(c);
                return (uint64_t)(uintptr_t)CreateNSString(s);
            }
            if (strcmp(op, "stringFromMD5") == 0) {
                return (uint64_t)(uintptr_t)CreateNSString("d41d8cd98f00b204e9800998ecf8427e");
            }
            if (strcmp(op, "intValue") == 0 || strcmp(op, "integerValue") == 0) {
                std::string s = GetNSString(self);
                LogToJava("OBJC-DEBUG: [intValue/integerValue] Парсинг строки: [" + s + "]");
                return s.empty() ? 0 : atoi(s.c_str());
            }
            if (strcmp(op, "floatValue") == 0) {
                std::string s = GetNSString(self);
                LogToJava("OBJC-DEBUG: [floatValue] Парсинг строки: [" + s + "]");
                float f = s.empty() ? 0.0f : (float)atof(s.c_str());
                g_fpu_ret[0] = f; g_fpu_ret_flag = 1;
                uint32_t ret; memcpy(&ret, &f, 4); return ret;
            }
            if (strcmp(op, "componentsSeparatedByString:") == 0) {
                std::string s = GetNSString(self);
                std::string delim = GetNSString(a1);
                uint32_t* arrInst = (uint32_t*)calloc(1, 32); arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
                size_t pos = 0;
                if (!delim.empty()) {
                    while ((pos = s.find(delim)) != std::string::npos) {
                        g_arrays[arrInst].push_back(CreateNSString(s.substr(0, pos)));
                        s.erase(0, pos + delim.length());
                    }
                }
                g_arrays[arrInst].push_back(CreateNSString(s));
                return (uint64_t)(uintptr_t)arrInst;
            }
        }
        if (clsName == "NSNumber") {
            if (strcmp(op, "intValue") == 0 || strcmp(op, "integerValue") == 0) {
                uint32_t type = self_ptr[2];
                if (type == 9 || type == 3) return self_ptr[1];
                if (type == 12) { float f; memcpy(&f, &self_ptr[1], 4); return (int)f; }
                if (type == 13) { double d; memcpy(&d, &self_ptr[1], 8); return (int)d; }
                return self_ptr[1];
            }
        }
            if (clsName == "NSFileManager") {
        if (strcmp(op, "contentsOfDirectoryAtPath:error:") == 0 || strcmp(op, "directoryContentsAtPath:") == 0) {
            std::string path = GetNSString(a1);
            uint32_t* arrInst = (uint32_t*)calloc(1, 32);
            arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
                DIR* dir = opendir(path.c_str());
                if (dir) {
                    struct dirent* ent;
                    while ((ent = readdir(dir)) != nullptr) {
                        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                            g_arrays[arrInst].push_back(CreateNSString(ent->d_name));
                        }
                    }
                    closedir(dir);
                }
                return (uint64_t)(uintptr_t)arrInst;
            }
            if (strcmp(op, "fileExistsAtPath:isDirectory:") == 0) {
                std::string path = GetNSString(a1);
                struct stat info;
                int exists = (stat(path.c_str(), &info) == 0);
                if (a2) {
                    int8_t* isDir = (int8_t*)a2; 
                    *isDir = (info.st_mode & S_IFDIR) ? 1 : 0;
                }
                return exists;
            }
            if (strcmp(op, "fileExistsAtPath:") == 0) {
                std::string path = GetNSString(a1);
                struct stat info;
                return (stat(path.c_str(), &info) == 0) ? 1 : 0;
            }
            if (strcmp(op, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:") == 0) {
                std::string path = GetNSString(a1);
                std::string cmd = "mkdir -p '" + path + "'";
                system(cmd.c_str());
                return 1;
            }
            if (strcmp(op, "URLForUbiquityContainerIdentifier:") == 0) {
                return 0; // Return nil for iCloud support
            }
        }

        // --- Original HLE Handlers ---
        if (clsName == "CADisplayLink" && strcmp(op, "addToRunLoop:forMode:") == 0) { 
            LogToJava("HLE_DEBUG: [CADisplayLink addToRunLoop:forMode:] - ИГРА ЗАПУСТИЛА РЕНДЕР ЛУП!");
            g_renderingStarted = true; 
            return (uint64_t)(uintptr_t)self; 
        }
        if (clsName == "CADisplayLink" && strcmp(op, "duration") == 0) { 
            double d = 0.01666666666; 
            memcpy(g_fpu_ret, &d, 8); g_fpu_ret_flag = 1;
            uint64_t ret; memcpy(&ret, &d, sizeof(d)); return ret; 
        }
        if (clsName == "CADisplayLink" && strcmp(op, "setFrameInterval:") == 0) {
            g_dictionaries[self][(void*)0x2001] = a1; 
            return 0; 
        }
        if (clsName == "CADisplayLink" && strcmp(op, "frameInterval") == 0) {
            void* val = g_dictionaries[self][(void*)0x2001];
            return val ? (uint64_t)(uintptr_t)val : 1;
        }
        
        // ВМЕШАТЕЛЬСТВО HLE: Рендерим наш UI поверх кадра игры перед SwapBuffers
        if (clsName == "EAGLContext" && strcmp(op, "presentRenderbuffer:") == 0) { 
            bool isSpamOn = (g_spamFiltersMask & (1 << 5)) != 0;
            static int pr_cnt = 0; pr_cnt++;
            if (!isSpamOn || pr_cnt <= 30 || pr_cnt % 120 == 0) {
                SyncLog("[MEGA-DEBUG] EAGLContext presentRenderbuffer: called");
                DumpGLState("BEFORE RenderHLEUI inside presentRenderbuffer");
            }
            RenderHLEUI(); 
            if (!isSpamOn || pr_cnt <= 30 || pr_cnt % 120 == 0) {
                DumpGLState("AFTER RenderHLEUI inside presentRenderbuffer");
            }
            MegaDebug_eglSwapBuffers(g_eglDisplay, g_eglSurface); 
            return 1; 
        }

        if (clsName == "EAGLContext" && strcmp(op, "renderbufferStorage:fromDrawable:") == 0) {
            uint32_t lr = (uint32_t)__builtin_return_address(0);
            LogToJava("[SIZE-TRACE] renderbufferStorage:fromDrawable: вызван. Caller: " + GetModuleInfoForAddress(lr));
            return 1;
        }
        
        if (strcmp(op, "layer") == 0) return (uint64_t)(uintptr_t)self;
        if (strcmp(op, "setCornerRadius:") == 0) return 0;

        if (strcmp(op, "setObject:forKey:") == 0) { void* keyPtr = GetNSValuePtr(a2); g_dictionaries[self][keyPtr] = a1; return 0; }
        if (strcmp(op, "removeObjectForKey:") == 0) { void* keyPtr = GetNSValuePtr(a1); g_dictionaries[self].erase(keyPtr); return 0; }
        if (strcmp(op, "objectForKey:") == 0) { void* keyPtr = GetNSValuePtr(a1); return (uint64_t)(uintptr_t)g_dictionaries[self][keyPtr]; }

        if ((strcmp(op, "locationInView:") == 0 || strcmp(op, "previousLocationInView:") == 0) && clsName == "FakeUITouch") {
            FakeUITouch* t = (FakeUITouch*)self;
            float rx = t->x, ry = t->y;
            
            if (g_gameViewportH > g_gameViewportW && g_surfaceWidth > g_surfaceHeight) {
                rx = (float)g_surfaceHeight - t->y;
                ry = t->x;
            }
            
            // Если запрашивают координаты относительно конкретного View, а не окна (a1 != nil)
            if (a1 && g_views.count(a1)) {
                // В SMB2 обычно всё на одном уровне, но для порядка вычтем смещение View
                // rx -= g_views[a1].frame[0]; ry -= g_views[a1].frame[1];
            }
            uint32_t bx, by; memcpy(&bx, &rx, 4); memcpy(&by, &ry, 4);
            return ((uint64_t)by << 32) | bx; // r0 = x, r1 = y
        }
        if (strcmp(op, "countByEnumeratingWithState:objects:count:") == 0 && clsName == "FakeNSSet") {
            NSFastEnumerationState* state = (NSFastEnumerationState*)a1; void** stackbuf = (void**)a2;
            if (state->state == 0) {
                static unsigned long mut = 0; state->mutationsPtr = &mut; state->itemsPtr = stackbuf; FakeNSSet* set = (FakeNSSet*)self;
                int c = 0; for (void* t : set->touches) stackbuf[c++] = t; state->state = 1; return c;
            } return 0;
        }
        if (clsName == "FakeNSSet" && strcmp(op, "count") == 0) {
            FakeNSSet* set = (FakeNSSet*)self;
            return set->touches.size();
        }
        if (clsName == "FakeNSSet" && strcmp(op, "allObjects") == 0) {
            FakeNSSet* set = (FakeNSSet*)self;
            uint32_t* arrInst = (uint32_t*)calloc(1, 32); 
            arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
            for (void* t : set->touches) g_arrays[arrInst].push_back(t);
            return (uint64_t)(uintptr_t)arrInst;
        }
        if (clsName == "FakeNSSet" && strcmp(op, "anyObject") == 0) {
            FakeNSSet* set = (FakeNSSet*)self;
            if (!set->touches.empty()) return (uint64_t)(uintptr_t)set->touches[0];
            return 0;
        }
        if (clsName == "FakeUITouch" && strcmp(op, "previousLocationInView:") == 0) {
            FakeUITouch* t = (FakeUITouch*)self; 
            float rx = t->x, ry = t->y;
            if (g_gameViewportH > g_gameViewportW && g_surfaceWidth > g_surfaceHeight) {
                rx = (float)g_surfaceHeight - t->y;
                ry = t->x;
            }
            uint32_t bx, by; memcpy(&bx, &rx, 4); memcpy(&by, &ry, 4); return ((uint64_t)by << 32) | bx;
        }
        if (clsName == "FakeUITouch" && strcmp(op, "view") == 0) {
            FakeUITouch* t = (FakeUITouch*)self; return (uint64_t)(uintptr_t)t->view;
        }
        if (clsName == "FakeUITouch" && strcmp(op, "phase") == 0) {
            return 0; // UITouchPhaseBegan
        }
        if (clsName == "FakeUITouch" && strcmp(op, "tapCount") == 0) {
            return 1;
        }
        if (clsName == "FakeUITouch" && strcmp(op, "timestamp") == 0) {
            double ts = (double)Stub_mach_absolute_time() / 1000000000.0;
            memcpy(g_fpu_ret, &ts, 8); g_fpu_ret_flag = 1;
            uint64_t ret; memcpy(&ret, &ts, 8);
            return ret;
        }
        if (clsName == "FakeUIAcceleration") {
            FakeUIAcceleration* acc = (FakeUIAcceleration*)self;
            if (strcmp(op, "timestamp") == 0) { 
                double d = acc->timestamp;
                memcpy(g_fpu_ret, &d, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &d, 8); return ret; 
            }
            if (strcmp(op, "x") == 0) { 
                double d = acc->x;
                memcpy(g_fpu_ret, &d, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &d, 8); return ret; 
            }
            if (strcmp(op, "y") == 0) { 
                double d = acc->y;
                memcpy(g_fpu_ret, &d, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &d, 8); return ret; 
            }
            if (strcmp(op, "z") == 0) { 
                double d = acc->z;
                memcpy(g_fpu_ret, &d, 8); g_fpu_ret_flag = 1;
                uint64_t ret; memcpy(&ret, &d, 8); return ret; 
            }
        }
        if (clsName == "NSRunLoop" && strcmp(op, "addTimer:forMode:") == 0) {
            LogToJava("HLE: [NSRunLoop addTimer:forMode:] (таймер уже запущен в фоне)");
            return 0;
        }
        if (clsName == "NSOperationQueue" && strcmp(op, "setMaxConcurrentOperationCount:") == 0) return 0;
        if (clsName == "UIWebView") {
            if (strcmp(op, "setDelegate:") == 0) { g_dictionaries[self][(void*)0x1013] = a1; return 0; }
            if (strcmp(op, "loadRequest:") == 0) return 0;
        }
        if (clsName == "NSDate" && strcmp(op, "timeIntervalSince1970") == 0) {
            double t = (double)Stub_mach_absolute_time() / 1000000000.0;
            memcpy(g_fpu_ret, &t, 8); g_fpu_ret_flag = 1;
            uint64_t ret; memcpy(&ret, &t, 8); return ret;
        }

        if (strncmp(op, "init", 4) == 0) return (uint64_t)(uintptr_t)self; 
        if (strcmp(op, "drain") == 0) return 0;
        if (strcmp(op, "touchesBegan:withEvent:") == 0 || strcmp(op, "touchesMoved:withEvent:") == 0 ||
            strcmp(op, "touchesEnded:withEvent:") == 0 || strcmp(op, "touchesCancelled:withEvent:") == 0) {
            return 0;
        }
        
        char ptrStr[32]; snprintf(ptrStr, sizeof(ptrStr), "0x%lx", (unsigned long)(uintptr_t)self);
        LogToJava(std::string("OBJC-TODO: Unimplemented HLE Instance Method -[(") + clsName + "*) " + ptrStr + " " + std::string(op) + "]");
        return (uint64_t)(uintptr_t)self;
    }
    // Ветка 3: Нативные классы (VC и App)
    else {
        if (strcmp(op, "alloc") == 0) {
            uint32_t* cls = (uint32_t*)self; uint32_t data_ptr = cls[4] & ~3; uint32_t instance_size = 32; 
            if (data_ptr > 0x1000) { uint32_t parsed_size = ((uint32_t*)data_ptr)[2]; if (parsed_size > 0 && parsed_size < 100000) instance_size = parsed_size; }
            void* instance = calloc(1, instance_size); ((uint32_t*)instance)[0] = (uint32_t)self; 
            
            std::string cName = GetObjCClassName(self);
            LogToJava("HLE_DEBUG: [ALLOC] Создан нативный объект класса: " + cName + " (size: " + std::to_string(instance_size) + ")");
            
            if (cName.find("Button") != std::string::npos) g_views[instance].type = "UIButton";
            else if (cName.find("Label") != std::string::npos) g_views[instance].type = "UILabel";
            else if (cName.find("View") != std::string::npos) g_views[instance].type = "UIView";
            
            return (uint64_t)(uintptr_t)instance;
        }
        
        if (strcmp(op, "view") == 0) {
            if (g_viewControllersViews.find(self) == g_viewControllersViews.end()) {
                LogToJava("HLE: Calling loadView for " + cName);
                void* imp = FindMethodIMP(isa, "loadView");
                if (imp) {
                    typedef void (*LoadViewFunc)(void*, const char*);
                    ((LoadViewFunc)imp)(self, "loadView");
                } else {
                    std::string baseSys = GetBaseSystemClassName(isa);
                    void* view = nullptr;
                    float x = 0, y = 0, w = (float)g_surfaceWidth, h = (float)g_surfaceHeight;
                    uint32_t px, py, pw, ph; memcpy(&px, &x, 4); memcpy(&py, &y, 4); memcpy(&pw, &w, 4); memcpy(&ph, &h, 4);
                    
                    if (baseSys == "UITableViewController") {
                        LogToJava("HLE: Auto-creating UITableView for UITableViewController");
                        uint32_t* tv_cls = (uint32_t*)g_hleClasses["UITableView"];
                        if (tv_cls) {
                            view = (void*)Stub_objc_msgSend(tv_cls, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            view = (void*)Stub_objc_msgSend(view, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend(view, "setDelegate:", self, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend(view, "setDataSource:", self, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            Stub_objc_msgSend(view, "reloadData", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            g_views[view].type = "UIView";
                            g_uiColors[view] = {0.9f, 0.9f, 0.9f, 1.0f}; // iOS group table background
                            g_views[view].bgColor = g_uiColors[view]; g_views[view].hasBg = true;
                        }
                    }
                    if (!view) {
                        uint32_t viewClassAddr = 0;
                        for (auto const& pair : g_appSymbols) {
                            if (pair.first.find("_OBJC_CLASS_$_EAGLView") == 0) {
                                viewClassAddr = pair.second; break;
                            }
                        }
                        if (viewClassAddr) {
                            view = (void*)Stub_objc_msgSend((void*)viewClassAddr, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            if (FindMethodIMP(viewClassAddr, "initWithCoder:")) {
                                uint32_t* dummyCoder = (uint32_t*)calloc(1, 32);
                                dummyCoder[0] = g_hleClasses.count("NSCoder") ? (uint32_t)g_hleClasses["NSCoder"] : 0xDEADBEEF;
                                view = (void*)Stub_objc_msgSend(view, "initWithCoder:", dummyCoder, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                                g_views[view].frame = {0.0f, 0.0f, (float)g_surfaceWidth, (float)g_surfaceHeight};
                                Stub_objc_msgSend(view, "setFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                            } else {
                                view = (void*)Stub_objc_msgSend(view, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                            }
                            if (FindMethodIMP(viewClassAddr, "awakeFromNib")) {
                                LogToJava("HLE: Calling awakeFromNib for EAGLView");
                                Stub_objc_msgSend(view, "awakeFromNib", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            }
                        } else {
                            uint32_t* uiview_cls = (uint32_t*)ResolveSymbol("OBJC_CLASS_$_UIView");
                            view = (void*)Stub_objc_msgSend(uiview_cls, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            view = (void*)Stub_objc_msgSend(view, "initWithFrame:", (void*)(uintptr_t)px, (void*)(uintptr_t)py, (void*)(uintptr_t)pw, (void*)(uintptr_t)ph, nullptr, nullptr, nullptr, nullptr);
                        }
                        g_views[view].type = "UIView";
                        g_uiColors[view] = {0.0f, 0.0f, 0.0f, 1.0f};
                        g_views[view].bgColor = g_uiColors[view]; g_views[view].hasBg = true;
                    }
                    g_viewControllersViews[self] = view;
                }
                LogToJava("HLE: Calling viewDidLoad for " + cName);
                void* vdl_imp = FindMethodIMP(isa, "viewDidLoad");
                if (vdl_imp) {
                    typedef void (*ViewDidLoadFunc)(void*, const char*);
                    ((ViewDidLoadFunc)vdl_imp)(self, "viewDidLoad");
                }
            }
            return (uint64_t)(uintptr_t)g_viewControllersViews[self];
        }

        if (strcmp(op, "setView:") == 0) {
            g_viewControllersViews[self] = a1; if (cName == "MainViewController") g_mainView = a1; return 0;
        }
                if (strcmp(op, "presentModalViewController:animated:") == 0) {
            void* modalVC = a1;
            g_presentedView = (void*)Stub_objc_msgSend(modalVC, "view", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            LogToJava("HLE: Modal Controller Presented!");
            
            uint32_t modalIsa = ((uint32_t*)modalVC)[0];
            void* impVDA = FindMethodIMP(modalIsa, "viewDidAppear:");
            if (impVDA) {
                LogToJava("HLE: Calling viewDidAppear: for Modal");
                typedef void (*VDAFunc)(void*, const char*, uint32_t);
                ((VDAFunc)impVDA)(modalVC, "viewDidAppear:", 1);
            }
            return 0;
        }

        if (strcmp(op, "dismissModalViewControllerAnimated:") == 0) {
            g_presentedView = nullptr; 
            g_pointerToUI.clear(); // Очищаем старые привязки кнопок модалки
            LogToJava("HLE: Modal Controller Dismissed!"); return 0;
        }

        if (strcmp(op, "class") == 0) return (uint64_t)isa;

        void* imp = FindMethodIMP(isa, op);
        if (imp) {
            LogToJava("OBJC-NATIVE-FORWARD: [" + cName + " " + std::string(op) + "]");
#if defined(__arm__)
            asm volatile (
                "vldr s0, [%0]\n"
                "vldr s1, [%0, #4]\n"
                "vldr s2, [%0, #8]\n"
                "vldr s3, [%0, #12]\n"
                : : "r"(saved_s) : "s0", "s1", "s2", "s3"
            );
#endif
            typedef uint64_t (*MethodType)(void*, const char*, void*, void*, void*, void*, void*, void*, void*, void*);
            return ((MethodType)imp)(self, op, a1, a2, a3, a4, a5, a6, a7, a8);
        }
        
        if (strncmp(op, "init", 4) == 0 || strcmp(op, "alloc") == 0) return (uint64_t)(uintptr_t)self; 

        if (strcmp(op, "touchesBegan:withEvent:") == 0 || strcmp(op, "touchesMoved:withEvent:") == 0 ||
            strcmp(op, "touchesEnded:withEvent:") == 0 || strcmp(op, "touchesCancelled:withEvent:") == 0) {
            return 0;
        }
        
        if (strcmp(op, "setMetricsId:") == 0) return 0;
        if (strcmp(op, "setClearsContextBeforeDrawing:") == 0) return 0;
        if (strcmp(op, "viewDidAppear:") == 0) return 0;
        
        if (strcmp(op, "openWithCompletionHandler:") == 0) {
            if (a1) {
                uint32_t* block = (uint32_t*)a1;
                typedef void (*BlockInvoke)(void*, uint32_t);
                BlockInvoke invoke = (BlockInvoke)block[3];
                if (invoke) {
                    invoke(block, 1); // 1 = YES (успешно)
                }
            }
            return 0;
        }

        std::string baseSysClass = GetBaseSystemClassName(isa);
        char ptrStr[32]; snprintf(ptrStr, sizeof(ptrStr), "0x%lx", (unsigned long)(uintptr_t)self);
        LogToJava(std::string("OBJC-TODO: Unimplemented Message [") + std::string(op) + "] sent to instance of (" + cName + "*) " + ptrStr + " <- base system class (" + baseSysClass + ")");
        return 0; 
    }
}

extern "C" uint64_t Stub_objc_msgSend(void* self, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
#if defined(__arm__)
    asm volatile("vstr s0, [%0]\n vstr s1, [%0, #4]\n vstr s2, [%0, #8]\n vstr s3, [%0, #12]" : : "r"(g_fpu_args) : "memory");
#endif
    uint64_t ret = Impl_objc_msgSend(self, op, a1, a2, a3, a4, a5, a6, a7, a8);
    if (g_fpu_ret_flag) {
#if defined(__arm__)
        asm volatile("vldr s0, [%0]\n vldr s1, [%0, #4]\n vldr s2, [%0, #8]\n vldr s3, [%0, #12]" : : "r"(g_fpu_ret) : "s0", "s1", "s2", "s3");
#endif
        g_fpu_ret_flag = 0;
    }
    return ret;
}

uint64_t Impl_objc_msgSendSuper2(void* super_struct, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (!super_struct || !isValidString(op)) return 0;
    uint32_t* sup = (uint32_t*)super_struct; void* receiver = (void*)sup[0]; uint32_t current_class = sup[1]; 
    if (strcmp(op, "frame") == 0 || strcmp(op, "bounds") == 0) {
        float rect[4] = {0.0f, 0.0f, (float)g_surfaceWidth, (float)g_surfaceHeight};
        std::string cName = GetObjCClassName(receiver);
        if (cName.find("UIScreen") != std::string::npos) {
            rect[2] = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
            rect[3] = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
        } else if (g_views.count(receiver)) { 
            rect[0] = g_views[receiver].frame[0]; rect[1] = g_views[receiver].frame[1]; rect[2] = g_views[receiver].frame[2]; rect[3] = g_views[receiver].frame[3]; 
        }
        if (rect[2] <= 0.0f || rect[3] <= 0.0f) { rect[2] = 480.0f; rect[3] = 320.0f; }
        LogToJava("[SUPER-MSG-DEBUG] Перехват " + std::string(op) + "! x=" + std::to_string(rect[0]) + " y=" + std::to_string(rect[1]) + " w=" + std::to_string(rect[2]) + " h=" + std::to_string(rect[3]));
        
        g_fpu_ret[0] = rect[0];
        g_fpu_ret[1] = rect[1];
        g_fpu_ret[2] = rect[2];
        g_fpu_ret[3] = rect[3];
        g_fpu_ret_flag = 1;
        
        uint32_t* r = (uint32_t*)rect;
        return ((uint64_t)r[1] << 32) | r[0];
    }
    if (current_class && current_class > 0x1000) {
        uint32_t* cls = (uint32_t*)current_class;
        if (cls[0] != 0xDEADBEEF) {
            uint32_t super_class = cls[1];
            if (super_class > 0x1000 && ((uint32_t*)super_class)[0] == 0xDEADBEEF) return (uint64_t)(uintptr_t)receiver;
            void* imp = FindMethodIMP(super_class, op);
            if (imp) {
                typedef uint64_t (*MethodType)(void*, const char*, void*, void*, void*, void*, void*, void*, void*, void*);
                return ((MethodType)imp)(receiver, op, a1, a2, a3, a4, a5, a6, a7, a8);
            }
        }
    }
    return (uint64_t)(uintptr_t)receiver;
}

extern "C" uint64_t Stub_objc_msgSendSuper2(void* super_struct, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    uint64_t ret = Impl_objc_msgSendSuper2(super_struct, op, a1, a2, a3, a4, a5, a6, a7, a8);
    if (g_fpu_ret_flag) {
#if defined(__arm__)
        asm volatile("vldr s0, [%0]\n vldr s1, [%0, #4]\n vldr s2, [%0, #8]\n vldr s3, [%0, #12]" : : "r"(g_fpu_ret) : "s0", "s1", "s2", "s3");
#endif
        g_fpu_ret_flag = 0;
    }
    return ret;
}

void* Impl_objc_msgSend_stret(void* ret_addr, void* self, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (!self || !isValidString(op)) return ret_addr;
    std::string cName = GetObjCClassName(self);
    char ptrStr[32]; snprintf(ptrStr, sizeof(ptrStr), "0x%lx", (unsigned long)(uintptr_t)self);
    LogToJava(std::string("OBJC-STRET-CALL: [(") + cName + "*) " + ptrStr + " " + std::string(op) + "]");
    if (strcmp(op, "acceleration") == 0) {
        if (ret_addr) {
            double* acc = (double*)ret_addr;
            acc[0] = g_latestAccelX;
            acc[1] = g_latestAccelY;
            acc[2] = g_latestAccelZ;
        }
        return ret_addr;
    }
    if (strcmp(op, "frame") == 0 || strcmp(op, "bounds") == 0) {
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        float w = (float)g_surfaceWidth;
        float h = (float)g_surfaceHeight;
        if (cName.find("UIScreen") != std::string::npos) {
            w = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
            h = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
        } else if (g_views.count(self)) { 
            w = g_views[self].frame[2]; h = g_views[self].frame[3]; 
        }
        
        // ВНИМАНИЕ: Возвращаем нормальный CGRect: x=0, y=0, w=width, h=height
        float rectData[4] = {0.0f, 0.0f, w, h};
        LogToJava(">>>>>>>> [SIZE-CRITICAL] STRET MSG_SEND " + std::string(op) + " <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr));
        LogToJava("  Class: " + cName + " | ptr: " + ptrStr + " | ret_addr: 0x" + std::to_string((uintptr_t)ret_addr));
        LogToJava("  Writing (Float): x=0 y=0 w=" + std::to_string(w) + " h=" + std::to_string(h));

        if (ret_addr) {
            LogToJava("  [STRET-MEM] До записи:    " + DumpHexToString((const char*)ret_addr, 16));
            memcpy(ret_addr, rectData, 16);
            LogToJava("  [STRET-MEM] После записи: " + DumpHexToString((const char*)ret_addr, 16));
        } else {
            LogToJava("  [STRET-MEM] ВНИМАНИЕ: ret_addr == NULL!");
        }

        g_fpu_ret[0] = rectData[0];
        g_fpu_ret[1] = rectData[1];
        g_fpu_ret[2] = rectData[2];
        g_fpu_ret[3] = rectData[3];
        g_fpu_ret_flag = 1;

        return ret_addr;
    }
    if (strcmp(op, "locationInView:") == 0 || strcmp(op, "previousLocationInView:") == 0) {
        if (ret_addr) {
            FakeUITouch* t = (FakeUITouch*)self;
            float rx = t->x, ry = t->y;
            if (g_gameViewportH > g_gameViewportW && g_surfaceWidth > g_surfaceHeight) {
                rx = (float)g_surfaceHeight - t->y;
                ry = t->x;
            }
            float* pt = (float*)ret_addr;
            pt[0] = rx;
            pt[1] = ry;
        }
        return ret_addr;
    }
    if (strcmp(op, "rangeOfString:options:") == 0 || strcmp(op, "rangeOfString:") == 0) {
        if (ret_addr) {
            uint32_t* range = (uint32_t*)ret_addr;
            std::string s1 = GetNSString(self);
            std::string s2 = GetNSString(a1);
            size_t pos = s1.find(s2);
            if (pos != std::string::npos) {
                range[0] = (uint32_t)pos;
                range[1] = (uint32_t)s2.length();
            } else {
                range[0] = 0x7FFFFFFF; // NSNotFound
                range[1] = 0;
            }
        }
        return ret_addr;
    }
    if (strcmp(op, "transform") == 0) {
        if (ret_addr) {
            CGAffineTransform* t = (CGAffineTransform*)ret_addr;
            *t = wrap_CGAffineTransformIdentity;
        }
        return ret_addr;
    }
    if (strcmp(op, "applicationFrame") == 0) {
        uint32_t lr = (uint32_t)__builtin_return_address(0);
        float w = (float)g_surfaceWidth;
        float h = (float)g_surfaceHeight;
        if (cName.find("UIScreen") != std::string::npos) {
            w = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
            h = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
        }
        LogToJava(">>>>>>>> [SIZE-CRITICAL] STRET applicationFrame <<<<<<<<");
        LogToJava("  Caller: " + GetModuleInfoForAddress(lr) + " | Class: " + cName + " | ptr: " + ptrStr + " | ret_addr: 0x" + std::to_string((uintptr_t)ret_addr));
        LogToJava("  Writing: x=0 y=0 w=" + std::to_string(w) + " h=" + std::to_string(h));
        
        if (ret_addr) {
            float* rect = (float*)ret_addr;
            rect[0] = 0.0f; rect[1] = 0.0f; rect[2] = w; rect[3] = h;
        }
        g_fpu_ret[0] = 0.0f; g_fpu_ret[1] = 0.0f; g_fpu_ret[2] = w; g_fpu_ret[3] = h;
        g_fpu_ret_flag = 1;
        return ret_addr;
    }
    if (strcmp(op, "statusBarFrame") == 0) {
        if (ret_addr) {
            float* rect = (float*)ret_addr;
            rect[0] = 0.0f; rect[1] = 0.0f; rect[2] = (float)g_surfaceWidth; rect[3] = 0.0f;
        }
        g_fpu_ret[0] = 0.0f; g_fpu_ret[1] = 0.0f; g_fpu_ret[2] = (float)g_surfaceWidth; g_fpu_ret[3] = 0.0f;
        g_fpu_ret_flag = 1;
        return ret_addr;
    }
    if (strcmp(op, "size") == 0 && cName.find("UIScreenMode") != std::string::npos) {
        if (ret_addr) {
            float* size = (float*)ret_addr;
            size[0] = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
            size[1] = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
        }
        return ret_addr;
    }
    uint32_t isa = ((uint32_t*)self)[0]; void* imp = FindMethodIMP(isa, op);
    if (imp) {
        typedef void* (*StretMethod)(void*, void*, const char*, void*, void*, void*, void*, void*, void*, void*);
        return ((StretMethod)imp)(ret_addr, self, op, a1, a2, a3, a4, a5, a6, a7);
    }
    LogToJava(std::string("OBJC-STRET-TODO: Unimplemented STRET Method -[(") + cName + "*) " + ptrStr + " " + std::string(op) + "]");
    return ret_addr;
}

extern "C" void* Stub_objc_msgSend_stret(void* ret_addr, void* self, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    void* ret = Impl_objc_msgSend_stret(ret_addr, self, op, a1, a2, a3, a4, a5, a6, a7);
    if (g_fpu_ret_flag) {
#if defined(__arm__)
        asm volatile("vldr s0, [%0]\n vldr s1, [%0, #4]\n vldr s2, [%0, #8]\n vldr s3, [%0, #12]" : : "r"(g_fpu_ret) : "s0", "s1", "s2", "s3");
#endif
        g_fpu_ret_flag = 0;
    }
    return ret;
}

void* Impl_objc_msgSendSuper2_stret(void* ret_addr, void* super_struct, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    if (!super_struct || !isValidString(op)) return ret_addr;
    uint32_t* sup = (uint32_t*)super_struct; void* receiver = (void*)sup[0]; uint32_t current_class = sup[1]; 
    std::string cName = GetObjCClassName(receiver);
    LogToJava(std::string("OBJC-SUPER-STRET-CALL: [(") + cName + "*) " + std::string(op) + "]");
    if (strcmp(op, "frame") == 0 || strcmp(op, "bounds") == 0) {
        LogToJava("[SUPER-STRET-DEBUG] Перехват " + std::string(op) + "! ret_addr=0x" + std::to_string((uintptr_t)ret_addr));
        if (ret_addr) {
            float rectData[4] = {11.0f, 22.0f, (float)g_surfaceWidth, (float)g_surfaceHeight};
            if (cName.find("UIScreen") != std::string::npos) {
                rectData[2] = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
                rectData[3] = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
            } else if (g_views.count(receiver)) { 
                rectData[0] = g_views[receiver].frame[0]; rectData[1] = g_views[receiver].frame[1]; rectData[2] = g_views[receiver].frame[2]; rectData[3] = g_views[receiver].frame[3]; 
            }
            if (rectData[2] <= 0.0f || rectData[3] <= 0.0f) {
                            LogToJava("[SUPER-STRET-DEBUG] Форсируем 480x320");
            rectData[2] = 480.0f; rectData[3] = 320.0f;
        }
        memcpy(ret_addr, rectData, 16);
        LogToJava("[SUPER-STRET-DEBUG] Записано: x=" + std::to_string(rectData[0]) + " y=" + std::to_string(rectData[1]) + " w=" + std::to_string(rectData[2]) + " h=" + std::to_string(rectData[3]));

        g_fpu_ret[0] = rectData[0];
        g_fpu_ret[1] = rectData[1];
        g_fpu_ret[2] = rectData[2];
        g_fpu_ret[3] = rectData[3];
        g_fpu_ret_flag = 1;
    }
    return ret_addr;
}

    if (strcmp(op, "locationInView:") == 0 || strcmp(op, "previousLocationInView:") == 0) {
        if (ret_addr) {
            FakeUITouch* t = (FakeUITouch*)receiver;
            float rx = t->x, ry = t->y;
            if (g_gameViewportH > g_gameViewportW && g_surfaceWidth > g_surfaceHeight) {
                rx = (float)g_surfaceHeight - t->y;
                ry = t->x;
            }
            float* pt = (float*)ret_addr;
            pt[0] = rx;
            pt[1] = ry;
        }
        return ret_addr;
    }
    if (strcmp(op, "rangeOfString:options:") == 0 || strcmp(op, "rangeOfString:") == 0) {
        if (ret_addr) {
            uint32_t* range = (uint32_t*)ret_addr;
            std::string s1 = GetNSString(receiver);
            std::string s2 = GetNSString(a1);
            size_t pos = s1.find(s2);
            if (pos != std::string::npos) {
                range[0] = (uint32_t)pos;
                range[1] = (uint32_t)s2.length();
            } else {
                range[0] = 0x7FFFFFFF; // NSNotFound
                range[1] = 0;
            }
        }
        return ret_addr;
    }
    if (strcmp(op, "transform") == 0) {
        if (ret_addr) {
            CGAffineTransform* t = (CGAffineTransform*)ret_addr;
            *t = wrap_CGAffineTransformIdentity;
        }
        return ret_addr;
    }
    if (strcmp(op, "applicationFrame") == 0) {
        float w = (float)g_surfaceWidth;
        float h = (float)g_surfaceHeight;
        if (cName.find("UIScreen") != std::string::npos) {
            w = std::min((float)g_surfaceWidth, (float)g_surfaceHeight);
            h = std::max((float)g_surfaceWidth, (float)g_surfaceHeight);
        }
        float rectData[4] = {0.0f, 0.0f, w, h};
        if (ret_addr) {
            memcpy(ret_addr, rectData, 16);
            LogToJava("[SIZE-TRACE] STRET applicationFrame записал: " + std::to_string(w) + "x" + std::to_string(h));
        }
        
        g_fpu_ret[0] = rectData[0];
        g_fpu_ret[1] = rectData[1];
        g_fpu_ret[2] = rectData[2];
        g_fpu_ret[3] = rectData[3];
        g_fpu_ret_flag = 1;
        
        return ret_addr;
    }
    if (strcmp(op, "statusBarFrame") == 0) {
        if (ret_addr) {
            float* rect = (float*)ret_addr;
            rect[0] = 0.0f; rect[1] = 0.0f; rect[2] = (float)g_surfaceWidth; rect[3] = 0.0f;
        }
        return ret_addr;
    }
    if (strcmp(op, "size") == 0 && cName.find("UIScreenMode") != std::string::npos) {
        if (ret_addr) {
            float* size = (float*)ret_addr;
            size[0] = (float)g_surfaceWidth;
            size[1] = (float)g_surfaceHeight;
        }
        return ret_addr;
    }
    if (current_class && current_class > 0x1000) {
        uint32_t* cls = (uint32_t*)current_class;
        if (cls[0] != 0xDEADBEEF) {
            uint32_t super_class = cls[1];
            if (super_class > 0x1000 && ((uint32_t*)super_class)[0] == 0xDEADBEEF) return ret_addr;
            void* imp = FindMethodIMP(super_class, op);
            if (imp) {
                typedef void* (*StretMethod)(void*, void*, const char*, void*, void*, void*, void*, void*, void*, void*);
                return ((StretMethod)imp)(ret_addr, receiver, op, a1, a2, a3, a4, a5, a6, a7);
            }
        }
    }
    LogToJava(std::string("OBJC-SUPER-STRET-TODO: Unimplemented -[(") + cName + "*) " + std::string(op) + "]");
    return ret_addr;
}

extern "C" void* Stub_objc_msgSendSuper2_stret(void* ret_addr, void* super_struct, const char* op, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
    void* ret = Impl_objc_msgSendSuper2_stret(ret_addr, super_struct, op, a1, a2, a3, a4, a5, a6, a7);
    if (g_fpu_ret_flag) {
#if defined(__arm__)
        asm volatile("vldr s0, [%0]\n vldr s1, [%0, #4]\n vldr s2, [%0, #8]\n vldr s3, [%0, #12]" : : "r"(g_fpu_ret) : "s0", "s1", "s2", "s3");
#endif
        g_fpu_ret_flag = 0;
    }
    return ret;
}

extern "C" void Stub_objc_setProperty(void* self, const char* op, ptrdiff_t offset, void* newValue) { if (self) { void** target = (void**)((uint8_t*)self + offset); *target = newValue; } }
extern "C" void* wrap_objc_getProperty(void* self, const char* op, ptrdiff_t offset, bool atomic) {
    if (!self) return nullptr;
    return *(void**)((uint8_t*)self + offset);
}
extern "C" void Stub_exit(int code) { 
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava("HLE_CALL: Игра вызвала exit(" + std::to_string(code) + ")! Caller: " + GetModuleInfoForAddress(lr)); 
    __builtin_trap(); 
}

// --- OBJC C-API STUBS ---
extern "C" const char* wrap_class_getName(void* cls) { return cls ? GetObjCClassName(cls).c_str() : "nil"; }
extern "C" size_t wrap_class_getInstanceSize(void* cls_ptr) {
    if (!cls_ptr || (uintptr_t)cls_ptr < 0x1000) return 0;
    uint32_t* cls = (uint32_t*)cls_ptr;
    if (cls[0] == 0xDEADBEEF) return 32;
    uint32_t data_ptr = cls[4] & ~3;
    if (data_ptr > 0x1000) return ((uint32_t*)data_ptr)[2];
    return 0;
}
extern "C" void* wrap_class_getSuperclass(void* cls_ptr) {
    if (!cls_ptr || (uintptr_t)cls_ptr < 0x1000) return nullptr;
    uint32_t* cls = (uint32_t*)cls_ptr;
    if (cls[0] == 0xDEADBEEF) return nullptr;
    return (void*)cls[1];
}
extern "C" const char* wrap_ivar_getName(void* ivar_ptr) {
    if (!ivar_ptr || (uintptr_t)ivar_ptr < 0x1000) return nullptr;
    return (const char*)((uint32_t*)ivar_ptr)[1];
}
extern "C" ptrdiff_t wrap_ivar_getOffset(void* ivar_ptr) {
    if (!ivar_ptr || (uintptr_t)ivar_ptr < 0x1000) return 0;
    uint32_t offset_ptr = ((uint32_t*)ivar_ptr)[0];
    if (offset_ptr > 0x1000) return *(int32_t*)offset_ptr;
    return 0;
}
struct objc_method_description { void* name; char* types; };
extern "C" objc_method_description wrap_protocol_getMethodDescription(void* p, const char* aSel, bool isRequiredMethod, bool isInstanceMethod) {
    return {nullptr, nullptr};
}

std::map<std::pair<void*, const void*>, void*> g_associatedObjects;

extern "C" void wrap_objc_setAssociatedObject(void* object, const void* key, void* value, uintptr_t policy) {
    if (!object || !key) return;
    if (value) {
        g_associatedObjects[{object, key}] = value;
    } else {
        g_associatedObjects.erase({object, key});
    }
}

extern "C" void* wrap_objc_getAssociatedObject(void* object, const void* key) {
    if (!object || !key) return nullptr;
    auto it = g_associatedObjects.find({object, key});
    if (it != g_associatedObjects.end()) return it->second;
    return nullptr;
}

extern "C" void* wrap_objc_retain(void* obj) {
    return obj; // Управление памятью перехвачено HLE
}
extern "C" void* wrap_objc_lookUpClass(const char* name) { return name ? ResolveSymbol(std::string("OBJC_CLASS_$_") + name) : nullptr; }
extern "C" void* wrap_class_getInstanceMethod(void* cls, const char* name) {
    if (!cls || !name) return nullptr;
    void* imp = FindMethodIMP((uint32_t)(uintptr_t)cls, name);
    if (!imp) return nullptr;
    HLE_Method* m = new HLE_Method{name, "v@:", imp};
    return (void*)m;
}
extern "C" void* wrap_class_getClassMethod(void* cls, const char* name) {
    return wrap_class_getInstanceMethod(cls, name);
}
extern "C" void* wrap_method_getImplementation(void* m) { return m ? ((HLE_Method*)m)->imp : nullptr; }
extern "C" const char* wrap_method_getName(void* m) { return m ? ((HLE_Method*)m)->name : nullptr; }
extern "C" int wrap_objc_getClassList(void** buffer, int bufferCount) {
    int total = g_hleClasses.size();
    if (buffer && bufferCount > 0) {
        int i = 0;
        for (auto const& pair : g_hleClasses) {
            if (i >= bufferCount) break;
            buffer[i++] = pair.second;
        }
        return i;
    }
    return total;
}
extern "C" void* wrap_object_getClass(void* obj) { if(!obj) return nullptr; uint32_t isa = ((uint32_t*)obj)[0]; return (void*)isa; }
extern "C" void wrap_objc_copyStruct(void* dest, const void* src, size_t size, bool atomic, bool hasStrong) { memcpy(dest, src, size); }

extern "C" void* wrap_NSClassFromString(void* nsstr) {
    std::string className = GetNSString(nsstr);
    if (className.empty()) return nullptr;
    return ResolveSymbol("OBJC_CLASS_$_" + className);
}

extern "C" int32_t wrap_OSAtomicOr32Barrier(uint32_t theMask, volatile uint32_t *theValue) {
    return __sync_or_and_fetch(theValue, theMask);
}

extern "C" bool wrap_OSAtomicTestAndClearBarrier(uint32_t n, volatile void *theAddress) {
    volatile uint8_t *byteAddr = (volatile uint8_t *)theAddress + (n / 8);
    uint8_t mask = 0x80 >> (n % 8);
    uint8_t old = __sync_fetch_and_and(byteAddr, ~mask);
    return (old & mask) != 0;
}

extern "C" void wrap_OSSpinLockLock(volatile int32_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        sched_yield();
    }
}

extern "C" bool wrap_OSSpinLockTry(volatile int32_t *lock) {
    return __sync_bool_compare_and_swap(lock, 0, 1);
}

extern "C" void wrap_OSSpinLockUnlock(volatile int32_t *lock) {
    __sync_lock_release(lock);
}

// --- AIO / SYSTEM STUBS ---
void FillIOSStat(struct stat& android_st, void* ios_stat_buf) {
    uint32_t* p32 = (uint32_t*)ios_stat_buf;
    memset(p32, 0, 108); // 108 байт для 32-bit ARM Darwin
    p32[0] = android_st.st_dev; 
    uint16_t* p16 = (uint16_t*)&p32[1];
    p16[0] = android_st.st_mode; 
    p16[1] = android_st.st_nlink; 
    uint64_t* p64 = (uint64_t*)&p32[2];
    p64[0] = android_st.st_ino; 
    p32[4] = android_st.st_uid; 
    p32[5] = android_st.st_gid; 
    p32[6] = android_st.st_rdev; 
    p32[7] = android_st.st_atime; 
    p32[9] = android_st.st_mtime; 
    p32[11] = android_st.st_ctime; 
    p32[13] = android_st.st_ctime; // birthtime fallback
    p64 = (uint64_t*)&p32[15]; // смещение 60
    p64[0] = android_st.st_size; 
    p64[1] = android_st.st_blocks; 
    p32[19] = android_st.st_blksize; 
}

extern "C" int wrap_stat(const char* path, void* ios_stat_buf) {
    if (!path || !ios_stat_buf) return -1;
    std::string sPath = path;
    bool isRelative = (sPath.length() > 0 && sPath[0] != '/');
    if (isRelative) sPath = g_sandboxDir + "Documents/" + sPath;
    
    if (sPath.find(".dll") != std::string::npos || sPath.find("mscorlib") != std::string::npos || sPath.find("Mono") != std::string::npos || sPath.find("Data/Managed") != std::string::npos) {
        LogToJava("MONO-TRACE: [stat] Запрос инфы: " + sPath);
    } else {
        LogToBlackBox("C-API-TRACE: [stat] Запрос инфы о пути: " + sPath);
    }
    struct stat android_st;
    int res = stat(sPath.c_str(), &android_st);
    if (res != 0 && isRelative) {
        sPath = g_appBundlePath + "/" + path;
        res = stat(sPath.c_str(), &android_st);
    }
    if (res == 0) FillIOSStat(android_st, ios_stat_buf);
    return res;
}

extern "C" int wrap_fstat(int fd, void* ios_stat_buf) {
    if (!ios_stat_buf) return -1;
    struct stat android_st;
    int res = fstat(fd, &android_st);
    if (res == 0) FillIOSStat(android_st, ios_stat_buf);
    return res;
}

extern "C" int wrap_lstat(const char* path, void* ios_stat_buf) {
    if (!path || !ios_stat_buf) return -1;
    std::string sPath = path;
    bool isRelative = (sPath.length() > 0 && sPath[0] != '/');
    if (isRelative) sPath = g_sandboxDir + "Documents/" + sPath;
    
    struct stat android_st;
    int res = lstat(sPath.c_str(), &android_st);
    if (res != 0 && isRelative) {
        sPath = g_appBundlePath + "/" + path;
        res = lstat(sPath.c_str(), &android_st);
    }
    if (res == 0) FillIOSStat(android_st, ios_stat_buf);
    return res;
}

extern "C" int wrap_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) { return sigprocmask(how, set, oldset); }
std::map<GLenum, std::vector<uint8_t>> g_mapBufferData;
extern "C" void* wrap_glMapBufferOES(GLenum target, GLenum access) { 
    LogToJava("[GL-TRACE] glMapBufferOES(target=" + std::to_string(target) + ", access=" + std::to_string(access) + ")");
    GLint size = 0;
    GLuint buffer = g_boundBuffers[target];
    if (buffer != 0 && g_vboShadow.count(buffer)) {
        size = g_vboShadow[buffer].size();
    }
    if (size <= 0) {
        LogToJava("[GL-TRACE] glMapBufferOES ERROR: Buffer size is <= 0! Returning dummy 1MB buffer.");
        size = 1024 * 1024; // Safe fallback
    }
    g_mapBufferData[target].resize(size);
    return g_mapBufferData[target].data();
}
extern "C" GLboolean wrap_glUnmapBufferOES(GLenum target) { 
    LogToJava("[GL-TRACE] glUnmapBufferOES(target=" + std::to_string(target) + ")");
    if (g_mapBufferData.count(target)) {
        GLuint buffer = g_boundBuffers[target];
        if (buffer != 0) {
            auto& buf = g_vboShadow[buffer];
            if (g_mapBufferData[target].size() > buf.size()) buf.resize(g_mapBufferData[target].size());
            memcpy(buf.data(), g_mapBufferData[target].data(), g_mapBufferData[target].size());
        }
        // БОЛЬШЕ НЕ ПРОБРАСЫВАЕМ В РЕАЛЬНЫЙ ДРАЙВЕР
        g_mapBufferData.erase(target);
        return GL_TRUE;
    }
    return GL_FALSE; 
}

// --- FRAMEWORK & CRYPTO STUBS ---
struct CFRange { int location; int length; };
struct HLE_CFData { uint32_t isa; std::vector<uint8_t> data; };
struct HLE_CFHTTPMessage { bool isRequest; std::string method; void* url; int statusCode; std::string statusDescription; std::map<std::string, std::string> headers; std::vector<uint8_t> body; };
struct HLE_CFReadStream { HLE_CFHTTPMessage* request; void* clientCB; void* clientCtx; };
struct HLE_CFRunLoopTimer { void* callback; void* info; double interval; bool repeats; bool valid; };

extern "C" void* wrap_CFRetain(void* cf) { return cf; }
extern "C" void wrap_CFRelease(void* cf) { /* Игнорируем вызов для стабильности HLE (избегаем двойных free) */ }

extern "C" uint64_t Stub_mach_absolute_time();

extern "C" double wrap_CACurrentMediaTime() { 
    return (double)Stub_mach_absolute_time() / 1000000000.0; 
}

extern "C" void* wrap_NSStringFromCGSize(CGSize size) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{%g, %g}", size.width, size.height);
    return CreateNSString(buf);
}

extern "C" void* wrap_SCNetworkReachabilityCreateWithName(void* allocator, const char* nodename) {
    return (void*)0xDEADBEEF; // Возвращаем стабильный dummy handle для симуляции оффлайн-сети
}

// --- NEW FULLY IMPLEMENTED CF APIs ---
extern "C" uint32_t wrap_CFGetTypeID(void* cf) { return 1; }
extern "C" uint32_t wrap_CFBooleanGetTypeID() { return 2; }
extern "C" uint8_t wrap_CFBooleanGetValue(void* cf) { return cf == (void*)1; }

extern "C" void* wrap_CFDataCreate(void* alloc, const uint8_t* bytes, int length) {
    HLE_CFData* d = new HLE_CFData{ g_hleClasses.count("NSData") ? (uint32_t)g_hleClasses["NSData"] : 0xDEADBEEF };
    if(bytes && length > 0) d->data.assign(bytes, bytes + length);
    return d;
}
extern "C" const uint8_t* wrap_CFDataGetBytePtr(void* cfData) { return cfData ? ((HLE_CFData*)cfData)->data.data() : nullptr; }
extern "C" void wrap_CFDataGetBytes(void* cfData, CFRange range, uint8_t* buffer) {
    if (!cfData || !buffer) return; HLE_CFData* d = (HLE_CFData*)cfData;
    if (range.location >= 0 && range.location + range.length <= d->data.size()) memcpy(buffer, d->data.data() + range.location, range.length);
}
extern "C" int wrap_CFDataGetLength(void* cfData) { return cfData ? ((HLE_CFData*)cfData)->data.size() : 0; }

extern "C" void* wrap_CFErrorCopyDescription(void* err) { return CreateNSString("HLE CFError"); }

extern "C" void* wrap_CFHTTPMessageCreateRequest(void* alloc, void* requestMethod, void* url, uint8_t httpVersion) {
    HLE_CFHTTPMessage* m = new HLE_CFHTTPMessage(); m->isRequest = true; m->method = GetNSString(requestMethod); m->url = url; return m;
}
extern "C" void* wrap_CFHTTPMessageCopyHeaderFieldValue(void* request, void* headerField) {
    if (!request || !headerField) return nullptr; HLE_CFHTTPMessage* m = (HLE_CFHTTPMessage*)request;
    std::string key = GetNSString(headerField); return m->headers.count(key) ? CreateNSString(m->headers[key]) : nullptr;
}
extern "C" void wrap_CFHTTPMessageSetHeaderFieldValue(void* request, void* headerField, void* value) {
    if (!request || !headerField || !value) return; HLE_CFHTTPMessage* m = (HLE_CFHTTPMessage*)request;
    m->headers[GetNSString(headerField)] = GetNSString(value);
}
extern "C" void wrap_CFHTTPMessageSetBody(void* request, void* bodyData) {
    if (!request || !bodyData) return; HLE_CFHTTPMessage* m = (HLE_CFHTTPMessage*)request; HLE_CFData* d = (HLE_CFData*)bodyData;
    m->body = d->data;
}
extern "C" void* wrap_CFHTTPMessageCopyResponseStatusLine(void* request) { return CreateNSString("HTTP/1.1 200 OK"); }
extern "C" void* wrap_CFHTTPMessageCopySerializedMessage(void* request) { return wrap_CFDataCreate(nullptr, nullptr, 0); }
extern "C" int wrap_CFHTTPMessageGetResponseStatusCode(void* request) { return 200; }

extern "C" void* wrap_CFNumberCreate(void* alloc, int theType, const void* valuePtr) {
    uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSNumber");
    if (theType == 9 || theType == 3) inst[1] = (uint32_t)(*(int*)valuePtr);
    else if (theType == 12 || theType == 13) { float f = (theType == 12) ? *(float*)valuePtr : (float)(*(double*)valuePtr); memcpy(&inst[1], &f, 4); }
    inst[2] = theType; return inst;
}
extern "C" uint8_t wrap_CFNumberGetValue(void* number, int theType, void* valuePtr) {
    if (!number || !valuePtr) return 0; uint32_t* inst = (uint32_t*)number;
    if (theType == 9 || theType == 3) { *(int*)valuePtr = (int)inst[1]; return 1; }
    if (theType == 12) { *(float*)valuePtr = *(float*)&inst[1]; return 1; }
    if (theType == 13) { *(double*)valuePtr = (double)(*(float*)&inst[1]); return 1; }
    return 0;
}
extern "C" int wrap_CFNumberGetType(void* number) { return number ? ((uint32_t*)number)[2] : 0; }
extern "C" uint32_t wrap_CFNumberGetTypeID() { return 3; }

extern "C" void* wrap_CFNumberFormatterCreate(void* alloc, void* locale, int style) { return (void*)1; }
extern "C" void* wrap_CFNumberFormatterGetValueFromString(void* formatter, void* string, CFRange* rangep, int options, void** errorp) {
    std::string s = GetNSString(string); int val = atoi(s.c_str());
    uint32_t* inst = (uint32_t*)calloc(1, 32); inst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSNumber"); inst[1] = val; inst[2] = 9; return inst;
}

extern "C" void* wrap_CFPreferencesCopyAppValue(void* key, void* appID) { return g_userDefaults[GetNSString(key)]; }
extern "C" void wrap_CFPreferencesSetValue(void* key, void* value, void* appID, void* user, void* host) { g_userDefaults[GetNSString(key)] = value; }
extern "C" uint8_t wrap_CFPreferencesSynchronize(void* appID, void* user, void* host) { return 1; }
extern "C" void* wrap_CFPreferencesCopyKeyList(void* appID, void* user, void* host) {
    uint32_t* arrInst = (uint32_t*)calloc(1, 32); arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
    for (auto const& pair : g_userDefaults) g_arrays[arrInst].push_back(CreateNSString(pair.first));
    return arrInst;
}

extern "C" void* wrap_CFReadStreamCreateForHTTPRequest(void* alloc, void* request) {
    HLE_CFReadStream* rs = new HLE_CFReadStream(); rs->request = (HLE_CFHTTPMessage*)request; return rs;
}
extern "C" uint8_t wrap_CFReadStreamOpen(void* stream) { return 1; }
extern "C" void wrap_CFReadStreamClose(void* stream) { if (stream) delete (HLE_CFReadStream*)stream; }
extern "C" uint8_t wrap_CFReadStreamSetProperty(void* stream, void* propertyName, void* propertyValue) { return 1; }
extern "C" void wrap_CFReadStreamSetClient(void* stream, uint32_t streamEvents, void* clientCB, void* clientContext) {
    if (stream) { ((HLE_CFReadStream*)stream)->clientCB = clientCB; if (clientContext) ((HLE_CFReadStream*)stream)->clientCtx = ((void**)clientContext)[0]; }
}
extern "C" void wrap_CFReadStreamScheduleWithRunLoop(void* stream, void* runLoop, void* runLoopMode) {
    HLE_CFReadStream* rs = (HLE_CFReadStream*)stream;
    if (rs && rs->clientCB) {
        typedef void (*StreamCB)(void*, uint32_t, void*);
        ((StreamCB)rs->clientCB)(rs, 1, rs->clientCtx); // kCFStreamEventOpenCompleted
        ((StreamCB)rs->clientCB)(rs, 16, rs->clientCtx); // kCFStreamEventEndEncountered
    }
}

extern "C" void* wrap_CFRunLoopTimerCreate(void* alloc, double fireDate, double interval, uint32_t flags, uint32_t order, void* info, void* callback) {
    HLE_CFRunLoopTimer* t = new HLE_CFRunLoopTimer(); t->callback = callback;
    if (info) t->info = ((void**)info)[0]; t->interval = interval; t->repeats = (interval > 0); t->valid = true; return t;
}
extern "C" void wrap_CFRunLoopAddTimer(void* rl, void* timer, void* mode) {
    HLE_CFRunLoopTimer* t = (HLE_CFRunLoopTimer*)timer; if (!t || !t->valid) return;
    struct TimerArgs { HLE_CFRunLoopTimer* t; }; TimerArgs* args = new TimerArgs{t};
    pthread_t pt; pthread_create(&pt, nullptr, [](void* p) -> void* {
        TimerArgs* a = (TimerArgs*)p;
        do {
            if (!a->t->valid) break; usleep((int)(a->t->interval * 1000000.0)); if (!a->t->valid) break;
            pthread_mutex_lock(&g_mainQueueMutex); typedef void (*TimerCB)(void*, void*);
            ((TimerCB)a->t->callback)(a->t, a->t->info); pthread_mutex_unlock(&g_mainQueueMutex);
        } while (a->t->repeats);
        delete a; return nullptr;
    }, args);
}
extern "C" void wrap_CFRunLoopTimerInvalidate(void* timer) { if (timer) ((HLE_CFRunLoopTimer*)timer)->valid = false; }
extern "C" int wrap_CFRunLoopRunInMode(void* mode, double seconds, uint8_t returnAfterSourceHandled) { usleep((int)(seconds * 1000000.0)); return 1; }

extern "C" void* wrap_CFStringCreateExternalRepresentation(void* alloc, void* theString, uint32_t encoding, uint8_t lossByte) {
    std::string s = GetNSString(theString); HLE_CFData* d = new HLE_CFData{ g_hleClasses.count("NSData") ? (uint32_t)g_hleClasses["NSData"] : 0xDEADBEEF };
    d->data.assign(s.begin(), s.end()); return d;
}
extern "C" void* wrap_CFStringCreateFromExternalRepresentation(void* alloc, void* data, uint32_t encoding) {
    HLE_CFData* d = (HLE_CFData*)data; if (!d) return CreateNSString("");
    return CreateNSString(std::string((char*)d->data.data(), d->data.size()));
}
extern "C" void* wrap_CFStringCreateWithCStringNoCopy(void* alloc, const char* cStr, uint32_t encoding, void* contentsDeallocator) { return CreateNSString(cStr ? cStr : ""); }
extern "C" void* wrap_CFStringCreateWithCharacters(void* alloc, const uint16_t* chars, int numChars) {
    std::string s; if (chars) { for (int i=0; i<numChars; i++) s += (char)(chars[i] & 0xFF); } return CreateNSString(s);
}
extern "C" void wrap_CFStringGetCharacters(void* theString, CFRange range, uint16_t* buffer) {
    std::string s = GetNSString(theString);
    if (range.location >= 0 && range.location + range.length <= s.length()) {
        for (int i=0; i<range.length; i++) buffer[i] = (uint16_t)s[range.location + i];
    }
}
extern "C" void wrap_CFStringAppendCharacters(void* theString, const uint16_t* chars, int numChars) {
    if (!theString || !chars || numChars <= 0) return;
    std::string base = GetNSString(theString);
    for (int i = 0; i < numChars; i++) {
        base += (char)(chars[i] & 0xFF);
    }
    ((uint32_t*)theString)[2] = (uint32_t)strdup(base.c_str());
}
extern "C" void* wrap_CFStringConvertEncodingToIANACharSetName(uint32_t encoding) {
    if (encoding == 0x08000100 || encoding == 4) return CreateNSString("utf-8");
    if (encoding == 0x0600 || encoding == 1) return CreateNSString("us-ascii");
    if (encoding == 0x0100 || encoding == 5) return CreateNSString("iso-8859-1");
    return CreateNSString("utf-8");
}
extern "C" uint32_t wrap_CFStringConvertEncodingToNSStringEncoding(uint32_t encoding) {
    if (encoding == 0x08000100) return 4; // NSUTF8StringEncoding
    if (encoding == 0x0600 || encoding == 0) return 1; // NSASCIIStringEncoding
    if (encoding == 0x0100) return 5; // NSISOLatin1StringEncoding
    return 4;
}
extern "C" uint32_t wrap_CFStringConvertIANACharSetNameToEncoding(void* theString) {
    std::string s = GetNSString(theString);
    for (auto& c : s) c = tolower(c);
    if (s == "utf-8" || s == "utf8") return 0x08000100;
    if (s == "us-ascii" || s == "ascii") return 0x0600;
    if (s == "iso-8859-1") return 0x0100;
    return 0x08000100;
}
extern "C" uint32_t wrap_CFStringConvertNSStringEncodingToEncoding(uint32_t encoding) {
    if (encoding == 4) return 0x08000100;
    if (encoding == 1) return 0x0600;
    if (encoding == 5) return 0x0100;
    return 0x08000100;
}
extern "C" int wrap_CFStringGetMaximumSizeForEncoding(int length, uint32_t encoding) { return length * 4; }
extern "C" uint32_t wrap_CFStringGetTypeID() { return 4; }

extern "C" void* wrap_CFURLCreateFromFileSystemRepresentation(void* alloc, const uint8_t* buffer, int bufLen, uint8_t isDir) {
    uint32_t* inst = (uint32_t*)calloc(1, 16); inst[0] = g_hleClasses.count("NSURL") ? (uint32_t)g_hleClasses["NSURL"] : 0xDEADBEEF;
    inst[1] = (uint32_t)CreateNSString(std::string((const char*)buffer, bufLen)); return inst;
}
extern "C" void* wrap_CFURLCreateStringByAddingPercentEscapes(void* alloc, void* originalString, void* charactersToLeaveUnescaped, void* legalURLCharactersToBeEscaped, uint32_t encoding) { return originalString; }
extern "C" void* wrap_CFURLCreateStringByReplacingPercentEscapes(void* alloc, void* originalString, void* charactersToLeaveEscaped) { return originalString; }
extern "C" void* wrap_CFURLCreateWithString(void* alloc, void* urlString, void* baseURL) {
    uint32_t* inst = (uint32_t*)calloc(1, 16); inst[0] = g_hleClasses.count("NSURL") ? (uint32_t)g_hleClasses["NSURL"] : 0xDEADBEEF;
    inst[1] = (uint32_t)urlString; return inst;
}
extern "C" void* wrap_CFAllocatorGetDefault() { return (void*)1; }
extern "C" uint32_t wrap_CFBundleGetVersionNumber(void* bundle) { return 1; }

extern "C" uint8_t wrap_CFDictionaryGetValueIfPresent(void* dict, void* key, void** value) {
    if (!dict || !key) return 0;
    if (g_dictionaries[dict].count(key)) {
        if (value) *value = g_dictionaries[dict][key];
        return 1;
    }
    return 0;
}

extern "C" void* wrap_CFNotificationCenterGetLocalCenter() { return (void*)1; }

extern "C" void wrap_CFStringAppend(void* theString, void* appendedString) {
    if (!theString || !appendedString) return;
    std::string base = GetNSString(theString);
    base += GetNSString(appendedString);
    ((uint32_t*)theString)[2] = (uint32_t)strdup(base.c_str());
}

extern "C" void wrap_CFStringAppendFormat(void* theString, void* formatOptions, void* format, ...) {
    if (!theString || !format) return;
    std::string fmt = GetNSString(format);
    va_list args; va_start(args, format);
    void* vargs[4] = {nullptr};
    for(int i=0; i<4; i++) vargs[i] = va_arg(args, void*);
    va_end(args);
    int argIdx = 0; std::string res = "";
    for (size_t i = 0; i < fmt.length(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.length()) {
            char type = fmt[i+1];
            void* arg = argIdx < 4 ? vargs[argIdx++] : nullptr;
            if (type == '@') res += GetNSString(arg);
            else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
            else if (type == 's') res += arg ? (const char*)arg : "null";
            else { res += "%"; res += type; }
            i++;
        } else res += fmt[i];
    }
    std::string base = GetNSString(theString);
    base += res;
    ((uint32_t*)theString)[2] = (uint32_t)strdup(base.c_str());
}

extern "C" int wrap_CFStringCompare(void* theString1, void* theString2, uint32_t compareOptions) {
    std::string s1 = GetNSString(theString1);
    std::string s2 = GetNSString(theString2);
    if (compareOptions & 1) { // kCFCompareCaseInsensitive
        for(auto& c : s1) c = tolower(c);
        for(auto& c : s2) c = tolower(c);
    }
    if (s1 < s2) return -1;
    if (s1 > s2) return 1;
    return 0;
}

extern "C" void* wrap_CFStringCreateArrayBySeparatingStrings(void* alloc, void* theString, void* separatorString) {
    std::string s = GetNSString(theString);
    std::string delim = GetNSString(separatorString);
    uint32_t* arrInst = (uint32_t*)calloc(1, 32); arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
    size_t pos = 0;
    if (!delim.empty()) {
        while ((pos = s.find(delim)) != std::string::npos) {
            g_arrays[arrInst].push_back(CreateNSString(s.substr(0, pos)));
            s.erase(0, pos + delim.length());
        }
    }
    g_arrays[arrInst].push_back(CreateNSString(s));
    return arrInst;
}

extern "C" void* wrap_CFStringCreateCopy(void* alloc, void* theString) { return CreateNSString(GetNSString(theString)); }
extern "C" void* wrap_CFStringCreateMutable(void* alloc, int maxLength) { return CreateNSString(""); }
extern "C" void* wrap_CFStringCreateMutableCopy(void* alloc, int maxLength, void* theString) { return CreateNSString(GetNSString(theString)); }

extern "C" void* wrap_CFStringCreateWithFormat(void* alloc, void* formatOptions, void* format, ...) {
    if (!format) return CreateNSString("");
    std::string fmt = GetNSString(format);
    va_list args; va_start(args, format);
    void* vargs[4] = {nullptr};
    for(int i=0; i<4; i++) vargs[i] = va_arg(args, void*);
    va_end(args);
    int argIdx = 0; std::string res = "";
    for (size_t i = 0; i < fmt.length(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.length()) {
            char type = fmt[i+1];
            void* arg = argIdx < 4 ? vargs[argIdx++] : nullptr;
            if (type == '@') res += GetNSString(arg);
            else if (type == 'd' || type == 'i') res += std::to_string((int)(uintptr_t)arg);
            else if (type == 's') res += arg ? (const char*)arg : "null";
            else { res += "%"; res += type; }
            i++;
        } else res += fmt[i];
    }
    return CreateNSString(res);
}

extern "C" void* wrap_CFStringCreateWithSubstring(void* alloc, void* str, CFRange range) {
    std::string s = GetNSString(str);
    if (range.location >= 0 && range.location + range.length <= (int)s.length()) {
        return CreateNSString(s.substr(range.location, range.length));
    }
    return CreateNSString("");
}

extern "C" CFRange wrap_CFStringFind(void* theString, void* stringToFind, uint32_t compareOptions) {
    std::string s = GetNSString(theString);
    std::string f = GetNSString(stringToFind);
    if (compareOptions & 1) { // kCFCompareCaseInsensitive
        for(auto& c : s) c = tolower(c);
        for(auto& c : f) c = tolower(c);
    }
    size_t pos = s.find(f);
    if (pos != std::string::npos) return {(int)pos, (int)f.length()};
    return {-1, 0}; // kCFNotFound
}

extern "C" uint8_t wrap_CFStringHasPrefix(void* theString, void* prefix) {
    std::string s = GetNSString(theString);
    std::string p = GetNSString(prefix);
    if (p.empty()) return 1;
    return (s.find(p) == 0) ? 1 : 0;
}

extern "C" uint8_t wrap_CFStringHasSuffix(void* theString, void* suffix) {
    std::string s = GetNSString(theString);
    std::string suf = GetNSString(suffix);
    if (suf.empty() || s.length() < suf.length()) return 0;
    return (s.compare(s.length() - suf.length(), suf.length(), suf) == 0) ? 1 : 0;
}

extern "C" void* wrap_CFURLCreateStringByReplacingPercentEscapesUsingEncoding(void* alloc, void* origString, void* charsToLeaveEscaped, uint32_t enc) {
    return origString;
}

extern "C" void* wrap_CFStringCreateWithCString(void* alloc, const char* cStr, uint32_t encoding) { return CreateNSString(cStr ? cStr : ""); }
extern "C" uint32_t wrap_CFStringGetLength(void* cfStr) { return GetNSString(cfStr).length(); }

std::map<void*, std::string> g_cfStringCache;
extern "C" const char* wrap_CFStringGetCStringPtr(void* cfStr, uint32_t encoding) { 
    if (!cfStr) return nullptr;
    g_cfStringCache[cfStr] = GetNSString(cfStr);
    return g_cfStringCache[cfStr].c_str(); 
}
extern "C" uint8_t wrap_CFStringGetCString(void* cfStr, char* buffer, int bufferSize, uint32_t encoding) {
    if (!cfStr || !buffer || bufferSize <= 0) return 0;
    std::string s = GetNSString(cfStr);
    if (s.length() >= bufferSize) return 0;
    strcpy(buffer, s.c_str());
    if (s.find("pngConf") != std::string::npos || s.find("jungle") != std::string::npos) {
        LogToJava("C-API-DEBUG: [CFStringGetCString] Экспорт строки в буфер: [" + s + "]");
    }
    return 1;
}
extern "C" void* wrap_CFArrayCreate(void* alloc, const void** values, uint32_t numValues, void* callbacks) {
    uint32_t* arrInst = (uint32_t*)calloc(1, 32);
    arrInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray");
    if (values && numValues > 0) {
        for (uint32_t i = 0; i < numValues; i++) {
            g_arrays[arrInst].push_back((void*)values[i]);
        }
    }
    return arrInst;
}
extern "C" void* wrap_CFArrayCreateMutable(void* alloc, uint32_t capacity, void* callbacks) { uint32_t* i = (uint32_t*)calloc(1, 32); i[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSArray"); return i; }
extern "C" uint32_t wrap_CFArrayGetCount(void* array) { return g_arrays[array].size(); }
extern "C" void* wrap_CFArrayGetValueAtIndex(void* array, uint32_t idx) { return idx < g_arrays[array].size() ? g_arrays[array][idx] : nullptr; }
extern "C" void wrap_CFArrayAppendValue(void* array, void* val) { g_arrays[array].push_back(val); }
extern "C" void wrap_CFArrayRemoveAllValues(void* array) { g_arrays[array].clear(); }
extern "C" void* wrap_CFDictionaryCreateMutable(void* alloc, uint32_t capacity, void* keyCB, void* valCB) { uint32_t* i = (uint32_t*)calloc(1, 32); i[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary"); return i; }
extern "C" void* wrap_CFDictionaryGetValue(void* dict, void* key) { return g_dictionaries[dict][key]; }
extern "C" void wrap_CFDictionarySetValue(void* dict, void* key, void* val) { g_dictionaries[dict][key] = val; }
extern "C" void wrap_CFDictionaryRemoveValue(void* dict, void* key) { g_dictionaries[dict].erase(key); }
extern "C" void* wrap_CFBundleGetInfoDictionary(void* bundle) {
    uint32_t* dictInst = (uint32_t*)calloc(1, 32);
    dictInst[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_NSDictionary");
    g_dictionariesHLE[dictInst]["CFBundleIdentifier"] = CreateNSString("com.damnwrapper");
    g_dictionariesHLE[dictInst]["CFBundleVersion"] = CreateNSString("1.0");
    g_dictionariesHLE[dictInst]["CFBundleShortVersionString"] = CreateNSString("1.0");
    return dictInst;
}
extern "C" void* wrap_CFBundleGetMainBundle() { return (void*)ResolveSymbol("OBJC_CLASS_$_NSBundle"); }
extern "C" void* wrap_CFBundleGetIdentifier(void* bundle) { return CreateNSString("com.damnwrapper"); }
extern "C" void* wrap_CFBundleGetValueForInfoDictionaryKey(void* bundle, void* key) { return CreateNSString("1.0"); }
extern "C" void* wrap_CFBundleCopyResourcesDirectoryURL(void* bundle) {
    void* nsPath = CreateNSString(g_appBundlePath);
    uint32_t* urlInst = (uint32_t*)calloc(1, 16);
    urlInst[0] = g_hleClasses.count("NSURL") ? (uint32_t)g_hleClasses["NSURL"] : 0xDEADBEEF;
    urlInst[1] = (uint32_t)nsPath;
    return (void*)urlInst;
}
extern "C" void* wrap_CFURLCopyFileSystemPath(void* url, int pathStyle) {
    if (!url) return nullptr;
    uint32_t* urlInst = (uint32_t*)url;
    return urlInst[1] ? (void*)urlInst[1] : CreateNSString(g_appBundlePath);
}
extern "C" uint8_t wrap_CFURLGetFileSystemRepresentation(void* url, uint8_t resolveAgainstBase, uint8_t* buffer, int maxBufLen) {
    if (!url || !buffer || maxBufLen <= 0) return 0;
    uint32_t* urlInst = (uint32_t*)url;
    std::string path = urlInst[1] ? GetNSString((void*)urlInst[1]) : g_appBundlePath;
    if (path.length() >= (size_t)maxBufLen) return 0;
    strcpy((char*)buffer, path.c_str());
    return 1;
}
extern "C" void* wrap_CFRunLoopGetCurrent() { return (void*)1; }
extern "C" void* wrap_CFRunLoopGetMain() { return (void*)1; }
extern "C" void* wrap_CFUUIDCreate(void* alloc) { return (void*)1; }
extern "C" void* wrap_CFUUIDCreateString(void* alloc, void* uuid) { return CreateNSString("00000000-0000-0000-0000-000000000000"); }
extern "C" void* wrap_CFTimeZoneCopySystem() { return (void*)1; }
extern "C" void* wrap_CFTimeZoneCopyDefault() { return (void*)1; }
struct CFGregorianDate { int32_t year; int8_t month; int8_t day; int8_t hour; int8_t minute; double second; };

extern "C" void* wrap_CFTimeZoneGetName(void* tz) { return CreateNSString("GMT"); }
extern "C" double wrap_CFTimeZoneGetSecondsFromGMT(void* tz) { return 0.0; }
extern "C" CFGregorianDate wrap_CFAbsoluteTimeGetGregorianDate(double at, void* tz) {
    time_t unixTime = (time_t)(at + 978307200.0); // Сдвиг от эпохи Apple (2001) к Unix (1970)
    struct tm* tm_info = gmtime(&unixTime);
    CFGregorianDate date = {0};
    if (tm_info) {
        date.year = tm_info->tm_year + 1900;
        date.month = tm_info->tm_mon + 1;
        date.day = tm_info->tm_mday;
        date.hour = tm_info->tm_hour;
        date.minute = tm_info->tm_min;
        date.second = tm_info->tm_sec;
    }
    return date;
}

extern "C" int32_t wrap_CFAbsoluteTimeGetDayOfWeek(double at, void* tz) {
    time_t unixTime = (time_t)(at + 978307200.0);
    struct tm* tm_info = gmtime(&unixTime);
    if (tm_info) {
        int wday = tm_info->tm_wday; // В C: 0 = Воскресенье... 6 = Суббота
        return (wday == 0) ? 7 : wday; // В CoreFoundation (Apple): 1 = Понедельник... 7 = Воскресенье
    }
    return 1;
}

extern "C" unsigned char* wrap_CC_MD5(const void *data, uint32_t len, unsigned char *md) { return md; }
extern "C" unsigned char* wrap_CC_SHA1(const void *data, uint32_t len, unsigned char *md) { return md; }
extern "C" unsigned char* wrap_CC_SHA256(const void *data, uint32_t len, unsigned char *md) { return md; }

struct HLE_CCCryptor {
    uint32_t op;
    uint32_t alg;
    uint32_t options;
};

extern "C" int wrap_CCCryptorCreate(uint32_t op, uint32_t alg, uint32_t options,
                                    const void *key, size_t keyLength, const void *iv,
                                    void **cryptorRef) {
    LogToJava("C-API-TRACE: [CCCryptorCreate] op=" + std::to_string(op) + " alg=" + std::to_string(alg));
    if (cryptorRef) {
        HLE_CCCryptor* cryptor = new HLE_CCCryptor();
        cryptor->op = op;
        cryptor->alg = alg;
        cryptor->options = options;
        *cryptorRef = cryptor;
    }
    return 0; // kCCSuccess
}

extern "C" int wrap_CCCryptorUpdate(void *cryptorRef, const void *dataIn, size_t dataInLength,
                                    void *dataOut, size_t dataOutAvailable, size_t *dataOutMoved) {
    size_t moved = 0;
    if (dataIn && dataOut && dataOutAvailable >= dataInLength) {
        memcpy(dataOut, dataIn, dataInLength);
        moved = dataInLength;
    } else if (dataIn && dataOut) {
        memcpy(dataOut, dataIn, dataOutAvailable);
        moved = dataOutAvailable;
    }
    if (dataOutMoved) *dataOutMoved = moved;
    return 0; // kCCSuccess
}

extern "C" int wrap_CCCryptorFinal(void *cryptorRef, void *dataOut, size_t dataOutAvailable, size_t *dataOutMoved) {
    if (dataOutMoved) *dataOutMoved = 0;
    return 0; // kCCSuccess
}

extern "C" int wrap_CCCryptorRelease(void *cryptorRef) {
    if (cryptorRef) delete (HLE_CCCryptor*)cryptorRef;
    return 0; // kCCSuccess
}

extern "C" size_t wrap_CCCryptorGetOutputLength(void *cryptorRef, size_t inputLength, bool final) {
    // Режим passthrough - длина на выходе всегда равна длине на входе
    return inputLength;
}

extern "C" int wrap_CCCrypt(uint32_t op, uint32_t alg, uint32_t options,
                            const void *key, size_t keyLength, const void *iv,
                            const void *dataIn, size_t dataInLength,
                            void *dataOut, size_t dataOutAvailable, size_t *dataOutMoved) {
    LogToJava("C-API-TRACE: [CCCrypt] op=" + std::to_string(op) + " alg=" + std::to_string(alg) + " len=" + std::to_string(dataInLength));
    // ВНИМАНИЕ: Полноценный AES/DES требует криптобиблиотеки (OpenSSL/mbedTLS) или JNI (javax.crypto.Cipher).
    // Без них делаем passthrough-копирование, чтобы игра не читала мусор из dataOut и не крашилась.
    size_t moved = 0;
    if (dataIn && dataOut && dataOutAvailable >= dataInLength) {
        memcpy(dataOut, dataIn, dataInLength);
        moved = dataInLength;
    } else if (dataIn && dataOut) {
        memcpy(dataOut, dataIn, dataOutAvailable);
        moved = dataOutAvailable;
    }
    if (dataOutMoved) *dataOutMoved = moved;
    return 0; // kCCSuccess
}
extern "C" void* wrap_AudioComponentFindNext(void* inComponent, void* inDesc) { return (void*)1; }
extern "C" int wrap_AudioComponentInstanceNew(void* inComponent, void** outInstance) { if(outInstance) *outInstance = (void*)1; return 0; }
extern "C" int wrap_AudioServicesCreateSystemSoundID(void* url, uint32_t* outSoundID) {
    if (!url || !outSoundID) return -50;
    uint32_t* urlInst = (uint32_t*)url;
    std::string path = urlInst[1] ? GetNSString((void*)urlInst[1]) : "";
    if (!path.empty()) {
        static uint32_t nextSoundID = 1000;
        *outSoundID = nextSoundID++;
        AudioInitToJava((void*)*outSoundID, path);
        LogToJava("HLE: AudioServicesCreateSystemSoundID: " + path + " -> ID: " + std::to_string(*outSoundID));
        return 0;
    }
    return -50;
}
extern "C" void wrap_AudioServicesPlaySystemSound(uint32_t soundID) {
    if (soundID >= 1000) {
        AudioSetCurrentTimeToJava((void*)soundID, 0.0f);
        AudioPlayToJava((void*)soundID);
    }
}
extern "C" int wrap_AudioServicesDisposeSystemSoundID(uint32_t soundID) {
    if (soundID >= 1000) AudioReleaseToJava((void*)soundID);
    return 0;
}

std::vector<void*> g_uiGraphicsContextStack;

extern "C" void wrap_UIGraphicsPushContext(void* context) {
    if (context) g_uiGraphicsContextStack.push_back(context);
}

extern "C" void wrap_UIGraphicsPopContext() {
    if (!g_uiGraphicsContextStack.empty()) g_uiGraphicsContextStack.pop_back();
}

extern "C" void* wrap_UIGraphicsGetCurrentContext() {
    if (!g_uiGraphicsContextStack.empty()) return g_uiGraphicsContextStack.back();
    return nullptr;
}

// Предварительные объявления для Core Graphics
extern "C" void* wrap_CGBitmapContextCreate(void* data, size_t width, size_t height, size_t bitsPerComponent, size_t bytesPerRow, void* space, uint32_t bitmapInfo);
extern "C" void wrap_CGContextRelease(void* c);
extern "C" void* wrap_CGBitmapContextCreateImage(void* c);

extern "C" void wrap_UIGraphicsBeginImageContext(CGSize size) {
    void* ctx = wrap_CGBitmapContextCreate(nullptr, (size_t)size.width, (size_t)size.height, 8, (size_t)(size.width * 4), nullptr, 0);
    wrap_UIGraphicsPushContext(ctx);
}

extern "C" void wrap_UIGraphicsEndImageContext() {
    void* ctx = wrap_UIGraphicsGetCurrentContext();
    wrap_UIGraphicsPopContext();
    wrap_CGContextRelease(ctx);
}

extern "C" void* wrap_UIGraphicsGetImageFromCurrentImageContext() {
    void* ctx = wrap_UIGraphicsGetCurrentContext();
    if (!ctx) return nullptr;
    void* cgImage = wrap_CGBitmapContextCreateImage(ctx);
    if (!cgImage) return nullptr;
    uint32_t* inst = (uint32_t*)calloc(1, 32);
    inst[0] = g_hleClasses.count("UIImage") ? (uint32_t)g_hleClasses["UIImage"] : 0xDEADBEEF;
    inst[1] = (uint32_t)cgImage;
    return inst;
}

extern "C" void* wrap_NSAllocateObject(void* aClass, uint32_t extraBytes, void* zone) {
    if (!aClass || (uintptr_t)aClass < 0x1000) return nullptr;
    uint32_t* cls = (uint32_t*)aClass;
    uint32_t data_ptr = cls[4] & ~3;
    uint32_t instance_size = 32;
    if (data_ptr > 0x1000) {
        uint32_t parsed_size = ((uint32_t*)data_ptr)[2];
        if (parsed_size > 0 && parsed_size < 100000) instance_size = parsed_size;
    }
    void* instance = calloc(1, instance_size + extraBytes);
    ((uint32_t*)instance)[0] = (uint32_t)aClass;
    return instance;
}

extern "C" void* wrap_NSStringFromClass(void* cls) {
    if (!cls) return nullptr;
    return CreateNSString(GetObjCClassName(cls));
}

extern "C" void* wrap_NSStringFromSelector(void* sel) {
    if (!sel) return nullptr;
    return CreateNSString((const char*)sel);
}

extern "C" void wrap_CGContextFillRect(void* c, CGRect rect);

extern "C" void wrap_UIRectFill(CGRect rect) {
    void* ctx = wrap_UIGraphicsGetCurrentContext();
    if (ctx) wrap_CGContextFillRect(ctx, rect);
}

extern "C" void* wrap_UIImagePNGRepresentation(void* uiImage) {
    if (!uiImage) return nullptr;
    uint32_t* ptr = (uint32_t*)uiImage;
    HLE_CGImage* cgImg = (HLE_CGImage*)ptr[1];
    if (!cgImg || !cgImg->data) return nullptr;

    int len = 0;
    unsigned char* png_data = stbi_write_png_to_mem((const unsigned char*)cgImg->data, cgImg->width * 4, cgImg->width, cgImg->height, 4, &len);
    
    if (!png_data || len <= 0) return nullptr;

    void* nsData = wrap_CFDataCreate(nullptr, png_data, len);
    STBIW_FREE(png_data);
    return nsData;
}

extern "C" void wrap_NSLogv(void* fmt, va_list args) {
    if (g_disableLogging) return;
    if (!fmt) return;
    std::string format = GetNSString(fmt);
    std::string res = "";
    for (size_t i = 0; i < format.length(); i++) {
        if (format[i] == '%' && i + 1 < format.length()) {
            char type = format[i+1];
            if (type == '@') { res += GetNSString(va_arg(args, void*)); }
            else if (type == 'd' || type == 'i' || type == 'c') { 
                int arg = va_arg(args, int); 
                if (type == 'c') res += (char)arg; else res += std::to_string(arg); 
            }
            else if (type == 's') { const char* arg = va_arg(args, const char*); res += arg ? arg : "(null)"; }
            else if (type == 'f' || type == 'g') { res += std::to_string(va_arg(args, double)); }
            else if (type == 'x' || type == 'X' || type == 'p') { 
                char hex[32]; snprintf(hex, sizeof(hex), "%p", va_arg(args, void*)); res += hex; 
            }
            else { res += "%"; res += type; }
            i++;
        } else res += format[i];
    }
    LogToJava("NSLog: " + res);
}

extern "C" void Stub_NSLog(void* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    wrap_NSLogv(fmt, args);
    va_end(args);
}
extern "C" void Stub_GenericUnimplemented() { LogToJava("КРИТИЧЕСКАЯ ОШИБКА: Прыжок в Stub_GenericUnimplemented! (Вызов отсутствующей C-API)"); __builtin_trap(); }

extern "C" int Stub_UIApplicationMain(int argc, char *argv[], void* principalClassName, void* delegateClassName) {
    std::string pName = GetNSString(principalClassName);
    std::string dName = GetNSString(delegateClassName);
    LogToJava("HLE_CALL: UIApplicationMain(principal: " + pName + ", delegate: " + dName + ") Запуск...");
    g_iosMainThread = pthread_self();

    static bool init_funcs_run = false;
    if (!init_funcs_run) {
        init_funcs_run = true;
        LogToJava("HLE: Выполнение C++ статических конструкторов (__mod_init_func)... (" + std::to_string(g_initFuncs.size()) + " функций)");
        for (uint32_t func_addr : g_initFuncs) {
            typedef void (*InitFunc)();
            ((InitFunc)func_addr)();
        }
        LogToJava("HLE: C++ статические конструкторы выполнены.");
    }
    
    uint32_t appDelClassAddr = 0;
    if (!dName.empty() && dName != "Unknown") {
        appDelClassAddr = (uint32_t)ResolveSymbol("OBJC_CLASS_$_" + dName);
    }
    
    if (!appDelClassAddr) {
        LogToJava("HLE: Делегат явно не передан (возможно NIB). Ищем класс с методами главного делегата...");
        for (auto const& pair : g_appSymbols) {
            if (pair.first.find("_OBJC_CLASS_$_") == 0) {
                uint32_t testClassAddr = pair.second;
                // Проверяем наличие методов, которые есть только у главного делегата
                if (FindMethodIMP(testClassAddr, "application:didFinishLaunchingWithOptions:") || 
                    FindMethodIMP(testClassAddr, "applicationDidFinishLaunching:")) {
                    appDelClassAddr = testClassAddr;
                    LogToJava("HLE: Автоматически найден главный AppDelegate (по наличию методов): " + pair.first);
                    break;
                }
            }
        }
        
        // Фолбек: если методы не нашлись напрямую в классе, откатываемся к поиску по имени
        if (!appDelClassAddr) {
            for (auto const& pair : g_appSymbols) {
                if (pair.first.find("_OBJC_CLASS_$_") == 0 && pair.first.find("Delegate") != std::string::npos) {
                    appDelClassAddr = pair.second;
                    LogToJava("HLE: Фолбек. Найден вероятный AppDelegate по имени: " + pair.first);
                    break;
                }
            }
        }
    }
    
    if (!appDelClassAddr) {
        LogToJava("HLE КРИТИЧЕСКАЯ ОШИБКА: Класс AppDelegate не найден! Игра скорее всего повиснет.");
        appDelClassAddr = (uint32_t)ResolveSymbol("OBJC_CLASS_$_AppDelegate");
    }

    uint32_t* dummyCoder = (uint32_t*)calloc(1, 32);
    dummyCoder[0] = g_hleClasses.count("NSCoder") ? (uint32_t)g_hleClasses["NSCoder"] : 0xDEADBEEF;

    if (appDelClassAddr) {
        void* appDel = (void*)Stub_objc_msgSend((void*)appDelClassAddr, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (FindMethodIMP(appDelClassAddr, "initWithCoder:")) {
            appDel = (void*)Stub_objc_msgSend(appDel, "initWithCoder:", dummyCoder, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else {
            appDel = (void*)Stub_objc_msgSend(appDel, "init", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        g_appDelegateInstance = appDel; 
        
        uint32_t appDelIsa = ((uint32_t*)appDel)[0];

        if (FindMethodIMP(appDelIsa, "awakeFromNib")) {
            LogToJava("HLE: Calling awakeFromNib for AppDelegate");
            Stub_objc_msgSend(appDel, "awakeFromNib", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        
        void* window = (void*)Stub_objc_msgSend((void*)ResolveSymbol("OBJC_CLASS_$_UIWindow"), "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        float wx = 0, wy = 0, ww = (float)g_surfaceWidth, wh = (float)g_surfaceHeight;
        uint32_t pwx, pwy, pww, pwh; memcpy(&pwx, &wx, 4); memcpy(&pwy, &wy, 4); memcpy(&pww, &ww, 4); memcpy(&pwh, &wh, 4);
        window = (void*)Stub_objc_msgSend(window, "initWithFrame:", (void*)(uintptr_t)pwx, (void*)(uintptr_t)pwy, (void*)(uintptr_t)pww, (void*)(uintptr_t)pwh, nullptr, nullptr, nullptr, nullptr);
        if (FindMethodIMP(appDelIsa, "setWindow:")) {
            Stub_objc_msgSend(appDel, "setWindow:", window, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        
        uint32_t vcClassAddr = 0;
        for (auto const& pair : g_appSymbols) {
            if (pair.first.find("_OBJC_CLASS_$_") == 0 && pair.first.find("ViewController") != std::string::npos) {
                if (FindMethodIMP(pair.second, "startAnimation")) {
                    vcClassAddr = pair.second; break;
                }
            }
        }
        if (!vcClassAddr) {
            for (auto const& pair : g_appSymbols) {
                if (pair.first.find("_OBJC_CLASS_$_") == 0 && pair.first.find("ViewController") != std::string::npos) {
                    vcClassAddr = pair.second; break; 
                }
            }
        }
        if (vcClassAddr) {
            void* vc = (void*)Stub_objc_msgSend((void*)vcClassAddr, "alloc", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            if (FindMethodIMP(vcClassAddr, "initWithCoder:")) {
                vc = (void*)Stub_objc_msgSend(vc, "initWithCoder:", dummyCoder, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            } else if (FindMethodIMP(vcClassAddr, "initWithNibName:bundle:")) {
                vc = (void*)Stub_objc_msgSend(vc, "initWithNibName:bundle:", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            } else {
                vc = (void*)Stub_objc_msgSend(vc, "init", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }

            uint32_t vcIsa = ((uint32_t*)vc)[0];
            if (FindMethodIMP(vcIsa, "awakeFromNib")) {
                LogToJava("HLE: Calling awakeFromNib for ViewController");
                Stub_objc_msgSend(vc, "awakeFromNib", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }

            if (FindMethodIMP(appDelIsa, "setViewController:")) {
                Stub_objc_msgSend(appDel, "setViewController:", vc, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            }
            Stub_objc_msgSend(window, "setRootViewController:", vc, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        
        void* uiApp = (void*)Stub_objc_msgSend((void*)ResolveSymbol("OBJC_CLASS_$_UIApplication"), "sharedApplication", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        
        if (FindMethodIMP(appDelIsa, "application:didFinishLaunchingWithOptions:")) {
            LogToJava("HLE: Вызов application:didFinishLaunchingWithOptions:...");
            Stub_objc_msgSend(appDel, "application:didFinishLaunchingWithOptions:", uiApp, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else if (FindMethodIMP(appDelIsa, "applicationDidFinishLaunching:")) {
            LogToJava("HLE: Вызов applicationDidFinishLaunching:...");
            Stub_objc_msgSend(appDel, "applicationDidFinishLaunching:", uiApp, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else {
            LogToJava("HLE: ВНИМАНИЕ! Метод запуска делегата не найден!");
        }

        LogToJava("HLE: Имитация перехода приложения в активный режим...");
        if (FindMethodIMP(appDelIsa, "applicationDidBecomeActive:")) {
            LogToJava("HLE: Вызов applicationDidBecomeActive:...");
            Stub_objc_msgSend(appDel, "applicationDidBecomeActive:", uiApp, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        
        // Отправляем нотификации (некоторые движки подписываются на них вместо делегата)
        uint32_t* actNotifInst = (uint32_t*)calloc(1, 32);
        actNotifInst[0] = g_hleClasses.count("NSNotification") ? (uint32_t)g_hleClasses["NSNotification"] : 0xDEADBEEF;
        actNotifInst[1] = (uint32_t)CreateNSString("UIApplicationDidBecomeActiveNotification");
        actNotifInst[2] = (uint32_t)uiApp; // UIApplication instance
        for (auto const& pair : g_notifications) {
            if (pair.first == "UIApplicationDidBecomeActiveNotification") {
                for (auto& obs : pair.second) {
                    LogToJava("HLE: Отправка UIApplicationDidBecomeActiveNotification -> " + obs.selector);
                    Stub_objc_msgSend(obs.observer, obs.selector.c_str(), (void*)actNotifInst, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                }
            }
        }
    }
    uint32_t* realFakeLink = (uint32_t*)calloc(1, 32); realFakeLink[0] = (uint32_t)ResolveSymbol("OBJC_CLASS_$_CADisplayLink");
    while (true) {
        // ПОТОКОБЕЗОПАСНАЯ ОБРАБОТКА ИВЕНТОВ ВИДЕО
        pthread_mutex_lock(&g_videoMutex);
        if (!g_pendingVideoFinishes.empty()) {
            std::vector<void*> finishes = g_pendingVideoFinishes;
            g_pendingVideoFinishes.clear();
            pthread_mutex_unlock(&g_videoMutex);
            
            for (void* player : finishes) {
                LogToJava("HLE: Обработка завершения видео для плеера 0x" + std::to_string((uintptr_t)player));
                bool called = false;
                
                uint32_t* notifInst = (uint32_t*)calloc(1, 32);
                notifInst[0] = g_hleClasses.count("NSNotification") ? (uint32_t)g_hleClasses["NSNotification"] : 0xDEADBEEF;
                notifInst[1] = (uint32_t)CreateNSString("MPMoviePlayerPlaybackDidFinishNotification");
                notifInst[2] = (uint32_t)player;
                
                for (auto const& pair : g_notifications) {
                    for (auto& obs : pair.second) {
                        if (obs.object == player || pair.first == "MPMoviePlayerPlaybackDidFinishNotification") {
                            LogToJava("HLE-NOTIF-TRACE: Уведомление: " + pair.first + " для плеера 0x" + std::to_string((uintptr_t)player) + " -> Observer: 0x" + std::to_string((uintptr_t)obs.observer) + " sel: " + obs.selector);
                            Stub_objc_msgSend(obs.observer, obs.selector.c_str(), (void*)notifInst, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                            called = true;
                        }
                    }
                }
                if (!called) LogToJava("HLE: ВНИМАНИЕ! Не найдено ни одного обсервера для этого видео!");
            }
        } else {
            pthread_mutex_unlock(&g_videoMutex);
        }

        pthread_mutex_lock(&g_mainQueueMutex);
        auto queueCopy = g_mainQueue;
        g_mainQueue.clear();
        pthread_mutex_unlock(&g_mainQueueMutex);
        for (auto& item : queueCopy) {
            Stub_objc_msgSend(item.target, item.sel, item.arg, item.arg2, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        }

        if (g_renderingStarted && g_displayLinkTarget && g_displayLinkSelector) {
            static int dl_ticks = 0;
            if (dl_ticks++ % 60 == 0) LogToJava("[MAIN-LOOP] Вызов DisplayLink: target=" + GetObjCClassName(g_displayLinkTarget) + " sel=" + std::string(g_displayLinkSelector));
            Stub_objc_msgSend(g_displayLinkTarget, g_displayLinkSelector, realFakeLink, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else {
            static int idle_ticks = 0;
            if (idle_ticks++ % 60 == 0) LogToJava("[MAIN-LOOP] Крутимся в IDLE_LOOP. g_renderingStarted=" + std::to_string(g_renderingStarted) + " target_set=" + std::to_string(g_displayLinkTarget != nullptr));
            SyncLog("\n[ABSOLUTE-IDLE-LOOP] --- FRAME START ---");
            g_frameHasDraw = false;
            SyncLog("[ABSOLUTE-IDLE-LOOP] 1. Checking Context...");
            EGLContext currentCtx = eglGetCurrentContext();
            EGLDisplay currentDpy = eglGetCurrentDisplay();
            EGLSurface currentSurf = eglGetCurrentSurface(EGL_DRAW);
            
            if (currentCtx == EGL_NO_CONTEXT) {
                SyncLog("[ABSOLUTE-IDLE-LOOP] WARNING: Context lost in idle loop! Re-binding...");
                eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext);
                currentSurf = eglGetCurrentSurface(EGL_DRAW);
            }

            EGLint surfW = -1, surfH = -1;
            if (currentSurf != EGL_NO_SURFACE) {
                eglQuerySurface(currentDpy, currentSurf, EGL_WIDTH, &surfW);
                eglQuerySurface(currentDpy, currentSurf, EGL_HEIGHT, &surfH);
            }
            SyncLog("[ABSOLUTE-IDLE-LOOP] 1.1 Surface Info: W=" + std::to_string(surfW) + " H=" + std::to_string(surfH));

            if (surfW <= 0 || surfH <= 0) {
                SyncLog("[ABSOLUTE-IDLE-LOOP] WARNING: Surface size is invalid (" + std::to_string(surfW) + "x" + std::to_string(surfH) + "). Skipping frame to avoid crash.");
                usleep(16000);
                continue;
            }

            SyncLog("[ABSOLUTE-IDLE-LOOP] 2. Entering MegaDebug_glClear...");
            MegaDebug_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            SyncLog("[ABSOLUTE-IDLE-LOOP] 3. Entering RenderHLEUI...");
            RenderHLEUI();
            
            SyncLog("[ABSOLUTE-IDLE-LOOP] 4. Entering MegaDebug_eglSwapBuffers...");
            MegaDebug_eglSwapBuffers(g_eglDisplay, g_eglSurface);
            SyncLog("[ABSOLUTE-IDLE-LOOP] --- FRAME END ---\n");
        }
        usleep(16000); 
    }
    return 0;
}


uint32_t hle_stack_chk_guard_val = 0xdeadbeef; 
void* hle_NSDefaultRunLoopMode_ptr = nullptr; 
void* hle_NSRunLoopCommonModes_ptr = nullptr;
void* hle_NSFileSize_ptr = nullptr;
void* hle_NSGenericException_ptr = nullptr;
void* hle_NSInternalInconsistencyException_ptr = nullptr;
void* hle_NSLocalizedRecoverySuggestionErrorKey_ptr = nullptr;
void* hle_UIApplicationDidChangeStatusBarFrameNotification_ptr = nullptr;
void* hle_NSURLAuthenticationMethodClientCertificate_ptr = nullptr;
void* hle_NSURLAuthenticationMethodServerTrust_ptr = nullptr;
void* hle_NSURLErrorDomain_ptr = nullptr;
void* hle_NSURLErrorFailingURLErrorKey_ptr = nullptr;
void* hle_MPMovieDurationAvailableNotification_ptr = nullptr;
void* hle_GCControllerDidConnectNotification_ptr = nullptr;
void* hle_GCControllerDidDisconnectNotification_ptr = nullptr;
void* hle_UILocalNotificationDefaultSoundName_ptr = nullptr;
void* hle_kEAGLColorFormatRGBA8_ptr = nullptr; 
void* hle_kEAGLColorFormatRGB565_ptr = nullptr;
void* hle_kEAGLDrawablePropertyColorFormat_ptr = nullptr; 
void* hle_kEAGLDrawablePropertyRetainedBacking_ptr = nullptr;
void* hle_AVAudioSessionCategoryPlayback_ptr = nullptr;
void* hle_AVAudioSessionCategoryAmbient_ptr = nullptr;
void* hle_NSUserDefaultsDidChangeNotification_ptr = nullptr;
void* hle_GKPlayerAuthenticationDidChangeNotificationName_ptr = nullptr;
double wrap_kCFCoreFoundationVersionNumber = 478.61;
void* hle_kCFAllocatorDefault = nullptr;
void* hle_kCFAllocatorNull = nullptr;
void* hle_kCFBooleanFalse = nullptr;
void* hle_kCFBooleanTrue = nullptr;
void* hle_kCFErrorDescriptionKey = nullptr;
void* hle_kCFHTTPVersion1_1 = nullptr;
void* hle_kCFPreferencesAnyHost = nullptr;
void* hle_kCFPreferencesCurrentUser = nullptr;
void* hle_kCFStreamPropertyHTTPResponseHeader = nullptr;
void* hle_kCFStreamPropertyHTTPShouldAutoredirect = nullptr;
void* hle_kSecAttrAccessGroup = nullptr;
void* hle_kSecAttrAccount = nullptr;
void* hle_kSecAttrGeneric = nullptr;
void* hle_kSecAttrService = nullptr;
void* hle_kSecClass = nullptr;
void* hle_kSecClassGenericPassword = nullptr;
void* hle_kSecMatchLimit = nullptr;
void* hle_kSecMatchLimitOne = nullptr;
void* hle_kSecReturnAttributes = nullptr;
void* hle_kSecReturnData = nullptr;
void* hle_kSecValueData = nullptr;
void* hle_kCFRunLoopDefaultMode = nullptr;
void* hle_kCFRunLoopCommonModes = nullptr;
void* hle_kCFTypeArrayCallBacks = nullptr;
void* hle_kCFTypeDictionaryKeyCallBacks = nullptr;
void* hle_kCFTypeDictionaryValueCallBacks = nullptr;
void* hle_NSFileModificationDate_ptr = nullptr;
void* hle_NSNetServicesErrorCode_ptr = nullptr;
void* hle_kCFBundleIdentifierKey = nullptr;
void* hle_kCFNumberNaN = nullptr;
void* hle_kCFNumberNegativeInfinity = nullptr;
void* hle_kCFNumberPositiveInfinity = nullptr;
void* hle_ADBannerContentSizeIdentifierLandscape_ptr = nullptr;
void* hle_ADBannerContentSizeIdentifierPortrait_ptr = nullptr;
uint32_t hle_objc_empty_cache = 0;
void* hle_dummy_str_ptr = nullptr;
uint32_t hle_dummy_struct[4] = {0, 0, 0, 0};
uint32_t hle_empty_string_rep[4] = {0, 0, 0xFFFFFFFF, 0}; // length=0, cap=0, ref=-1, data=\0

HLEClass hle_NSConcreteStackBlock_class = {0xDEADBEEF, "__NSConcreteStackBlock"};
HLEClass hle_NSConcreteGlobalBlock_class = {0xDEADBEEF, "__NSConcreteGlobalBlock"};
HLEClass hle_NSConcreteMallocBlock_class = {0xDEADBEEF, "__NSConcreteMallocBlock"};

uint32_t wrap_ZSt4cout[32] = {0};
uint32_t wrap_ZSt4cerr[32] = {0};
uint32_t wrap_ZTVN10__cxxabiv117__class_type_infoE[8] = {0};
uint32_t wrap_ZTVN10__cxxabiv120__si_class_type_infoE[8] = {0};
uint32_t wrap_ZTVN10__cxxabiv121__vmi_class_type_infoE[8] = {0};
uint32_t wrap_ZTVSt15basic_streambufIcSt11char_traitsIcEE[16] = {0};
uint32_t wrap_ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE[16] = {0};
uint32_t wrap_ZTVSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE[16] = {0};
uint32_t wrap_ZTVSt9basic_iosIcSt11char_traitsIcEE[16] = {0};
uint32_t wrap_ZTTSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE[4] = {0};
size_t wrap_ZNSt8numpunctIwE2idE = 0;
uint32_t wrap_ZTISt9bad_alloc[8] = {0};

extern "C" void wrap_abort();
extern "C" void wrap_ZSt16__throw_bad_castv() { LogToJava("C++ EXCEPTION: std::bad_cast"); wrap_abort(); }
extern "C" void wrap_ZSt17__throw_bad_allocv() { LogToJava("C++ EXCEPTION: std::bad_alloc"); wrap_abort(); }
extern "C" void wrap_ZSt19__throw_logic_errorPKc(const char* s) { LogToJava(std::string("C++ EXCEPTION: std::logic_error: ") + (s?s:"")); wrap_abort(); }
extern "C" void wrap_ZSt20__throw_length_errorPKc(const char* s) { LogToJava(std::string("C++ EXCEPTION: std::length_error: ") + (s?s:"")); wrap_abort(); }
extern "C" void wrap_ZSt20__throw_out_of_rangePKc(const char* s) { LogToJava(std::string("C++ EXCEPTION: std::out_of_range: ") + (s?s:"")); wrap_abort(); }

extern "C" void* wrap_Block_copy(const void* aBlock) {
    if (!aBlock) return nullptr;
    uint32_t* block = (uint32_t*)aBlock;
    std::string cName = GetObjCClassName((void*)aBlock);
    if (cName.find("__NSConcreteMallocBlock") != std::string::npos) return (void*)aBlock;

    uint32_t flags = block[1];
    uint32_t* descriptor = (uint32_t*)block[4];
    uint32_t size = descriptor ? descriptor[1] : 0;
    if (size == 0 || size > 1024) size = 64; 

    uint32_t* heapBlock = (uint32_t*)calloc(1, size);
    memcpy(heapBlock, block, size);
    heapBlock[0] = (uint32_t)&hle_NSConcreteMallocBlock_class;

    if (flags & (1 << 25)) { // BLOCK_HAS_COPY_DISPOSE
        typedef void (*CopyHelper)(void*, const void*);
        CopyHelper helper = (CopyHelper)descriptor[2];
        if (helper) helper(heapBlock, block);
    }
    return heapBlock;
}

extern "C" void wrap_Block_release(const void* aBlock) {
    // Оставляем утечку памяти by design для стабильности HLE
}

extern "C" void wrap_Block_object_assign(void *destAddr, const void *object, int flags) {
    if (destAddr) *(void **)destAddr = (void *)object;
}

void* hle_stderrp_ptr = nullptr;
void* hle_stdoutp_ptr = nullptr;

extern "C" uint32_t Stub_ReturnZero() {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava("C-API-STUB: ReturnZero вызван из: " + GetModuleInfoForAddress(lr));
    return 0;
}

extern "C" void* wrap_dynamic_cast(void* src_ptr, void* src_type, void* dst_type, ptrdiff_t src2dst) {
    // Хак: возвращаем оригинальный указатель.
    // Без оригинального RTTI честный каст невозможен. Если в игре начнутся 
    // странные баги с логикой (из-за сломанных проверок is_a), 
    // измените "return src_ptr;" на "return nullptr;"
    return src_ptr;
}
extern "C" void Stub_objc_enumerationMutation(void* obj) { LogToJava("HLE_CALL: objc_enumerationMutation called (ignored)"); }

extern "C" {

    bool wrap_CGRectContainsPoint(CGRect rect, CGPoint point) {
        return (point.x >= rect.origin.x && point.x <= rect.origin.x + rect.size.width &&
                point.y >= rect.origin.y && point.y <= rect.origin.y + rect.size.height);
    }
    float wrap_CGRectGetHeight(CGRect rect) { return rect.size.height; }
    float wrap_CGRectGetWidth(CGRect rect) { return rect.size.width; }
    float wrap_CGRectGetMinX(CGRect rect) { return rect.origin.x; }
    float wrap_CGRectGetMinY(CGRect rect) { return rect.origin.y; }
    float wrap_CGRectGetMidX(CGRect rect) { return rect.origin.x + rect.size.width / 2.0f; }
    float wrap_CGRectGetMidY(CGRect rect) { return rect.origin.y + rect.size.height / 2.0f; }
    
    CGRect wrap_CGRectInset(CGRect rect, float dx, float dy) {
        rect.origin.x += dx; rect.origin.y += dy;
        rect.size.width -= dx * 2.0f; rect.size.height -= dy * 2.0f;
        return rect;
    }
    CGRect wrap_CGRectOffset(CGRect rect, float dx, float dy) {
        rect.origin.x += dx; rect.origin.y += dy;
        return rect;
    }
    bool wrap_CGRectIsEmpty(CGRect rect) { return (rect.size.width <= 0.0f || rect.size.height <= 0.0f); }
    bool wrap_CGRectIsNull(CGRect rect) { return std::isinf(rect.origin.x) || std::isinf(rect.origin.y); }
    CGRect wrap_CGRectIntersection(CGRect r1, CGRect r2) {
        float minX = fmaxf(r1.origin.x, r2.origin.x);
        float minY = fmaxf(r1.origin.y, r2.origin.y);
        float maxX = fminf(r1.origin.x + r1.size.width, r2.origin.x + r2.size.width);
        float maxY = fminf(r1.origin.y + r1.size.height, r2.origin.y + r2.size.height);
        if (maxX <= minX || maxY <= minY) return wrap_CGRectNull;
        return {{minX, minY}, {maxX - minX, maxY - minY}};
    }

    CGAffineTransform wrap_CGAffineTransformMakeTranslation(float tx, float ty) { return {1.0f, 0.0f, 0.0f, 1.0f, tx, ty}; }
    CGAffineTransform wrap_CGAffineTransformMakeScale(float sx, float sy) { return {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f}; }
    CGAffineTransform wrap_CGAffineTransformMakeRotation(float angle) {
        float c = std::cos(angle), s = std::sin(angle);
        return {c, s, -s, c, 0.0f, 0.0f};
    }
    CGAffineTransform wrap_CGAffineTransformTranslate(CGAffineTransform t, float tx, float ty) {
        t.tx += t.a * tx + t.c * ty; t.ty += t.b * tx + t.d * ty;
        return t;
    }
    CGAffineTransform wrap_CGAffineTransformScale(CGAffineTransform t, float sx, float sy) {
        t.a *= sx; t.b *= sx; t.c *= sy; t.d *= sy;
        return t;
    }
    CGAffineTransform wrap_CGAffineTransformRotate(CGAffineTransform t, float angle) {
        float c = std::cos(angle), s = std::sin(angle);
        CGAffineTransform res;
        res.a = t.a * c + t.c * s; res.b = t.b * c + t.d * s;
        res.c = t.a * -s + t.c * c; res.d = t.b * -s + t.d * c;
        res.tx = t.tx; res.ty = t.ty;
        return res;
    }
}

// --- CORE GRAPHICS HLE ---

extern "C" void* wrap_CGBitmapContextCreate(void* data, size_t width, size_t height, size_t bitsPerComponent, size_t bytesPerRow, void* space, uint32_t bitmapInfo) {
    HLE_CGContext* ctx = new HLE_CGContext();
    ctx->width = width; ctx->height = height; ctx->bpp = bitsPerComponent;
    ctx->data = data ? data : calloc(1, bytesPerRow * height);
    LogToJava("HLE: CGBitmapContextCreate(" + std::to_string(width) + "x" + std::to_string(height) + ") -> " + std::to_string((uintptr_t)ctx));
    return ctx;
}
extern "C" void* wrap_CGBitmapContextGetData(void* ctx) {
    if (!ctx) return nullptr;
    return ((HLE_CGContext*)ctx)->data;
}
extern "C" void* wrap_CGBitmapContextCreateImage(void* c) {
    if (!c) return nullptr;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    HLE_CGImage* img = new HLE_CGImage{ctx->width, ctx->height, ctx->bpp, ctx->data};
    return img;
}
extern "C" size_t wrap_CGImageGetWidth(void* image) { return image ? ((HLE_CGImage*)image)->width : 0; }
extern "C" size_t wrap_CGImageGetHeight(void* image) { return image ? ((HLE_CGImage*)image)->height : 0; }
extern "C" size_t wrap_CGImageGetBitsPerComponent(void* image) { return image ? ((HLE_CGImage*)image)->bpp : 8; }
extern "C" uint32_t wrap_CGImageGetAlphaInfo(void* image) { return 1; } // CGImageAlphaInfo::kCGImageAlphaPremultipliedLast
extern "C" void* wrap_CGColorSpaceCreateDeviceRGB() {
    uint32_t* cs = (uint32_t*)calloc(1, 4);
    cs[0] = 0xC01086B; // Magic
    return cs;
}
extern "C" void wrap_CGColorSpaceRelease(void* cs) {
    if (cs) free(cs);
}
extern "C" void wrap_CGContextRelease(void* c) {
    if (c) {
        HLE_CGContext* ctx = (HLE_CGContext*)c;
        // Мы не освобождаем ctx->data, т.к. она может быть привязана к внешней памяти, 
        // но освобождаем саму структуру контекста, избегая утечки.
        delete ctx;
    }
}
extern "C" void wrap_CGImageRelease(void* image) { 
    if (image) {
        HLE_CGImage* img = (HLE_CGImage*)image;
        if (img->data) stbi_image_free(img->data);
        delete img;
    }
}

extern "C" void wrap_CGContextDrawImage(void* c, CGRect rect, void* image) {
    if (!c || !image) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    HLE_CGImage* img = (HLE_CGImage*)image;
    if (ctx->data && img->data) {
        int copyW = std::min(ctx->width, img->width);
        int copyH = std::min(ctx->height, img->height);
        for (int y = 0; y < copyH; y++) {
            memcpy((uint8_t*)ctx->data + y * ctx->width * 4, 
                   (uint8_t*)img->data + y * img->width * 4, copyW * 4);
        }
    }
}

void CGDrawPixel(HLE_CGContext* ctx, int x, int y, float* color) {
    if (!ctx || !ctx->data || x < 0 || y < 0 || x >= ctx->width || y >= ctx->height) return;
    uint32_t* buf = (uint32_t*)ctx->data;
    uint8_t r = (uint8_t)(color[0] * 255.0f);
    uint8_t g = (uint8_t)(color[1] * 255.0f);
    uint8_t b = (uint8_t)(color[2] * 255.0f);
    uint32_t bg = buf[y * ctx->width + x];
    uint8_t br = bg & 0xFF, bg_g = (bg >> 8) & 0xFF, bb = (bg >> 16) & 0xFF;
    float alpha = color[3];
    uint8_t fr = (uint8_t)(r * alpha + br * (1.0f - alpha));
    uint8_t fg = (uint8_t)(g * alpha + bg_g * (1.0f - alpha));
    uint8_t fb = (uint8_t)(b * alpha + bb * (1.0f - alpha));
    buf[y * ctx->width + x] = 0xFF000000 | (fb << 16) | (fg << 8) | fr;
}

void CGDrawLine(HLE_CGContext* ctx, float x0, float y0, float x1, float y1, float* color) {
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    int dx = abs(ix1 - ix0), sx = ix0 < ix1 ? 1 : -1;
    int dy = -abs(iy1 - iy0), sy = iy0 < iy1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        CGDrawPixel(ctx, ix0, iy0, color);
        if (ix0 == ix1 && iy0 == iy1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; ix0 += sx; }
        if (e2 <= dx) { err += dx; iy0 += sy; }
    }
}

void CGApplyCTM(HLE_CGContext* ctx, float* x, float* y) {
    float nx = *x * ctx->currentState.ctm.a + *y * ctx->currentState.ctm.c + ctx->currentState.ctm.tx;
    float ny = *x * ctx->currentState.ctm.b + *y * ctx->currentState.ctm.d + ctx->currentState.ctm.ty;
    *x = nx; *y = ny;
}

extern "C" const float* wrap_CGColorGetComponents(void* color) {
    return color ? ((HLE_CGColor*)color)->components : nullptr;
}

extern "C" void* wrap_CGColorGetColorSpace(void* color) {
    static void* dummyColorSpace = nullptr;
    if (!dummyColorSpace) dummyColorSpace = wrap_CGColorSpaceCreateDeviceRGB();
    return dummyColorSpace;
}

extern "C" int wrap_CGColorSpaceGetModel(void* space) {
    return 1; // kCGColorSpaceModelRGB
}

extern "C" void* wrap_CGGradientCreateWithColors(void* space, void* colors, const float* locations) {
    static uint32_t dummyGrad[4] = {0x67726164, 0, 0, 0}; // Статичный указатель, не течет по памяти
    return dummyGrad;
}

extern "C" void wrap_CGContextSaveGState(void* c) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->stateStack.push_back(ctx->currentState);
}

extern "C" void wrap_CGContextRestoreGState(void* c) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    if (!ctx->stateStack.empty()) {
        ctx->currentState = ctx->stateStack.back();
        ctx->stateStack.pop_back();
    }
}

extern "C" void wrap_CGContextScaleCTM(void* c, float sx, float sy) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentState.ctm = wrap_CGAffineTransformScale(ctx->currentState.ctm, sx, sy);
}

extern "C" void wrap_CGContextTranslateCTM(void* c, float tx, float ty) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentState.ctm = wrap_CGAffineTransformTranslate(ctx->currentState.ctm, tx, ty);
}

extern "C" void wrap_CGContextSetFillColor(void* c, const float* components) {
    if (!c || !components) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    memcpy(ctx->currentState.fillColor, components, sizeof(float) * 4);
}

extern "C" void wrap_CGContextSetRGBFillColor(void* c, float r, float g, float b, float a) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentState.fillColor[0] = r; ctx->currentState.fillColor[1] = g; ctx->currentState.fillColor[2] = b; ctx->currentState.fillColor[3] = a;
}

extern "C" void wrap_CGContextSetStrokeColor(void* c, const float* components) {
    if (!c || !components) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    memcpy(ctx->currentState.strokeColor, components, sizeof(float) * 4);
}

extern "C" void wrap_CGContextSetRGBStrokeColor(void* c, float r, float g, float b, float a) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentState.strokeColor[0] = r; ctx->currentState.strokeColor[1] = g; ctx->currentState.strokeColor[2] = b; ctx->currentState.strokeColor[3] = a;
}

extern "C" void wrap_CGContextSetFillColorWithColor(void* c, void* color) {
    if (!c || !color) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    HLE_CGColor* cgColor = (HLE_CGColor*)color;
    memcpy(ctx->currentState.fillColor, cgColor->components, sizeof(float) * 4);
}

extern "C" void wrap_CGContextSetStrokeColorWithColor(void* c, void* color) {
    if (!c || !color) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    HLE_CGColor* cgColor = (HLE_CGColor*)color;
    memcpy(ctx->currentState.strokeColor, cgColor->components, sizeof(float) * 4);
}

extern "C" void wrap_CGContextSetLineWidth(void* c, float width) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentState.lineWidth = width;
}

extern "C" void wrap_CGContextBeginPath(void* c) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentPath.clear();
}

extern "C" void wrap_CGContextClosePath(void* c) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    if (!ctx->currentPath.empty()) {
        float startX = ctx->currentPath[0].points[0];
        float startY = ctx->currentPath[0].points[1];
        ctx->currentPath.push_back({1, {startX, startY, 0, 0, 0, 0}});
        ctx->currentX = startX; ctx->currentY = startY;
    }
}

extern "C" void wrap_CGContextMoveToPoint(void* c, float x, float y) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentPath.push_back({0, {x, y, 0, 0, 0, 0}});
    ctx->currentX = x; ctx->currentY = y;
}

extern "C" void wrap_CGContextAddLineToPoint(void* c, float x, float y) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    ctx->currentPath.push_back({1, {x, y, 0, 0, 0, 0}});
    ctx->currentX = x; ctx->currentY = y;
}

extern "C" void wrap_CGContextAddRect(void* c, CGRect rect) {
    if (!c) return;
    wrap_CGContextMoveToPoint(c, rect.origin.x, rect.origin.y);
    wrap_CGContextAddLineToPoint(c, rect.origin.x + rect.size.width, rect.origin.y);
    wrap_CGContextAddLineToPoint(c, rect.origin.x + rect.size.width, rect.origin.y + rect.size.height);
    wrap_CGContextAddLineToPoint(c, rect.origin.x, rect.origin.y + rect.size.height);
    wrap_CGContextClosePath(c);
}

extern "C" void wrap_CGContextAddArc(void* c, float x, float y, float radius, float startAngle, float endAngle, int clockwise) {
    if (!c) return;
    int segments = 32;
    float step = (endAngle - startAngle) / segments;
    for (int i = 0; i <= segments; i++) {
        float angle = startAngle + i * step;
        float px = x + radius * cosf(angle);
        float py = y + radius * sinf(angle);
        if (i == 0) wrap_CGContextMoveToPoint(c, px, py);
        else wrap_CGContextAddLineToPoint(c, px, py);
    }
}

extern "C" void wrap_CGContextAddArcToPoint(void* c, float x1, float y1, float x2, float y2, float radius) {
    if (!c) return;
    wrap_CGContextAddLineToPoint(c, x1, y1);
}

extern "C" void wrap_CGContextStrokePath(void* c) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    float px = 0, py = 0;
    for (auto& el : ctx->currentPath) {
        float x = el.points[0], y = el.points[1];
        CGApplyCTM(ctx, &x, &y);
        if (el.type == 0) { px = x; py = y; }
        else if (el.type == 1) {
            CGDrawLine(ctx, px, py, x, y, ctx->currentState.strokeColor);
            px = x; py = y;
        }
    }
    ctx->currentPath.clear();
}

extern "C" void wrap_CGContextFillPath(void* c) {
    if (!c) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    if (ctx->currentPath.size() >= 4) {
        float minX = 99999, minY = 99999, maxX = -99999, maxY = -99999;
        for (auto& el : ctx->currentPath) {
            float x = el.points[0], y = el.points[1];
            CGApplyCTM(ctx, &x, &y);
            if (x < minX) minX = x; if (x > maxX) maxX = x;
            if (y < minY) minY = y; if (y > maxY) maxY = y;
        }
        for (int y = (int)minY; y <= (int)maxY; y++) {
            for (int x = (int)minX; x <= (int)maxX; x++) {
                CGDrawPixel(ctx, x, y, ctx->currentState.fillColor);
            }
        }
    }
    ctx->currentPath.clear();
}

extern "C" void wrap_CGContextFillRect(void* c, CGRect rect) {
    if (!c) return;
    wrap_CGContextBeginPath(c);
    wrap_CGContextAddRect(c, rect);
    wrap_CGContextFillPath(c);
}

extern "C" void wrap_CGContextStrokeLineSegments(void* c, const CGPoint* points, size_t count) {
    if (!c || !points) return;
    HLE_CGContext* ctx = (HLE_CGContext*)c;
    for (size_t i = 0; i < count; i += 2) {
        float x0 = points[i].x, y0 = points[i].y;
        float x1 = points[i+1].x, y1 = points[i+1].y;
        CGApplyCTM(ctx, &x0, &y0);
        CGApplyCTM(ctx, &x1, &y1);
        CGDrawLine(ctx, x0, y0, x1, y1, ctx->currentState.strokeColor);
    }
}

extern "C" void wrap_CGContextSetStrokeColorSpace(void* c, void* space) {}

// --- OPENAL HLE ---
static uint32_t g_alIdCounter = 1;
extern "C" void* wrap_alcOpenDevice(const char* devicename) { LogToJava("HLE: alcOpenDevice"); return (void*)1; }
extern "C" void* wrap_alcCreateContext(void* device, const int* attrlist) { LogToJava("HLE: alcCreateContext"); return (void*)1; }
extern "C" uint8_t wrap_alcMakeContextCurrent(void* context) { LogToJava("HLE: alcMakeContextCurrent"); return 1; }
extern "C" void wrap_alGenSources(int n, uint32_t* sources) { for(int i=0; i<n; i++) sources[i] = g_alIdCounter++; }
extern "C" void wrap_alGenBuffers(int n, uint32_t* buffers) { for(int i=0; i<n; i++) buffers[i] = g_alIdCounter++; }

extern "C" void wrap_alSourcei(uint32_t source, int param, int value) {
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "alSourceiJava", "(III)V");
    if (m) env->CallVoidMethod(g_mainActivity, m, (jint)source, (jint)param, (jint)value);
    env->DeleteLocalRef(clazz);
}

extern "C" void wrap_alSourcef(uint32_t source, int param, float value) {
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "alSourcefJava", "(IIF)V");
    if (m) env->CallVoidMethod(g_mainActivity, m, (jint)source, (jint)param, (jfloat)value);
    env->DeleteLocalRef(clazz);
}

extern "C" void wrap_alBufferData(uint32_t buffer, int format, const void* data, int size, int freq) {
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity || !data || size <= 0) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "alBufferDataJava", "(II[BI)V");
    if (m) {
        jbyteArray jData = env->NewByteArray(size);
        if (jData) {
            env->SetByteArrayRegion(jData, 0, size, (const jbyte*)data);
            env->CallVoidMethod(g_mainActivity, m, (jint)buffer, (jint)format, jData, (jint)freq);
            env->DeleteLocalRef(jData);
        } else {
            env->ExceptionClear();
            LogToJava("HLE_AUDIO_ERROR: JNI OOM! Не удалось выделить " + std::to_string(size) + " байт под аудио буфер.");
        }
    }
    env->DeleteLocalRef(clazz);
}


extern "C" void wrap_alSourceQueueBuffers(uint32_t source, int nb, const uint32_t* buffers) {
    // Хак: Если игра ставит буферы в очередь, просто привяжем первый как основной
    if (nb > 0 && buffers) wrap_alSourcei(source, 0x1009, buffers[0]);
}

extern "C" void wrap_alSourcePlay(uint32_t source) {
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "alSourcePlayJava", "(I)V");
    if (m) env->CallVoidMethod(g_mainActivity, m, (jint)source);
    env->DeleteLocalRef(clazz);
}

extern "C" void wrap_alSourceStop(uint32_t source) {
    JNIEnv* env = GetJNIEnv(); if (!env || !g_mainActivity) return;
    jclass clazz = env->GetObjectClass(g_mainActivity);
    jmethodID m = env->GetMethodID(clazz, "alSourceStopJava", "(I)V");
    if (m) env->CallVoidMethod(g_mainActivity, m, (jint)source);
    env->DeleteLocalRef(clazz);
}

extern "C" void wrap_alDeleteBuffers(int n, const uint32_t* buffers) {
    LogToJava("HLE: alDeleteBuffers (" + std::to_string(n) + ")");
}

extern "C" void wrap_alDeleteSources(int n, const uint32_t* sources) {
    if (sources) {
        for (int i = 0; i < n; i++) {
            AudioReleaseToJava((void*)(uintptr_t)sources[i]);
        }
    }
    LogToJava("HLE: alDeleteSources (" + std::to_string(n) + ")");
}

extern "C" void wrap_alGetSourcei(uint32_t source, int param, int* value) {
    if (!value) return;
    if (param == 0x1010) { // AL_SOURCE_STATE
        bool playing = AudioIsPlayingToJava((void*)(uintptr_t)source);
        *value = playing ? 0x1015 : 0x1014; // 0x1015 = AL_PLAYING, 0x1014 = AL_STOPPED
    } else if (param == 0x1009) { // AL_BUFFER
        *value = 0; 
    } else {
        *value = 0;
    }
}

extern "C" void wrap_alListenerf(int param, float value) {
    LogToJava("HLE: alListenerf param=" + std::to_string(param) + " val=" + std::to_string(value));
}

extern "C" void wrap_alSource3f(uint32_t source, int param, float v1, float v2, float v3) {
    LogToJava("HLE: alSource3f source=" + std::to_string(source) + " param=" + std::to_string(param));
}

extern "C" uint8_t wrap_alcCloseDevice(void* device) {
    LogToJava("HLE: alcCloseDevice");
    return 1; // ALC_TRUE
}

extern "C" void wrap_alcDestroyContext(void* context) {
    LogToJava("HLE: alcDestroyContext");
}

extern "C" const char* wrap_alcGetString(void* device, int param) {
    if (param == 0x1004 || param == 0x1005) return "DamnWrapper Audio Device"; // ALC_DEFAULT_DEVICE_SPECIFIER & ALC_DEVICE_SPECIFIER
    if (param == 0x1006) return "ALC_EXT_DEFAULT_DEVICE"; // ALC_EXTENSIONS
    return "";
}

extern "C" void wrap_alcProcessContext(void* context) {}

extern "C" void wrap_alcSuspendContext(void* context) {
    LogToJava("HLE: alcSuspendContext");
}

extern "C" int* wrap___error() { return &errno; }

// --- COMPILER BUILTINS & CXX ABI ---
extern "C" long wrap_sysconf(int name) {
    LogToJava("C-API-TRACE: [sysconf] name=" + std::to_string(name));
    if (name == 58) return 2; // _SC_NPROCESSORS_ONLN
    if (name == 57) return 2; // _SC_NPROCESSORS_CONF
    if (name == 29) return 4096; // _SC_PAGESIZE
    return sysconf(name);
}

extern "C" int wrap_pthread_mutex_init(void* mutex, const void* attr) {
    memset(mutex, 0, 40); // iOS mutex = 40 bytes
    return pthread_mutex_init((pthread_mutex_t*)mutex, nullptr);
}
extern "C" int wrap_pthread_mutex_lock(void* mutex) {
    uint32_t* ptr = (uint32_t*)mutex;
    if (ptr[0] == 0x32AAABA7) { ptr[0] = 0; pthread_mutex_init((pthread_mutex_t*)mutex, nullptr); }
    return pthread_mutex_lock((pthread_mutex_t*)mutex);
}
extern "C" int wrap_pthread_mutex_trylock(void* mutex) {
    uint32_t* ptr = (uint32_t*)mutex;
    if (ptr[0] == 0x32AAABA7) { ptr[0] = 0; pthread_mutex_init((pthread_mutex_t*)mutex, nullptr); }
    return pthread_mutex_trylock((pthread_mutex_t*)mutex);
}
extern "C" int wrap_pthread_mutex_unlock(void* mutex) {
    return pthread_mutex_unlock((pthread_mutex_t*)mutex);
}
extern "C" int wrap_pthread_mutex_destroy(void* mutex) {
    return pthread_mutex_destroy((pthread_mutex_t*)mutex);
}
extern "C" int wrap_pthread_cond_init(void* cond, const void* attr) {
    memset(cond, 0, 24); // iOS cond = 24 bytes
    return pthread_cond_init((pthread_cond_t*)cond, nullptr);
}
extern "C" int wrap_pthread_cond_wait(void* cond, void* mutex) {
    uint32_t* ptr = (uint32_t*)cond;
    if (ptr[0] == 0x3CB0B1BB) { ptr[0] = 0; pthread_cond_init((pthread_cond_t*)cond, nullptr); }
    return pthread_cond_wait((pthread_cond_t*)cond, (pthread_mutex_t*)mutex);
}
extern "C" int wrap_pthread_cond_timedwait(void* cond, void* mutex, const struct timespec* abstime) {
    uint32_t* ptr = (uint32_t*)cond;
    if (ptr[0] == 0x3CB0B1BB) { ptr[0] = 0; pthread_cond_init((pthread_cond_t*)cond, nullptr); }
    return pthread_cond_timedwait((pthread_cond_t*)cond, (pthread_mutex_t*)mutex, abstime);
}
extern "C" int wrap_pthread_cond_signal(void* cond) {
    uint32_t* ptr = (uint32_t*)cond;
    if (ptr[0] == 0x3CB0B1BB) return 0;
    return pthread_cond_signal((pthread_cond_t*)cond);
}
extern "C" int wrap_pthread_cond_broadcast(void* cond) {
    uint32_t* ptr = (uint32_t*)cond;
    if (ptr[0] == 0x3CB0B1BB) return 0;
    return pthread_cond_broadcast((pthread_cond_t*)cond);
}
extern "C" int wrap_pthread_cond_destroy(void* cond) {
    return pthread_cond_destroy((pthread_cond_t*)cond);
}

extern "C" int wrap_sysctlbyname(const char* name, void* oldp, size_t* oldlenp, void* newp, size_t newlen) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava(std::string(">>>>>>>> [SYSCTL-TRACE] sysctlbyname: ") + (name ? name : "null") + " | Caller: " + GetModuleInfoForAddress(lr) + " <<<<<<<<");
    if (name && (strcmp(name, "hw.machine") == 0 || strcmp(name, "hw.model") == 0)) {
        const char* machine = "iPhone2,1"; // Обманываем игру, притворяясь iPhone 3GS
        if (oldlenp) {
            if (oldp && *oldlenp >= strlen(machine) + 1) {
                strcpy((char*)oldp, machine);
                LogToJava(std::string("  Отдали строку: ") + machine);
            }
            *oldlenp = strlen(machine) + 1;
            return 0;
        }
    }
    return -1;
}

extern "C" int wrap_sysctl(int* name, unsigned int namelen, void* oldp, size_t* oldlenp, void* newp, size_t newlen) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    if (name && namelen > 0) {
        LogToJava(">>>>>>>> [SYSCTL-TRACE] sysctl: [" + std::to_string(name[0]) + (namelen > 1 ? ", " + std::to_string(name[1]) : "") + "] | Caller: " + GetModuleInfoForAddress(lr) + " <<<<<<<<");
    }
    if (name && namelen >= 2 && name[0] == 6) { // 6 = CTL_HW
        if (name[1] == 1 || name[1] == 2) { // 1 = HW_MACHINE, 2 = HW_MODEL
            const char* machine = "iPhone2,1";
            if (oldlenp) {
                if (oldp && *oldlenp >= strlen(machine) + 1) {
                    strcpy((char*)oldp, machine);
                    LogToJava(std::string("  Отдали строку: ") + machine);
                }
                *oldlenp = strlen(machine) + 1;
                return 0;
            }
        }
    }
    return -1;
}

extern "C" int wrap___cxa_guard_acquire(int32_t* guard) {
    char* g = (char*)guard;
    if (g[0] == 0) return 1; // 1 = Требуется инициализация
    return 0; // 0 = Уже инициализировано
}
extern "C" void wrap___cxa_guard_release(int32_t* guard) {
    char* g = (char*)guard;
    g[0] = 1; // Помечаем как инициализированное
}

extern "C" void wrap__Unwind_SjLj_Resume(void* context) {
    LogToJava("КРИТИЧЕСКАЯ ОШИБКА: __Unwind_SjLj_Resume (SjLj Exceptions не поддерживаются)");
    wrap_abort();
}
extern "C" void* wrap___cxa_begin_catch(void* exc) { return exc; }
extern "C" void wrap___cxa_call_unexpected(void* exc) { wrap_abort(); }
extern "C" void wrap___cxa_rethrow() { wrap_abort(); }
extern "C" void* wrap_objc_begin_catch(void* exc) { return exc; }
extern "C" void* wrap_dyld_stub_binder() {
    LogToJava("КРИТИЧЕСКАЯ ОШИБКА: Вызван dyld_stub_binder! (Lazy binding failure)");
    wrap_abort();
    return nullptr;
}

extern "C" int32_t wrap___divsi3(int32_t a, int32_t b) { return b != 0 ? (a / b) : 0; }
extern "C" int32_t wrap___modsi3(int32_t a, int32_t b) { return b != 0 ? (a % b) : 0; }
extern "C" uint32_t wrap___udivsi3(uint32_t a, uint32_t b) { return b != 0 ? (a / b) : 0; }
extern "C" uint32_t wrap___umodsi3(uint32_t a, uint32_t b) { return b != 0 ? (a % b) : 0; }
extern "C" uint64_t wrap___udivdi3(uint64_t a, uint64_t b) { return b != 0 ? (a / b) : 0; }
extern "C" uint64_t wrap___umoddi3(uint64_t a, uint64_t b) { return b != 0 ? (a % b) : 0; }

extern "C" int64_t wrap___fixdfdi(double a) { return (int64_t)a; }
extern "C" int64_t wrap___fixsfdi(float a) { return (int64_t)a; }
extern "C" uint64_t wrap___fixunsdfdi(double a) { return (uint64_t)a; }
extern "C" double wrap___floatdidf(int64_t a) { return (double)a; }
extern "C" double wrap___floatundidf(uint64_t a) { return (double)a; }
extern "C" float wrap___floatundisf(uint32_t a) { return (float)a; }
// -----------------------------------

// --- OpenGL ES 1.1 ---
int g_clientActiveTexture = 0;
extern "C" {
    void wrap_glAlphaFunc(GLenum func, GLclampf ref) {
        g_alphaFunc = func;
        g_alphaRef = ref;
    }
    void wrap_glLoadIdentity() {
        auto& stack = GetCurrentMatrixStack();
        float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(stack.back().data(), id, 16 * sizeof(float));
    }
    void wrap_glLoadMatrixf(const GLfloat *m) {
        if (m) memcpy(GetCurrentMatrixStack().back().data(), m, 16 * sizeof(float));
    }
    void wrap_glMatrixMode(GLenum mode) {
        g_matrixMode = mode;
    }
    void wrap_glPopMatrix() {
        auto& stack = GetCurrentMatrixStack();
        if (stack.size() > 1) stack.pop_back();
        else LogToJava("GL_ERROR: glPopMatrix underflow!");
    }
    void wrap_glPushMatrix() {
        auto& stack = GetCurrentMatrixStack();
        stack.push_back(stack.back());
    }
    void wrap_glScalef(GLfloat x, GLfloat y, GLfloat z) {
        auto& stack = GetCurrentMatrixStack();
        float* m = stack.back().data();
        m[0] *= x; m[1] *= x; m[2]  *= x; m[3]  *= x;
        m[4] *= y; m[5] *= y; m[6]  *= y; m[7]  *= y;
        m[8] *= z; m[9] *= z; m[10] *= z; m[11] *= z;
    }
    void wrap_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
        auto& stack = GetCurrentMatrixStack();
        float* m = stack.back().data();
        m[12] += m[0] * x + m[4] * y + m[8]  * z;
        m[13] += m[1] * x + m[5] * y + m[9]  * z;
        m[14] += m[2] * x + m[6] * y + m[10] * z;
        m[15] += m[3] * x + m[7] * y + m[11] * z;
    }
    void wrap_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
        float a = angle * 3.14159265359f / 180.0f;
        float c = cosf(a), s = sinf(a);
        float len = sqrtf(x*x + y*y + z*z);
        if (len == 0.0f) return;
        x /= len; y /= len; z /= len;
        float ic = 1.0f - c;
        float rot[16] = {
            x*x*ic + c,   y*x*ic + z*s, z*x*ic - y*s, 0,
            x*y*ic - z*s, y*y*ic + c,   z*y*ic + x*s, 0,
            x*z*ic + y*s, y*z*ic - x*s, z*z*ic + c,   0,
            0,            0,            0,            1
        };
        auto& stack = GetCurrentMatrixStack();
        float* m = stack.back().data();
        float res[16];
        for (int i=0; i<4; i++) {
            for (int j=0; j<4; j++) {
                res[i*4+j] = m[0*4+j]*rot[i*4+0] + m[1*4+j]*rot[i*4+1] + m[2*4+j]*rot[i*4+2] + m[3*4+j]*rot[i*4+3];
            }
        }
        memcpy(m, res, 16 * sizeof(float));
    }
    void wrap_glMultMatrixf(const GLfloat *m2) {
        if (!m2) return;
        auto& stack = GetCurrentMatrixStack();
        float* m1 = stack.back().data();
        float res[16];
        for (int i=0; i<4; i++) {
            for (int j=0; j<4; j++) {
                res[i*4+j] = m1[0*4+j]*m2[i*4+0] + m1[1*4+j]*m2[i*4+1] + m1[2*4+j]*m2[i*4+2] + m1[3*4+j]*m2[i*4+3];
            }
        }
        memcpy(m1, res, 16 * sizeof(float));
    }
    void wrap_glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
        if (std::abs(r - l) < 0.001f) { r = 480.0f; l = 0.0f; }
        if (std::abs(t - b) < 0.001f) { t = 320.0f; b = 0.0f; }
        if (std::abs(f - n) < 0.001f) { f = 1.0f; n = -1.0f; }
        float ortho[16] = {
            2.0f/(r-l), 0, 0, 0,
            0, 2.0f/(t-b), 0, 0,
            0, 0, -2.0f/(f-n), 0,
            -(r+l)/(r-l), -(t+b)/(t-b), -(f+n)/(f-n), 1
        };
        wrap_glMultMatrixf(ortho);
    }
    void wrap_glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
        if (std::abs(r - l) < 0.001f) { r = 480.0f; l = 0.0f; }
        if (std::abs(t - b) < 0.001f) { t = 320.0f; b = 0.0f; }
        if (std::abs(f - n) < 0.001f) { f = 1.0f; n = 0.1f; }
        float frustum[16] = {
            2.0f*n/(r-l), 0, 0, 0,
            0, 2.0f*n/(t-b), 0, 0,
            (r+l)/(r-l), (t+b)/(t-b), -(f+n)/(f-n), -1.0f,
            0, 0, -(2.0f*f*n)/(f-n), 0
        };
        wrap_glMultMatrixf(frustum);
    }
    void wrap_glShadeModel(GLenum mode) {
        g_shadeModel = mode;
    }
    void wrap_glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
        // Заглушка: программный рендер уже применяет текстуру через GL_MODULATE в CPUExtractAndDraw
    }
    void wrap_glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {}
    void wrap_glTexEnvi(GLenum target, GLenum pname, GLint param) {}
    
    void wrap_glTexEnvx(GLenum target, GLenum pname, GLint param) {
        wrap_glTexEnvf(target, pname, (GLfloat)param / 65536.0f);
    }
    void wrap_glCurrentPaletteMatrixOES(GLuint matrixpaletteindex) {
        g_currentPaletteMatrix = matrixpaletteindex;
    }
    void wrap_glLoadPaletteFromModelViewMatrixOES() {
        if (g_currentPaletteMatrix < 32) {
            g_paletteStacks[g_currentPaletteMatrix].back() = g_modelViewStack.back();
        }
    }
    void wrap_glMatrixIndexPointerOES(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
        g_matrixIndexSize = size;
        g_matrixIndexType = type;
        g_matrixIndexStride = stride;
        g_matrixIndexPointer = pointer;
        g_matrixIndexVBO = g_boundBuffers[0x8892]; // Читаем GL_ARRAY_BUFFER прямо из нашего кэша враппера
    }
    void wrap_glWeightPointerOES(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
        g_weightSize = size;
        g_weightType = type;
        g_weightStride = stride;
        g_weightPointer = pointer;
        g_weightVBO = g_boundBuffers[0x8892]; // Читаем GL_ARRAY_BUFFER прямо из нашего кэша враппера
    }
    void wrap_glPointSizePointerOES(GLenum type, GLsizei stride, const GLvoid *pointer) {
        g_pointSizeType = type;
        g_pointSizeStride = stride;
        g_pointSizePointer = pointer;
    }
    void wrap_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
        glUniformMatrix3fv(location, count, transpose, value);
    }
    void wrap_glClipPlanef(GLenum p, const GLfloat *eqn) {}
    void wrap_glDiscardFramebufferEXT(GLenum target, GLsizei numAttachments, const GLenum *attachments) {}
    void wrap_glFogf(GLenum pname, GLfloat param) {}
    void wrap_glFogfv(GLenum pname, const GLfloat *params) {}
    void wrap_glLightModelf(GLenum pname, GLfloat param) {}
    void wrap_glLightModelfv(GLenum pname, const GLfloat *params) {}
    void wrap_glLightf(GLenum light, GLenum pname, GLfloat param) {}
    void wrap_glLightfv(GLenum light, GLenum pname, const GLfloat *params) {}
    void wrap_glMaterialf(GLenum face, GLenum pname, GLfloat param) {}
    void wrap_glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {}
    void wrap_glPointParameterf(GLenum pname, GLfloat param) {}
    void wrap_glPointParameterfv(GLenum pname, const GLfloat *params) {}
    void wrap_glPointSize(GLfloat size) { g_pointSize = size; }
    void wrap_glResolveMultisampleFramebufferAPPLE() {}

    void wrap_glValidateProgram(GLuint program) {
        glValidateProgram(program);
        GLint status = 0;
        glGetProgramiv(program, GL_VALIDATE_STATUS, &status);
        if (status == GL_FALSE) {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            if (logLength > 0) {
                std::vector<char> log(logLength);
                glGetProgramInfoLog(program, logLength, nullptr, log.data());
                LogToJava(std::string("GL_VALIDATE ERROR: ") + log.data());
            }
        }
    }

    void wrap_glEnableClientState(GLenum array) {
        if (array == 0x8074) Stub_glEnableVertexAttribArray(0); // GL_VERTEX_ARRAY
        else if (array == 0x8076) Stub_glEnableVertexAttribArray(1); // GL_COLOR_ARRAY
        else if (array == 0x8078) Stub_glEnableVertexAttribArray(2 + g_clientActiveTexture); // GL_TEXTURE_COORD_ARRAY
        else if (array == 0x8075) Stub_glEnableVertexAttribArray(5); // GL_NORMAL_ARRAY
        else if (array == 0x8B44) g_matrixIndexEnabled = true; // GL_MATRIX_INDEX_ARRAY_OES
        else if (array == 0x8B46) g_weightEnabled = true; // GL_WEIGHT_ARRAY_OES
    }
    void wrap_glDisableClientState(GLenum array) {
        if (array == 0x8074) Stub_glDisableVertexAttribArray(0);
        else if (array == 0x8076) Stub_glDisableVertexAttribArray(1);
        else if (array == 0x8078) Stub_glDisableVertexAttribArray(2 + g_clientActiveTexture);
        else if (array == 0x8075) Stub_glDisableVertexAttribArray(5);
        else if (array == 0x8B44) g_matrixIndexEnabled = false;
        else if (array == 0x8B46) g_weightEnabled = false;
    }
    void wrap_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
        Stub_glVertexAttribPointer(0, size, type, GL_FALSE, stride, pointer);
    }
    void wrap_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
        GLboolean normalized = (type == GL_UNSIGNED_BYTE) ? GL_TRUE : GL_FALSE;
        Stub_glVertexAttribPointer(1, size, type, normalized, stride, pointer);
    }
    void wrap_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
        Stub_glVertexAttribPointer(2 + g_clientActiveTexture, size, type, GL_FALSE, stride, pointer);
    }
    void wrap_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
        Stub_glVertexAttribPointer(5, 3, type, GL_FALSE, stride, pointer);
    }
    void wrap_glClientActiveTexture(GLenum texture) {
        g_clientActiveTexture = texture - GL_TEXTURE0; 
    }
    void wrap_glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
        Stub_glVertexAttrib4f(1, red, green, blue, alpha);
    }
    void wrap_glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
        Stub_glVertexAttrib4f(1, red/255.0f, green/255.0f, blue/255.0f, alpha/255.0f);
    }
    void wrap_glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
        int idx = target - GL_TEXTURE0;
        Stub_glVertexAttrib4f(2 + idx, s, t, r, q);
    }
    void wrap_glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
        Stub_glVertexAttrib4f(5, nx, ny, nz, 1.0f);
    }
}
extern "C" void wrap_abort() { 
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava("КРИТИЧЕСКАЯ ОШИБКА: Игра вызвала abort()! Caller: " + GetModuleInfoForAddress(lr)); 
    __builtin_trap(); 
}
extern "C" void wrap___assert_rtn(const char *func, const char *file, int line, const char *expr) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    std::string fName = func ? func : "unknown_func";
    std::string fFile = file ? file : "unknown_file";
    std::string fExpr = expr ? expr : "unknown_expr";
    LogToJava("ASSERT: [" + fExpr + "] in " + fName + " at " + fFile + ":" + std::to_string(line) + " | Caller: " + GetModuleInfoForAddress(lr));
    wrap_abort();
}
extern "C" void* wrap___memset_chk(void* dest, int c, size_t len, size_t destlen) { 
    if (len > destlen) wrap_abort(); 
    return memset(dest, c, len); 
}
extern "C" char* wrap_strnstr(const char* s, const char* find, size_t slen) {
    char c, sc; size_t len;
    if ((c = *find++) != '\0') {
        len = strlen(find);
        do {
            do { if (slen-- < 1 || (sc = *s++) == '\0') return nullptr; } while (sc != c);
            if (len > slen) return nullptr;
        } while (strncmp(s, find, len) != 0);
        s--;
    }
    return (char*)s;
}
extern "C" void* wrap___memcpy_chk(void* dest, const void* src, size_t len, size_t destlen) { return memcpy(dest, src, len); }
extern "C" void* wrap___memmove_chk(void* dest, const void* src, size_t len, size_t destlen) { return memmove(dest, src, len); }
extern "C" char* wrap___strcpy_chk(char* dest, const char* src, size_t destlen) { 
    if (src && (strstr(src, "pngConf") || strstr(src, "jungle"))) {
        LogToJava(std::string("C-API-DEBUG: [__strcpy_chk] Копируется подозрительная строка: [") + src + "]");
    }
    return strcpy(dest, src); 
}
extern "C" char* wrap_strcat(char* dest, const char* src) {
    char* res = strcat(dest, src);
    if (res && (strstr(res, "pngConf") || strstr(res, "jungle"))) {
        LogToJava(std::string("C-API-DEBUG: [strcat] Собрана строка: [") + res + "] src: [" + src + "]");
    }
    return res;
}
extern "C" char* wrap_strncat(char* dest, const char* src, size_t n) {
    char* res = strncat(dest, src, n);
    if (res && (strstr(res, "pngConf") || strstr(res, "jungle"))) {
        LogToJava(std::string("C-API-DEBUG: [strncat] Собрана строка: [") + res + "] src: [" + src + "] n: " + std::to_string(n));
    }
    return res;
}
extern "C" int wrap___sprintf_chk(char* str, int flag, size_t slen, const char* format, ...) { 
    va_list args; va_start(args, format); 
    int ret = vsprintf(str, format, args); 
    va_end(args); 
    if (str && (strstr(str, "pngConf") || strstr(str, "jungle"))) {
        LogToJava(std::string("C-API-DEBUG: [__sprintf_chk] Собрана строка: [") + str + "]");
    }
    return ret; 
}
extern "C" int wrap___snprintf_chk(char *str, size_t maxlen, int flag, size_t bos, const char *format, ...) { 
    va_list args; va_start(args, format); 
    int ret = vsnprintf(str, maxlen, format, args); 
    va_end(args); 
    if (str && (strstr(str, "pngConf") || strstr(str, "jungle"))) {
        LogToJava(std::string("C-API-DEBUG: [__snprintf_chk] Собрана строка: [") + str + "]");
    }
    return ret; 
}
extern "C" int wrap___tolower(int c) { return (c >= 'A' && c <= 'Z') ? (c + 32) : c; }
extern "C" int wrap___toupper(int c) { return (c >= 'a' && c <= 'z') ? (c - 32) : c; }

FILE* unwrap_file(void* fp);

extern "C" int wrap_fputc(int c, void* fp) { return fputc(c, unwrap_file(fp)); }
extern "C" int wrap_fscanf(void* fp, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vfscanf(unwrap_file(fp), format, args);
    va_end(args);
    return res;
}
extern "C" char* wrap_ecvt(double value, int ndigit, int* decpt, int* sign) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%.*e", ndigit - 1, value);
    char* e = strchr(buf, 'e');
    if (sign) *sign = (value < 0.0) ? 1 : 0;
    if (decpt) *decpt = e ? atoi(e + 1) + 1 : 0;
    static char res[64];
    int idx = 0;
    for (char* p = buf; *p && p < e; p++) {
        if (*p >= '0' && *p <= '9') res[idx++] = *p;
    }
    res[idx] = '\0';
    return res;
}
extern "C" char* wrap_fcvt(double value, int ndigit, int* decpt, int* sign) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", ndigit, value);
    if (sign) *sign = (value < 0.0) ? 1 : 0;
    char* dot = strchr(buf, '.');
    if (decpt) *decpt = dot ? (dot - buf) : strlen(buf);
    static char res[64];
    int idx = 0;
    for (char* p = buf; *p; p++) {
        if (*p >= '0' && *p <= '9') res[idx++] = *p;
    }
    res[idx] = '\0';
    return res;
}
extern "C" int wrap_fnmatch(const char* pattern, const char* string, int flags) {
    return fnmatch(pattern, string, flags);
}
extern "C" size_t wrap_mbstowcs(wchar_t* dest, const char* src, size_t n) {
    return mbstowcs(dest, src, n);
}
extern "C" int wrap_rmdir(const char* pathname) {
    return rmdir(pathname);
}
extern "C" long long wrap_strtoll(const char* nptr, char** endptr, int base) {
    return strtoll(nptr, endptr, base);
}
extern "C" int wrap_swscanf(const wchar_t* ws, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vswscanf(ws, format, args);
    va_end(args);
    return res;
}
extern "C" int wrap_unlink(const char* pathname) {
    return unlink(pathname);
}
extern "C" int wrap_aio_read(void *aiocbp) { 
    // У Android нет AIO. Возвращаем -1, чтобы игра откатилась на обычный read/fread
    return -1; 
}
extern "C" int wrap_aio_error(void *aiocbp) { return -1; }
extern "C" ssize_t wrap_aio_return(void *aiocbp) { return -1; }
extern "C" int wrap_ioctl(int fd, unsigned long request, ...) {
    if (fd == 100) return 0; // Имитация успеха для RakNet
    va_list args; va_start(args, request); void* argp = va_arg(args, void*); va_end(args);
    return ioctl(fd, request, argp);
}
extern "C" int wrap_fcntl(int fd, int cmd, ...) {
    if (fd == 100) return 0; // Защита от O_NONBLOCK на фейковом сокете
    va_list args; va_start(args, cmd); void* argp = va_arg(args, void*); va_end(args);
    return fcntl(fd, cmd, argp);
}
extern "C" int wrap_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout) {
    // Безопасная имитация таймаута без реального вызова select (чтобы не крашилось на фейковых FD)
    if (timeout) {
        struct timeval* tv = (struct timeval*)timeout;
        long usec = tv->tv_sec * 1000000L + tv->tv_usec;
        if (usec > 0) usleep(usec);
    } else {
        usleep(5000); // 5ms sleep, если таймаут не передан
    }
    
    // Очищаем множества сокетов (сообщаем, что нет новых данных)
    if (readfds) FD_ZERO((fd_set*)readfds);
    if (writefds) FD_ZERO((fd_set*)writefds);
    if (exceptfds) FD_ZERO((fd_set*)exceptfds);
    
    return 0; // Всегда возвращаем 0 (таймаут)
}
extern "C" int wrap_statfs(const char *path, struct statfs *buf) {
    return statfs(path, buf);
}
extern "C" int wrap_getifaddrs(struct ifaddrs_dummy **ifap) {
    static struct ifaddrs_dummy dummy_ifa;
    static char ifa_name[] = "lo0";
    static struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = 0x0100007F; // 127.0.0.1
    
    dummy_ifa.ifa_next = nullptr;
    dummy_ifa.ifa_name = ifa_name;
    dummy_ifa.ifa_flags = 0x9; // IFF_UP | IFF_LOOPBACK
    dummy_ifa.ifa_addr = &sa;
    dummy_ifa.ifa_netmask = nullptr;
    dummy_ifa.dummy_ifa_dstaddr = nullptr;
    dummy_ifa.ifa_data = nullptr;
    
    if (ifap) *ifap = &dummy_ifa;
    LogToJava("C-API-TRACE: getifaddrs() ИМИТАЦИЯ (lo0, 127.0.0.1)");
    return 0;
}
extern "C" void wrap_freeifaddrs(struct ifaddrs_dummy *ifa) {}
extern "C" int wrap_vswprintf(wchar_t* ws, size_t len, const wchar_t* format, va_list args) {
    return vswprintf(ws, len, format, args);
}
extern "C" int wrap_wcsncmp(const wchar_t* ws1, const wchar_t* ws2, size_t n) {
    return wcsncmp(ws1, ws2, n);
}
extern "C" float wrap_wcstof(const wchar_t* nptr, wchar_t** endptr) {
    return wcstof(nptr, endptr);
}
extern "C" uint32_t wrap___maskrune(int c, uint32_t f) {
    return c & f; // BSD ctype fallback
}
extern "C" int64_t wrap___divdi3(int64_t a, int64_t b) {
    return b != 0 ? (a / b) : 0;
}
extern "C" uint64_t wrap___fixunssfdi(float a) {
    return (uint64_t)a;
}
extern "C" char* wrap___cxa_demangle(const char* mangled_name, char* output_buffer, size_t* length, int* status) {
    return abi::__cxa_demangle(mangled_name, output_buffer, length, status);
}
extern "C" int wrap___gxx_personality_sj0(...) {
    return 8; // _URC_CONTINUE_UNWIND
}
extern "C" int wrap___objc_personality_v0(...) {
    return 8; // _URC_CONTINUE_UNWIND
}
extern "C" int wrap_SecItemAdd(void* attributes, void** result) {
    LogToJava("HLE: SecItemAdd called (returning errSecSuccess)");
    if (result) *result = nullptr;
    return 0;
}
extern "C" int wrap_SecItemCopyMatching(void* query, void** result) {
    LogToJava("HLE: SecItemCopyMatching called (returning errSecItemNotFound)");
    if (result) *result = nullptr;
    return -25300; // errSecItemNotFound
}
extern "C" int wrap_SecItemUpdate(void* query, void* attributesToUpdate) {
    LogToJava("HLE: SecItemUpdate called (returning errSecSuccess)");
    return 0;
}

pthread_mutex_t g_timeMutex = PTHREAD_MUTEX_INITIALIZER;
uint64_t g_fake_time_ns = 0;
int g_timer_log_count = 0;

extern "C" uint64_t Stub_mach_absolute_time() {
    pthread_mutex_lock(&g_timeMutex);
    g_fake_time_ns += 16666666ULL; // Искусственный шаг ~16.6мс (60 FPS)
    uint64_t res = g_fake_time_ns;
    if (g_timer_log_count++ < 30) LogToJava("C-API-TRACE: Stub_mach_absolute_time() -> " + std::to_string(res));
    pthread_mutex_unlock(&g_timeMutex);
    return res;
}
extern "C" int Stub_mach_timebase_info(uint32_t* info) {
    if (info) { info[0] = 1; info[1] = 1; }
    LogToJava("C-API-TRACE: Stub_mach_timebase_info() called");
    return 0;
}
extern "C" double Stub_CFAbsoluteTimeGetCurrent() {
    pthread_mutex_lock(&g_timeMutex);
    g_fake_time_ns += 16666666ULL;
    double res = (double)g_fake_time_ns / 1000000000.0;
    if (g_timer_log_count++ < 30) LogToJava("C-API-TRACE: Stub_CFAbsoluteTimeGetCurrent() -> " + std::to_string(res));
    pthread_mutex_unlock(&g_timeMutex);
#if defined(__arm__)
    // ABI Hard-Float Fix: double в iOS всегда возвращается в d0
    register double d0 asm("d0") = res;
    asm volatile ("" : : "w"(d0));
#endif
    return res;
}
extern "C" void Stub_cxa_pure_virtual() { LogToJava("C++ pure virtual function call!"); __builtin_trap(); }
extern "C" void Stub_bzero(void* s, size_t n) { memset(s, 0, n); }

// --- ПРЕДВАРИТЕЛЬНОЕ ОБЪЯВЛЕНИЕ ФУНКЦИЙ (FORWARD DECLARATIONS) ---
extern "C" void* wrap_cxx_string_default_ctor(void* this_ptr);
extern "C" void* wrap_cxx_string_ctor(void* this_ptr, void* str_ptr, void* alloc);
extern "C" void* wrap_cxx_string_copy_ctor(void* this_ptr, void* other_ptr);
extern "C" void* wrap_cxx_string_dtor(void* this_ptr);
extern "C" void* wrap_cxx_string_reserve(void* this_ptr, size_t res_arg);
extern "C" void* wrap_cxx_string_append(void* this_ptr, void* other_ptr);
extern "C" void* wrap_cxx_string_append_ptr_len(void* this_ptr, const char* str_ptr, size_t len);
extern "C" int wrap_cxx_string_compare_string(void* this_ptr, void* other_ptr);
extern "C" void wrap_cxx_string_push_back(void* this_ptr, char c);
extern "C" void wrap_cxx_string_dispose(void* rep_ptr, void* alloc_ptr);
extern "C" void* wrap_cxx_string_assign_ptr_len(void* this_ptr, const char* str_ptr, size_t len);
extern "C" void* wrap_cxx_string_assign_string(void* this_ptr, void* other_ptr);
extern "C" void* wrap_cxx_string_ctor_ptr_len(void* this_ptr, void* str_ptr, size_t len, void* alloc);
extern "C" size_t wrap_cxx_string_rfind_char(void* this_ptr, char c, size_t pos);
extern "C" void* wrap_cxx_string_ctor_sub(void* this_ptr, void* str_ptr, size_t pos, size_t n);
extern "C" int wrap_cxx_string_compare_char(void* this_ptr, const char* s);
extern "C" void wrap_Rb_tree_insert_and_rebalance(bool insert_left, void* x_node, void* p_node, void* header_node);
extern "C" void* wrap_Rb_tree_increment(void* x_node);
extern "C" void* wrap_Rb_tree_decrement(void* x_node);

extern "C" void* wrap_cxx_wstring_default_ctor(void* this_ptr);
extern "C" void* wrap_cxx_wstring_ctor(void* this_ptr, void* str_ptr, void* alloc);
extern "C" void* wrap_cxx_wstring_copy_ctor(void* this_ptr, void* other_ptr);
extern "C" void* wrap_cxx_wstring_reserve(void* this_ptr, size_t res_arg);
extern "C" const int32_t* wrap_cxx_wstring_c_str(void* this_ptr);
extern "C" void* wrap_cxx_wstring_assign_ptr_len(void* this_ptr, const int32_t* str_ptr, size_t len);
extern "C" void* wrap_cxx_wstring_assign_string(void* this_ptr, void* other_ptr);
extern "C" void* wrap_cxx_wstring_assign_c_str(void* this_ptr, const int32_t* str);

extern "C" size_t wrap_cxx_string_length(void* this_ptr);
extern "C" void* wrap_cxx_string_assign_c_str(void* this_ptr, const char* str);
extern "C" void* wrap_cxx_string_M_leak_hard(void* this_ptr);
extern "C" size_t wrap_cxx_string_find_char(void* this_ptr, char c, size_t pos);
extern "C" void wrap_cxx_string_M_mutate(void* this_ptr, size_t pos, size_t len1, size_t len2);
extern "C" void* wrap_cxx_string_M_replace_aux(void* this_ptr, size_t pos, size_t n1, size_t n2, char c);
extern "C" void wrap_cxx_string_M_destroy(void* rep_ptr, void* alloc_ptr);
extern "C" void* wrap_allocator_ctor(void* this_ptr);
extern "C" void* wrap_allocator_dtor(void* this_ptr);
extern "C" void wrap_memset_pattern16(void *b, const void *pattern16, size_t len);

extern "C" void* wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
extern "C" int wrap_munmap(void *addr, size_t length);
extern "C" void wrap_free(void* ptr);
extern "C" int wrap_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern "C" void* wrap_gethostbyname(const char *name);
extern "C" uint64_t wrap_mach_absolute_time();
extern "C" int wrap_mach_timebase_info(uint32_t* info);
extern "C" int wrap_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);
extern "C" ssize_t wrap_recv(int sockfd, void *buf, size_t len, int flags);
extern "C" ssize_t wrap_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);

// --- РЕАЛИЗАЦИИ НОВЫХ ОБЕРТОК ---
extern "C" char* wrap_getenv(const char* name) {
    if (name && strcmp(name, "HOME") == 0) return (char*)g_sandboxDir.c_str();
    if (name && strcmp(name, "TMPDIR") == 0) { static std::string tmp = g_sandboxDir + "tmp/"; return (char*)tmp.c_str(); }
    return getenv(name);
}

extern "C" int wrap_pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    return pthread_attr_setstacksize(attr, stacksize);
}

extern "C" uint32_t wrap_pthread_mach_thread_np(pthread_t thread) {
    // В HLE Mach-порт потока обычно имитируется возвратом самого указателя/ID pthread
    return (uint32_t)(uintptr_t)thread;
}

extern "C" int wrap_thread_get_state(uint32_t target_thread, int flavor, void* old_state, uint32_t* old_stateCnt) {
    LogToJava("C-API-HLE: thread_get_state вызван (Имитация KERN_SUCCESS)");
    // Безопасно зануляем буфер регистров, чтобы игра не прочитала мусор из памяти
    if (old_state && old_stateCnt) {
        memset(old_state, 0, (*old_stateCnt) * sizeof(uint32_t));
    }
    return 0; // KERN_SUCCESS
}

extern "C" int wrap_thread_set_state(uint32_t target_thread, int flavor, void* new_state, uint32_t new_stateCnt) {
    LogToJava("C-API-HLE: thread_set_state вызван (Имитация KERN_SUCCESS)");
    return 0; // KERN_SUCCESS
}

extern "C" int wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava("C-API-TRACE: [pthread_create] Создается новый поток! Caller: " + GetModuleInfoForAddress(lr));
    // КРИТИЧНО: Игнорируем attr, передавая nullptr. 
    // Структуры iOS и Android не совпадают, Android вернет EINVAL и поток не запустится!
    int res = pthread_create(thread, nullptr, start_routine, arg);
    if (res != 0) {
        LogToJava("C-API-ERROR: pthread_create FAILED with code " + std::to_string(res));
    }
    return res;
}

pthread_mutex_t g_onceMutex = PTHREAD_MUTEX_INITIALIZER;
extern "C" int wrap_pthread_once(uint32_t* once_control, void (*init_routine)(void)) {
    pthread_mutex_lock(&g_onceMutex);
    // В iOS PTHREAD_ONCE_INIT равен 0x30B1BCBA, а не 0 как в Android.
    if (*once_control == 0x30B1BCBA || *once_control == 0) {
        init_routine();
        *once_control = 2; // Отмечаем как выполненное
    }
    pthread_mutex_unlock(&g_onceMutex);
    return 0;
}

extern "C" int wrap__NSGetExecutablePath(char* buf, uint32_t* bufsize) {
    if (!buf || !bufsize) return -1;
    if (*bufsize < g_execPath.length() + 1) {
        *bufsize = g_execPath.length() + 1;
        return -1;
    }
    strcpy(buf, g_execPath.c_str());
    return 0;
}

extern "C" int wrap_mach_timebase_info(uint32_t* info) {
    if (info) { info[0] = 1; info[1] = 1; } // numer=1, denom=1 (наносекунды)
    return 0;
}
extern "C" int wrap_socket(int domain, int type, int protocol) {
    LogToJava("C-API-TRACE: socket() ИМИТАЦИЯ УСПЕХА");
    return 100; // Фейковый file descriptor
}
extern "C" int wrap_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    LogToJava("C-API-TRACE: bind() ИМИТАЦИЯ УСПЕХА");
    return 0;
}
extern "C" int wrap_listen(int sockfd, int backlog) { return 0; }
extern "C" int wrap_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(19132);
        sin->sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1
        *addrlen = sizeof(struct sockaddr_in);
    }
    return 0;
}
extern "C" int wrap_setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) { return 0; }
extern "C" int wrap_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) { return 0; }
extern "C" ssize_t wrap_send(int sockfd, const void *buf, size_t len, int flags) { return len; }
extern "C" ssize_t wrap_sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) { return len; }
extern "C" ssize_t wrap_recv(int sockfd, void *buf, size_t len, int flags) { 
    usleep(1000); // 1ms sleep для защиты от жесткого цикла
    errno = EAGAIN; return -1; 
}
extern "C" ssize_t wrap_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) { 
    usleep(1000); 
    errno = EAGAIN; return -1; 
}
extern "C" int wrap_gethostname(char *name, size_t len) {
    if (name && len > 0) {
        strncpy(name, "localhost", len);
        name[len - 1] = '\0';
    }
    return 0;
}

uint32_t hle_dispatch_main_q_struct[16] = {0};

extern "C" void* wrap_dispatch_get_main_queue() {
    return (void*)&hle_dispatch_main_q_struct;
}

extern "C" void wrap_dispatch_async(void* queue, void* block) {
    if (!block) return;
    LogToJava("C-API-HLE: Выполняем dispatch_async блок (синхронно)...");
    uint32_t* b = (uint32_t*)block;
    typedef void (*BlockInvoke)(void*);
    // В структуре блока Apple функция лежит со смещением 12 байт (индекс 3)
    BlockInvoke invoke = (BlockInvoke)b[3];
    if (invoke) {
        invoke(block); 
    }
}

extern "C" void wrap_dispatch_sync(void* queue, void* block) {
    if (!block) return;
    LogToJava("C-API-HLE: Выполняем dispatch_sync блок...");
    uint32_t* b = (uint32_t*)block;
    typedef void (*BlockInvoke)(void*);
    BlockInvoke invoke = (BlockInvoke)b[3];
    if (invoke) {
        invoke(block); 
    }
}

extern "C" void wrap_dispatch_once(uint32_t* predicate, void* block) {
    if (!predicate || !block) return;
    if (*predicate == 0) {
        *predicate = 1;
        LogToJava("C-API-HLE: Выполняем dispatch_once блок...");
        uint32_t* b = (uint32_t*)block;
        typedef void (*BlockInvoke)(void*);
        BlockInvoke invoke = (BlockInvoke)b[3];
        if (invoke) invoke(block); 
    }
}

std::string DumpHexToString(const char* data, int max_len) {
    if (!data) return "null";
    std::string hex = "";
    char buf[16];
    for (int i = 0; i < max_len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0) { hex += "00 "; break; } // Смотрим, где строка реально обрывается
        snprintf(buf, sizeof(buf), "%02X ", c);
        hex += buf;
    }
    return hex;
}

std::map<void*, std::string> g_fileNames;

extern bool g_machOLoaded;

// Защита от рекурсии внутри wrap_malloc/free/etc.
//
// ПОЧЕМУ НЕ thread_local:
//   На Android NDK thread_local в .so реализован через __emutls_get_address,
//   который при ПЕРВОМ обращении сам вызывает malloc → wrap_malloc → рекурсия.
//   pthread_getspecific — единственный TLS-механизм безопасный внутри malloc.
//
// Схема: pthread_key инициализируется один раз через pthread_once.
//   Значение ключа: NULL = не в malloc, (void*)1 = уже внутри.
//   pthread_getspecific/setspecific не делают malloc — они безопасны.

static pthread_key_t  g_wrapMallocKey;
static pthread_once_t g_wrapMallocKeyOnce = PTHREAD_ONCE_INIT;

static void _wrapMallocKeyCreate() {
    pthread_key_create(&g_wrapMallocKey, nullptr);
}

// inline: вызывается очень часто, важна скорость
static inline bool _inWrapMalloc() {
    pthread_once(&g_wrapMallocKeyOnce, _wrapMallocKeyCreate);
    return pthread_getspecific(g_wrapMallocKey) != nullptr;
}
static inline void _setInWrapMalloc(bool v) {
    pthread_setspecific(g_wrapMallocKey, v ? (void*)1 : nullptr);
}

extern "C" void* wrap_malloc(size_t size) {
    if (_inWrapMalloc()) return malloc(size);
    _setInWrapMalloc(true);
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void* res = malloc(size);
    if (size > 5 * 1024 * 1024) {
        LogToJava("C-API-DEBUG: [malloc] АНОМАЛИЯ! Игра просит " + std::to_string(size) + " байт! Caller: " + GetModuleInfoForAddress(lr));
    } else if (g_machOLoaded) {
        LogToBlackBox("C-API-MEM: [malloc] ptr=" + std::to_string((uintptr_t)res) + " size=" + std::to_string(size) + " Caller: " + GetModuleInfoForAddress(lr));
    }
    _setInWrapMalloc(false);
    return res;
}
extern "C" void* wrap_calloc(size_t num, size_t size) {
    if (_inWrapMalloc()) return calloc(num, size);
    _setInWrapMalloc(true);
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void* res = calloc(num, size);
    if (num * size > 5 * 1024 * 1024) {
        LogToJava("C-API-DEBUG: [calloc] АНОМАЛИЯ! Игра просит " + std::to_string(num * size) + " байт! Caller: " + GetModuleInfoForAddress(lr));
    } else if (g_machOLoaded) {
        LogToBlackBox("C-API-MEM: [calloc] ptr=" + std::to_string((uintptr_t)res) + " num=" + std::to_string(num) + " size=" + std::to_string(size) + " Caller: " + GetModuleInfoForAddress(lr));
    }
    _setInWrapMalloc(false);
    return res;
}
extern "C" void* wrap_realloc(void* ptr, size_t size) {
    if (_inWrapMalloc()) return realloc(ptr, size);
    _setInWrapMalloc(true);
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void* res = realloc(ptr, size);
    if (size > 5 * 1024 * 1024) {
        LogToJava("C-API-DEBUG: [realloc] АНОМАЛИЯ! Игра просит " + std::to_string(size) + " байт! Caller: " + GetModuleInfoForAddress(lr));
    } else if (g_machOLoaded) {
        LogToBlackBox("C-API-MEM: [realloc] old_ptr=" + std::to_string((uintptr_t)ptr) + " new_ptr=" + std::to_string((uintptr_t)res) + " size=" + std::to_string(size) + " Caller: " + GetModuleInfoForAddress(lr));
    }
    _setInWrapMalloc(false);
    return res;
}
extern "C" void wrap_free(void* ptr) {
    if (_inWrapMalloc()) { free(ptr); return; }
    _setInWrapMalloc(true);
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    if (g_machOLoaded && ptr) {
        LogToBlackBox("C-API-MEM: [free] ptr=" + std::to_string((uintptr_t)ptr) + " Caller: " + GetModuleInfoForAddress(lr));
    }
    free(ptr);
    _setInWrapMalloc(false);
}

// Хелпер для извлечения реального Android FILE* из нашей фейковой iOS структуры
std::map<void*, FILE*> g_fileMap;

FILE* unwrap_file(void* fp) {
    if (!fp) return nullptr;
    // Системные потоки Android Bionic пропускаем как есть
    if (fp == stdout || fp == stderr || fp == stdin) return (FILE*)fp;
    
    // Проверяем наличие в нашем пуленепробиваемом словаре
    if (g_fileMap.count(fp)) {
        return g_fileMap[fp];
    }
    
    // Резервная проверка (если структура не повреждена)
    uint32_t isa = 0;
    if (SafeRead32((uintptr_t)fp + 60, &isa) && isa == 0xBADF00D5) {
        uint32_t real_fp = 0;
        if (SafeRead32((uintptr_t)fp + 64, &real_fp) && real_fp != 0) {
            return (FILE*)real_fp;
        }
    }
    
    return nullptr;
}

extern "C" int wrap_access(const char* path, int mode) {
    std::string sPath = path ? path : "null";
    bool isRelative = (sPath.length() > 0 && sPath[0] != '/');
    if (isRelative) {
        std::string docPath = g_sandboxDir + "Documents/" + sPath;
        if (access(docPath.c_str(), mode) == 0) return 0;
        std::string bundlePath = g_appBundlePath + "/" + sPath;
        if (access(bundlePath.c_str(), mode) == 0) return 0;
        sPath = docPath; // Для лога
    }
    LogToBlackBox("C-API-IO: [access] Проверка доступа: " + sPath + " mode: " + std::to_string(mode));
    return access(sPath.c_str(), mode);
}

extern "C" DIR* wrap_opendir(const char* name) {
    std::string sName = name ? name : "null";
    LogToBlackBox("C-API-IO: [opendir] Чтение директории: " + sName);
    return opendir(name);
}

extern "C" struct dirent* wrap_readdir(DIR* dirp) {
    struct dirent* res = readdir(dirp);
    if (res) {
        LogToBlackBox("C-API-IO: [readdir] Найдено: " + std::string(res->d_name));
    }
    return res;
}

extern "C" void* wrap_fopen(const char* path, const char* mode) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    std::string origPath = path ? path : "null";
    std::string sPath = origPath;
    std::string sMode = mode ? mode : "null";
    
    bool isRelative = (sPath.length() > 0 && sPath[0] != '/');
    if (isRelative) {
        sPath = g_sandboxDir + "Documents/" + origPath;
    }
    
    if (sPath.find(".dll") != std::string::npos || sPath.find("mscorlib") != std::string::npos || sPath.find("Mono") != std::string::npos) {
        LogToJava("MONO-TRACE: [fopen] Попытка открыть: " + sPath + " | Caller: " + GetModuleInfoForAddress(lr));
    } else {
        LogToBlackBox("C-API-DEBUG: [FOPEN] Попытка открыть файл: [" + sPath + "] Caller: " + GetModuleInfoForAddress(lr));
        if (sPath.find("pngConf") != std::string::npos || sPath.find("jungle") != std::string::npos || sPath.find("achieve") != std::string::npos || sPath.find(".ima") != std::string::npos || sPath.find(".wav") != std::string::npos) {
            LogToJava("C-API-DEBUG: ВАЖНЫЙ [FOPEN]: [" + sPath + "] Caller: " + GetModuleInfoForAddress(lr));
        }
    }
    
    FILE* real_f = fopen(sPath.c_str(), sMode.c_str());
    
    // ФОЛБЕК НА БАНДЛ: Если файл не найден в Documents (например, кэш) и открыт на чтение, ищем в архивах игры!
    if (!real_f && isRelative && sMode.find('w') == std::string::npos && sMode.find('a') == std::string::npos) {
        std::string bundlePath = g_appBundlePath + "/" + origPath;
        real_f = fopen(bundlePath.c_str(), sMode.c_str());
        if (real_f) {
            sPath = bundlePath;
            LogToJava("C-API-IO: Файл успешно открыт из Bundle: " + sPath);
        }
    }

    if (!real_f) {
        LogToJava("C-API-TRACE: fopen FAILED for: " + sPath + " mode: " + sMode + " | Caller: " + GetModuleInfoForAddress(lr));
        return nullptr;
    }

    // Создаем фейковый iOS (BSD) FILE*
    uint32_t* fake_fp = (uint32_t*)calloc(1, 128);
    fake_fp[0] = 0; // _p = NULL
    
    // КРИТИЧЕСКИЙ ХАК: _r = 0.
    // Это заставит заинлайненный iOS макрос getc(--fp->_r < 0) провалиться
    // и принудительно вызвать внешнюю функцию __srget, которую мы перехватим ниже!
    fake_fp[1] = 0; 
    
    fake_fp[15] = 0xBADF00D5; // MAGIC для идентификации нашей обертки
    fake_fp[16] = (uint32_t)real_f; // Прячем реальный указатель
    
    g_fileMap[fake_fp] = real_f;  // Надежно регистрируем файл
    g_fileNames[fake_fp] = sPath; // Запоминаем путь для отладки
    return fake_fp;
}

// Обертки, которые распаковывают фейковый указатель перед вызовом Android libc
// --- Сбор статистики чтения файлов ---
struct FileReadStats {
    long firstOffset;
    size_t totalBytesRead;
    int readCalls;
    std::string firstBytesHex;
};
std::map<void*, FileReadStats> g_fileReadStats;

extern "C" size_t wrap_fread(void* ptr, size_t size, size_t nitems, void* fp) { 
    FILE* real_f = unwrap_file(fp);
    if (!real_f) return 0; 
    
    long cur_offset = ftell(real_f);
    size_t res = fread(ptr, size, nitems, real_f); 
    size_t bytesRead = res * size;

    if (g_fileNames.count(fp) && bytesRead > 0) {
        if (!g_fileReadStats.count(fp)) {
            g_fileReadStats[fp].firstOffset = cur_offset;
            g_fileReadStats[fp].totalBytesRead = 0;
            g_fileReadStats[fp].readCalls = 0;
            g_fileReadStats[fp].firstBytesHex = DumpHexToString((const char*)ptr, std::min((size_t)16, bytesRead));
        }
        g_fileReadStats[fp].totalBytesRead += bytesRead;
        g_fileReadStats[fp].readCalls++;
    }
    return res; 
}
extern "C" size_t wrap_fwrite(const void* ptr, size_t size, size_t nitems, void* fp) { 
    if (fp == stdout || fp == stderr) {
        if (ptr && size * nitems > 0) {
            std::string s((const char*)ptr, size * nitems);
            LogToJava("GAME-LOG: " + s);
        }
        return nitems;
    }
    FILE* real_f = unwrap_file(fp);
    if (!real_f) return 0;
    return fwrite(ptr, size, nitems, real_f); 
}
extern "C" int wrap_fseek(void* fp, long offset, int whence) { 
    FILE* real_f = unwrap_file(fp);
    if (!real_f) return -1;
    int res = fseek(real_f, offset, whence); 
    if (g_fileNames.count(fp)) {
        LogToBlackBox("C-API-IO: fseek offset=" + std::to_string(offset) + " whence=" + std::to_string(whence) + " in " + g_fileNames[fp] + " -> result: " + std::to_string(res));
    }
    return res; 
}
extern "C" long wrap_ftell(void* fp) { 
    FILE* real_f = unwrap_file(fp);
    if (!real_f) return -1;
    return ftell(real_f); 
}
extern "C" int wrap_fgetpos(void* fp, uint64_t* pos) {
    FILE* real_f = unwrap_file(fp);
    if (!real_f || !pos) return -1;
    long p = ftell(real_f);
    if (p < 0) return -1;
    *pos = (uint64_t)p;
    return 0;
}
extern "C" int wrap_fsetpos(void* fp, const uint64_t* pos) {
    FILE* real_f = unwrap_file(fp);
    if (!real_f || !pos) return -1;
    return fseek(real_f, (long)(*pos), SEEK_SET);
}
extern "C" int wrap_fclose(void* fp) { 
    if (!fp) return EOF;
    if (fp == stdout || fp == stderr || fp == stdin) return 0;
    
    if (g_fileNames.count(fp)) {
        std::string fname = g_fileNames[fp];
        if (g_fileReadStats.count(fp)) {
            auto& stats = g_fileReadStats[fp];
            if (stats.totalBytesRead > 0 && (fname.find(".bin") != std::string::npos || fname.find(".png") != std::string::npos || fname.find(".pvr") != std::string::npos)) {
                LogToJava("[FREAD-STATS] Закрыт: " + fname + " | Прочитано: " + std::to_string(stats.totalBytesRead) + " байт за " + std::to_string(stats.readCalls) + " вызовов | Смещение: " + std::to_string(stats.firstOffset) + " | Начало: " + stats.firstBytesHex);
            }
            g_fileReadStats.erase(fp);
        }
        LogToBlackBox("C-API-IO: fclose called for " + fname);
        g_fileNames.erase(fp);
    }
    
    FILE* real_f = unwrap_file(fp);
    int res = EOF;
    if (real_f) res = fclose(real_f);
    g_fileMap.erase(fp);
    if (real_f != fp) free(fp);
    return res; 
}

extern "C" int wrap___srget(void* fp) { FILE* f = unwrap_file(fp); return f ? fgetc(f) : EOF; }
extern "C" int wrap_feof(void* fp) { FILE* f = unwrap_file(fp); return f ? feof(f) : 1; }
extern "C" int wrap_ferror(void* fp) { FILE* f = unwrap_file(fp); return f ? ferror(f) : 1; }
extern "C" int wrap_fgetc(void* fp) { FILE* f = unwrap_file(fp); return f ? fgetc(f) : EOF; }
extern "C" char* wrap_fgets(char* str, int n, void* fp) { FILE* f = unwrap_file(fp); return f ? fgets(str, n, f) : nullptr; }
extern "C" int wrap_fileno(void* fp) { FILE* f = unwrap_file(fp); return f ? fileno(f) : -1; }
extern "C" int wrap_fflush(void* fp) { FILE* f = unwrap_file(fp); return f ? fflush(f) : EOF; }

// Глобальные перехваты логов игры
extern "C" int wrap_printf(const char* format, ...) {
    va_list args; va_start(args, format);
    char buf[1024]; vsnprintf(buf, sizeof(buf), format, args);
    LogToJava(std::string("GAME-LOG: ") + buf);
    va_end(args); return strlen(buf);
}
extern "C" int wrap_vprintf(const char* format, va_list args) {
    char buf[1024]; vsnprintf(buf, sizeof(buf), format, args);
    LogToJava(std::string("GAME-LOG: ") + buf);
    return strlen(buf);
}
extern "C" int wrap_puts(const char* str) {
    LogToJava(std::string("GAME-LOG: ") + (str ? str : ""));
    return 0;
}
extern "C" int wrap_fputs(const char* str, void* fp) {
    if (fp == stdout || fp == stderr) { LogToJava(std::string("GAME-LOG: ") + (str ? str : "")); return 0; }
    return fputs(str, unwrap_file(fp));
}
extern "C" int wrap_fprintf(void* fp, const char* format, ...) {
    va_list args; va_start(args, format);
    if (fp == stdout || fp == stderr) {
        char buf[1024]; vsnprintf(buf, sizeof(buf), format, args);
        LogToJava(std::string("GAME-LOG: ") + buf);
        va_end(args); return strlen(buf);
    }
    int ret = vfprintf(unwrap_file(fp), format, args);
    va_end(args); return ret;
}
extern "C" int wrap_vfprintf(void* fp, const char* format, va_list args) {
    if (fp == stdout || fp == stderr) {
        char buf[1024]; vsnprintf(buf, sizeof(buf), format, args);
        LogToJava(std::string("GAME-LOG: ") + buf);
        return strlen(buf);
    }
    return vfprintf(unwrap_file(fp), format, args);
}


extern "C" void wrap_cxa_throw(void* thrown_exception, void* tinfo, void* dest) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    LogToJava("КРИТИЧЕСКАЯ ОШИБКА: Игра выбросила C++ исключение (__cxa_throw)! Caller: " + GetModuleInfoForAddress(lr));
    __builtin_trap();
}

// --- AUDIO HLE IMPLEMENTATIONS ---
struct HLE_AudioQueue { void* callback; void* userData; bool isPlaying; uint32_t alSource; int sampleRate; int channels; int bitDepth; uint32_t formatID; bool hasLoggedFormat; };
struct HLE_AudioQueueBuffer { uint32_t mAudioDataBytesCapacity; void* mAudioData; uint32_t mAudioDataByteSize; void* mUserData; uint32_t mPacketDescriptionCapacity; void* mPacketDescriptions; uint32_t mPacketDescriptionCount; };
struct HLE_AudioFile { std::string path; uint32_t sampleRate; uint16_t channels; uint16_t bitsPerSample; uint32_t dataOffset; uint32_t dataSize; bool isWav; uint32_t formatID; uint32_t bytesPerPacket; };
struct HLE_AudioConverter { uint32_t dummy; };

extern "C" void wrap_alGenSources(int n, uint32_t* sources);
extern "C" void wrap_alGenBuffers(int n, uint32_t* buffers);
extern "C" void wrap_alBufferData(uint32_t buffer, int format, const void* data, int size, int freq);
extern "C" void wrap_alSourcei(uint32_t source, int param, int value);
extern "C" void wrap_alSourcePlay(uint32_t source);
extern "C" void wrap_alSourceStop(uint32_t source);

extern "C" int wrap_AudioFileOpenURL(void* inFileRef, int inPermissions, uint32_t inFileTypeHint, void** outAudioFile) {
    if (!outAudioFile) return -50;
    HLE_AudioFile* af = new HLE_AudioFile();
    af->isWav = false; af->sampleRate = 44100; af->channels = 2; af->bitsPerSample = 16; af->dataOffset = 0; af->dataSize = 0;
    if (inFileRef) { 
        uint32_t* urlInst = (uint32_t*)inFileRef; 
        if (urlInst[1]) {
            std::string path = GetNSString((void*)urlInst[1]);
            if (!path.empty()) {
                if (path.find("file://") == 0) path = path.substr(7);
                if (path[0] != '/') path = g_appBundlePath + "/" + path;
                af->path = path; 
            }
        } 
    }
    LogToJava("HLE_AUDIO: AudioFileOpenURL запрашивает: " + af->path);
    FILE* f = fopen(af->path.c_str(), "rb");
    if (!f) { 
        LogToJava("HLE_AUDIO_ERROR: AudioFileOpenURL не смог открыть: " + af->path);
        delete af; return -43; 
    }
    char riff[4];
    if (fread(riff, 1, 4, f) == 4 && strncmp(riff, "RIFF", 4) == 0) {
        af->isWav = true;
        af->formatID = 0x6C70636D; // 'lpcm'
        fseek(f, 22, SEEK_SET); fread(&af->channels, 2, 1, f); fread(&af->sampleRate, 4, 1, f);
        fseek(f, 34, SEEK_SET); fread(&af->bitsPerSample, 2, 1, f);
        af->bytesPerPacket = (af->bitsPerSample / 8) * af->channels;
        if (af->bytesPerPacket == 0) af->bytesPerPacket = 1;
        fseek(f, 12, SEEK_SET);
        while (!feof(f)) {
            char chunkID[4]; uint32_t chunkSize = 0;
            if (fread(chunkID, 1, 4, f) != 4 || fread(&chunkSize, 4, 1, f) != 1) break;
            if (strncmp(chunkID, "data", 4) == 0) { af->dataOffset = ftell(f); af->dataSize = chunkSize; break; }
            fseek(f, chunkSize, SEEK_CUR);
        }
    } else {
        af->formatID = 0x696D6134; // 'ima4'
        
        af->channels = 1;       // ИСПРАВЛЕНИЕ: Mono по умолчанию убирает искажения и 2x скорость!
        af->sampleRate = 44100;

        fseek(f, 0, SEEK_SET);
        uint32_t hdr[4] = {0};
        if (fread(hdr, 1, 16, f) == 16) {
            for (int i = 0; i < 4; i++) {
                // Эвристика: ищем частоту дискретизации прямо в кастомном заголовке
                if (hdr[i] == 44100 || hdr[i] == 22050 || hdr[i] == 11025 || hdr[i] == 48000 || hdr[i] == 32000) {
                    af->sampleRate = hdr[i];
                    break;
                }
            }
        }

        af->bytesPerPacket = 34 * af->channels;
        fseek(f, 0, SEEK_END); 
        uint32_t fullSize = ftell(f); 
        
        // Магия выравнивания: пропускаем кастомные заголовки SEGA (обычно 16 байт)
        af->dataOffset = fullSize % af->bytesPerPacket; 
        af->dataSize = fullSize - af->dataOffset;
        
        LogToJava("HLE_AUDIO: Определен файл как IMA4. Полный размер: " + std::to_string(fullSize) + " байт, Смещение (Header): " + std::to_string(af->dataOffset) + " SR: " + std::to_string(af->sampleRate) + " Ch: " + std::to_string(af->channels));
    }
    fclose(f);
    *outAudioFile = af;
    return 0;
}
extern "C" int wrap_AudioFileOpenWithCallbacks(void* inClientData, void* inReadFunc, void* inWriteFunc, void* inGetSizeFunc, void* inSetSizeFunc, uint32_t inFileTypeHint, void** outAudioFile) {
    if (outAudioFile) *outAudioFile = new HLE_AudioFile(); return 0;
}
extern "C" int wrap_AudioFileClose(void* inAudioFile) {
    if (inAudioFile) delete (HLE_AudioFile*)inAudioFile; return 0;
}
extern "C" int wrap_AudioFileGetPropertyInfo(void* inAudioFile, uint32_t inPropertyID, uint32_t* outDataSize, uint32_t* isWritable) {
    if (isWritable) *isWritable = 0;
    if (inPropertyID == 0x64666D74) { if (outDataSize) *outDataSize = 40; return 0; } // dfmt
    if (inPropertyID == 0x62636E74) { if (outDataSize) *outDataSize = 8; return 0; }  // bcnt
    if (inPropertyID == 0x70737A65) { if (outDataSize) *outDataSize = 4; return 0; }  // psze (MaximumPacketSize)
    if (inPropertyID == 0x70737562) { if (outDataSize) *outDataSize = 4; return 0; }  // psub (PacketSizeUpperBound)
    if (inPropertyID == 0x70636E74) { if (outDataSize) *outDataSize = 8; return 0; }  // pcnt (PacketCount)
    if (inPropertyID == 0x6D676963) { if (outDataSize) *outDataSize = 0; return -50; } // mgic (MagicCookie) - скипаем ошибкой
    
    char prop[5] = { (char)(inPropertyID >> 24), (char)((inPropertyID >> 16) & 0xFF), (char)((inPropertyID >> 8) & 0xFF), (char)(inPropertyID & 0xFF), 0 };
    LogToJava(std::string("HLE_AUDIO: AudioFileGetPropertyInfo запрошен неизвестный ID: ") + prop);
    
    if (outDataSize) *outDataSize = 128; 
    return 0;
}
extern "C" int wrap_AudioFileGetProperty(void* inAudioFile, uint32_t inPropertyID, uint32_t* ioDataSize, void* outPropertyData) {
    if (!inAudioFile || !outPropertyData || !ioDataSize) return -50;
    HLE_AudioFile* af = (HLE_AudioFile*)inAudioFile;
    if (inPropertyID == 0x64666D74) { // dfmt
        if (*ioDataSize >= 40) {
            memset(outPropertyData, 0, 40);
            double* sampleRate = (double*)outPropertyData;
            uint32_t* props = (uint32_t*)((uint8_t*)outPropertyData + 8);
            *sampleRate = af->sampleRate;
            props[0] = af->formatID;
            props[1] = (af->formatID == 0x6C70636D) ? 0xC : 0;
            props[2] = af->bytesPerPacket;
            props[3] = (af->formatID == 0x696D6134) ? 64 : 1;
            props[4] = (af->formatID == 0x6C70636D) ? props[2] : 0;
            props[5] = af->channels;
            props[6] = (af->formatID == 0x6C70636D) ? af->bitsPerSample : 0;
        }
        return 0;
    }
    if (inPropertyID == 0x62636E74) { // bcnt
        if (*ioDataSize >= 8) { *(uint64_t*)outPropertyData = af->dataSize; }
        else if (*ioDataSize >= 4) { *(uint32_t*)outPropertyData = (uint32_t)af->dataSize; }
        return 0;
    }
    if (inPropertyID == 0x70737A65 || inPropertyID == 0x70737562) { // psze or psub
        if (*ioDataSize >= 4) { *(uint32_t*)outPropertyData = af->bytesPerPacket; }
        return 0;
    }
    if (inPropertyID == 0x70636E74) { // pcnt
        uint64_t packets = af->bytesPerPacket > 0 ? (af->dataSize / af->bytesPerPacket) : 0;
        if (*ioDataSize >= 8) { *(uint64_t*)outPropertyData = packets; }
        else if (*ioDataSize >= 4) { *(uint32_t*)outPropertyData = (uint32_t)packets; }
        return 0;
    }

    char prop[5] = { (char)(inPropertyID >> 24), (char)((inPropertyID >> 16) & 0xFF), (char)((inPropertyID >> 8) & 0xFF), (char)(inPropertyID & 0xFF), 0 };
    LogToJava(std::string("HLE_AUDIO: AudioFileGetProperty запрошен неизвестный ID: ") + prop);

    memset(outPropertyData, 0, *ioDataSize); return 0;
}
extern "C" int wrap_AudioFileReadPackets(void* inAudioFile, uint8_t inUseCache, uint32_t* outNumBytes, void* outPacketDescriptions, int64_t inStartingPacket, uint32_t* ioNumPackets, void* outBuffer) {
    if (!inAudioFile || !ioNumPackets || !outBuffer) return -50;
    HLE_AudioFile* af = (HLE_AudioFile*)inAudioFile;
    uint32_t bytesPerPacket = af->bytesPerPacket;
    if (bytesPerPacket == 0) bytesPerPacket = 1;
    uint32_t reqPackets = *ioNumPackets;
    uint32_t bytesToRead = reqPackets * bytesPerPacket;
    FILE* f = fopen(af->path.c_str(), "rb");
    if (!f) { 
        LogToJava("HLE_AUDIO_ERROR: ReadPackets не смог открыть файл: " + af->path);
        *ioNumPackets = 0; if (outNumBytes) *outNumBytes = 0; return -39; 
    }
    fseek(f, af->dataOffset + (inStartingPacket * bytesPerPacket), SEEK_SET);
    size_t readBytes = fread(outBuffer, 1, bytesToRead, f);
    fclose(f);
    *ioNumPackets = (uint32_t)(readBytes / bytesPerPacket);
    if (outNumBytes) *outNumBytes = (uint32_t)readBytes;
    
    static int rp_log = 0;
    if (rp_log++ < 20) {
        LogToJava("HLE_AUDIO: Чтение пакетов из " + af->path + ". Запрошено пакетов: " + std::to_string(reqPackets) + ", прочитано байт: " + std::to_string(readBytes));
    }
    return 0;
}
extern "C" int wrap_AudioFileReadBytes(void* inAudioFile, uint8_t inUseCache, int64_t inStartingByte, uint32_t* ioNumBytes, void* outBuffer) {
    if (!inAudioFile || !ioNumBytes || !outBuffer) return -50;
    HLE_AudioFile* af = (HLE_AudioFile*)inAudioFile;
    uint32_t reqBytes = *ioNumBytes;
    FILE* f = fopen(af->path.c_str(), "rb");
    if (!f) { 
        LogToJava("HLE_AUDIO_ERROR: ReadBytes не смог открыть файл: " + af->path);
        *ioNumBytes = 0; return -39; 
    }
    fseek(f, af->dataOffset + inStartingByte, SEEK_SET);
    size_t readBytes = fread(outBuffer, 1, reqBytes, f);
    fclose(f);
    *ioNumBytes = (uint32_t)readBytes;
    
    static int rb_log = 0;
    if (rb_log++ < 20) {
        LogToJava("HLE_AUDIO: Чтение сырых байт из " + af->path + ". Запрошено: " + std::to_string(reqBytes) + ", прочитано: " + std::to_string(readBytes));
    }
    return 0;
}
extern "C" int wrap_AudioConverterNew(void* inSourceFormat, void* inDestinationFormat, void** outAudioConverter) {
    if (outAudioConverter) *outAudioConverter = new HLE_AudioConverter(); return 0;
}
extern "C" int wrap_AudioConverterDispose(void* inAudioConverter) {
    if (inAudioConverter) delete (HLE_AudioConverter*)inAudioConverter; return 0;
}
extern "C" int wrap_AudioConverterFillComplexBuffer(void* inAudioConverter, void* inInputDataProc, void* inInputDataProcUserData, uint32_t* ioOutputDataPacketSize, void* outOutputData, void* outPacketDescription) {
    if (ioOutputDataPacketSize) *ioOutputDataPacketSize = 0; return 0;
}
extern "C" int wrap_AudioQueueNewOutput(void* inFormat, void* inCallbackProc, void* inUserData, void* inCallbackRunLoop, void* inCallbackRunLoopMode, uint32_t inFlags, void** outAQ) {
    if (outAQ) { 
        HLE_AudioQueue* aq = new HLE_AudioQueue(); 
        aq->callback = inCallbackProc; aq->userData = inUserData; aq->isPlaying = false; aq->hasLoggedFormat = false;
        if (inFormat) {
            double sampleRate; memcpy(&sampleRate, inFormat, 8);
            uint32_t formatID; memcpy(&formatID, (uint8_t*)inFormat + 8, 4);
            uint32_t channels; memcpy(&channels, (uint8_t*)inFormat + 28, 4); // Исправлено смещение (28 = mChannelsPerFrame)
            uint32_t bitDepth; memcpy(&bitDepth, (uint8_t*)inFormat + 32, 4); // Исправлено смещение (32 = mBitsPerChannel)
            aq->sampleRate = (int)sampleRate; aq->channels = channels; aq->bitDepth = bitDepth; aq->formatID = formatID;
            
            char fmtBuf[64];
            snprintf(fmtBuf, sizeof(fmtBuf), "0x%X", formatID);
            LogToJava("HLE_AUDIO: AudioQueueNewOutput создан. Формат: " + std::string(fmtBuf) + ", SR: " + std::to_string(aq->sampleRate) + ", Ch: " + std::to_string(aq->channels) + " BitDepth: " + std::to_string(aq->bitDepth));
        } else {
            aq->sampleRate = 44100; aq->channels = 2; aq->bitDepth = 16; aq->formatID = 0x6C70636D;
            LogToJava("HLE_AUDIO: AudioQueueNewOutput создан без формата, принудительно ставим LPCM");
        }
        wrap_alGenSources(1, &aq->alSource);
        *outAQ = aq; 
    }
    return 0;
}
extern "C" int wrap_AudioQueueDispose(void* inAQ, bool inImmediate) {
    if (inAQ) delete (HLE_AudioQueue*)inAQ; return 0;
}
extern "C" int wrap_AudioQueueAllocateBufferWithPacketDescriptions(void* inAQ, uint32_t inBufferByteSize, uint32_t inNumberPacketDescriptions, void** outBuffer) {
    if (outBuffer) { 
        HLE_AudioQueueBuffer* buf = new HLE_AudioQueueBuffer(); 
        buf->mAudioDataBytesCapacity = inBufferByteSize; 
        buf->mAudioData = calloc(1, inBufferByteSize); 
        buf->mAudioDataByteSize = 0; 
        buf->mUserData = nullptr;
        buf->mPacketDescriptionCapacity = inNumberPacketDescriptions;
        buf->mPacketDescriptions = inNumberPacketDescriptions > 0 ? calloc(inNumberPacketDescriptions, 16) : nullptr;
        buf->mPacketDescriptionCount = 0;
        *outBuffer = buf; 
    }
    return 0;
}
extern "C" int wrap_AudioQueueAllocateBuffer(void* inAQ, uint32_t inBufferByteSize, void** outBuffer) {
    return wrap_AudioQueueAllocateBufferWithPacketDescriptions(inAQ, inBufferByteSize, 0, outBuffer);
}
extern "C" int wrap_AudioQueueFreeBuffer(void* inAQ, void* inBuffer) {
    if (inBuffer) { 
        HLE_AudioQueueBuffer* buf = (HLE_AudioQueueBuffer*)inBuffer; 
        if (buf->mAudioData) free(buf->mAudioData); 
        if (buf->mPacketDescriptions) free(buf->mPacketDescriptions);
        delete buf; 
    }
    return 0;
}
static const int ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};
static const int ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

void DecodeAppleIMA4(const uint8_t* inBuf, uint32_t inSize, int channels, std::vector<int16_t>& outPCM) {
    uint32_t packetSize = 34;
    uint32_t numPackets = inSize / (packetSize * channels);
    outPCM.resize(numPackets * 64 * channels);

    for (uint32_t p = 0; p < numPackets; p++) {
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t* packet = inBuf + (p * channels + ch) * packetSize;
            int16_t preamble = (packet[0] << 8) | packet[1];
            int predictor = (int16_t)(preamble & 0xFF80);
            int step_idx = preamble & 0x7F;
            if (step_idx > 88) step_idx = 88;

            uint32_t baseOutIdx = p * 64 * channels + ch;

            for (int i = 0; i < 32; i++) {
                uint8_t byte = packet[2 + i];
                for (int nibble = 0; nibble < 2; nibble++) {
                    int code = (nibble == 0) ? (byte & 0x0F) : (byte >> 4);
                    
                    int step = ima_step_table[step_idx];
                    int diff = step >> 3;
                    if (code & 1) diff += step >> 2;
                    if (code & 2) diff += step >> 1;
                    if (code & 4) diff += step;
                    if (code & 8) predictor -= diff;
                    else predictor += diff;

                    if (predictor > 32767) predictor = 32767;
                    else if (predictor < -32768) predictor = -32768;

                    step_idx += ima_index_table[code];
                    if (step_idx < 0) step_idx = 0;
                    else if (step_idx > 88) step_idx = 88;

                    outPCM[baseOutIdx + (i * 2 + nibble) * channels] = (int16_t)predictor;
                }
            }
        }
    }
}

extern "C" int wrap_AudioQueueEnqueueBuffer(void* inAQ, void* inBuffer, uint32_t inNumPacketDescs, const void* inPacketDescs) {
    if (inAQ && inBuffer) {
        HLE_AudioQueue* aq = (HLE_AudioQueue*)inAQ;
        HLE_AudioQueueBuffer* buf = (HLE_AudioQueueBuffer*)inBuffer;
        
        uint32_t pcmSize = 0;
        int format = 0x1103;

        if (buf->mAudioData && buf->mAudioDataByteSize > 0) {
            uint32_t alBuf = 0;
            wrap_alGenBuffers(1, &alBuf);
            
            void* pcmData = buf->mAudioData;
            pcmSize = buf->mAudioDataByteSize;
            std::vector<int16_t> decodedPCM;
            
            if (aq->formatID == 0x696D6134) { // 'ima4'
                if (!aq->hasLoggedFormat) {
                    LogToJava("HLE_AUDIO: Запуск софтварного декодера IMA4! Размер пакета: " + std::to_string(buf->mAudioDataByteSize));
                    aq->hasLoggedFormat = true;
                }
                DecodeAppleIMA4((const uint8_t*)buf->mAudioData, buf->mAudioDataByteSize, aq->channels, decodedPCM);
                pcmData = decodedPCM.data();
                pcmSize = decodedPCM.size() * sizeof(int16_t);
                format = (aq->channels == 2) ? 0x1103 : 0x1101; 
            } else {
                if (!aq->hasLoggedFormat) {
                    LogToJava("HLE_AUDIO: Воспроизведение LPCM. Размер пакета: " + std::to_string(buf->mAudioDataByteSize));
                    aq->hasLoggedFormat = true;
                }
                if (aq->channels == 1 && aq->bitDepth == 8) format = 0x1100;
                else if (aq->channels == 1 && aq->bitDepth == 16) format = 0x1101;
                else if (aq->channels == 2 && aq->bitDepth == 8) format = 0x1102;
            }
            
            wrap_alBufferData(alBuf, format, pcmData, pcmSize, aq->sampleRate);
            wrap_alSourcei(aq->alSource, 0x1009, alBuf);
            if (aq->isPlaying) wrap_alSourcePlay(aq->alSource);
        }

        if (aq->callback) {
            uint32_t duration_ms = 15;
            if (aq->sampleRate > 0 && pcmSize > 0) {
                int ch = (format == 0x1103 || format == 0x1102) ? 2 : 1;
                int bytes_per_sample = (format == 0x1101 || format == 0x1103) ? 2 : 1;
                uint32_t frames = pcmSize / (ch * bytes_per_sample);
                duration_ms = (frames * 1000) / aq->sampleRate;
            }
            if (duration_ms < 15) duration_ms = 15;

            struct AQArgs { void* cb; void* user; void* aq; void* buf; uint32_t sleep_ms; };
            AQArgs* args = new AQArgs{aq->callback, aq->userData, aq, inBuffer, duration_ms};
            
            pthread_t t;
            pthread_create(&t, nullptr, [](void* p) -> void* {
                AQArgs* a = (AQArgs*)p; 
                
                uint32_t elapsed = 0;
                while (elapsed < a->sleep_ms) {
                    usleep(15000);
                    elapsed += 15;
                }
                
                typedef void (*AQCallback)(void*, void*, void*);
                ((AQCallback)a->cb)(a->user, a->aq, a->buf);
                
                if (g_jvm) {
                    JNIEnv* env;
                    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
                        g_jvm->DetachCurrentThread(); // Защита от утечек и крашей GC
                    }
                }
                delete a; 
                return nullptr;
            }, args);
        }
    }
    return 0;
}
extern "C" int wrap_AudioQueueEnqueueBufferWithParameters(void* inAQ, void* inBuffer, uint32_t inNumPacketDescs, const void* inPacketDescs, uint32_t inTrimFramesAtStart, uint32_t inTrimFramesAtEnd, uint32_t inNumParamValues, const void* inParamValues, const void* inStartTime, void* outActualStartTime) {
    return wrap_AudioQueueEnqueueBuffer(inAQ, inBuffer, inNumPacketDescs, inPacketDescs);
}
extern "C" int wrap_AudioQueueStart(void* inAQ, const void* inStartTime) { 
    if (inAQ) {
        ((HLE_AudioQueue*)inAQ)->isPlaying = true; 
        wrap_alSourcePlay(((HLE_AudioQueue*)inAQ)->alSource);
    } 
    return 0; 
}
extern "C" int wrap_AudioQueueStop(void* inAQ, bool inImmediate) { 
    if (inAQ) {
        ((HLE_AudioQueue*)inAQ)->isPlaying = false; 
        wrap_alSourceStop(((HLE_AudioQueue*)inAQ)->alSource);
    } 
    return 0; 
}
extern "C" int wrap_AudioQueuePause(void* inAQ) { 
    if (inAQ) {
        ((HLE_AudioQueue*)inAQ)->isPlaying = false; 
        wrap_alSourceStop(((HLE_AudioQueue*)inAQ)->alSource);
    } 
    return 0; 
}
extern "C" int wrap_AudioQueueGetProperty(void* inAQ, uint32_t inID, void* outData, uint32_t* ioDataSize) { if (outData && ioDataSize) memset(outData, 0, *ioDataSize); return 0; }
extern "C" int wrap_AudioQueueGetCurrentTime(void* inAQ, void* inTimeline, void* outTimeStamp, uint8_t* outTimelineDiscontinuity) {
    if (outTimeStamp) { memset(outTimeStamp, 0, 64); *(double*)outTimeStamp = (double)Stub_mach_absolute_time() / 1000000000.0; }
    if (outTimelineDiscontinuity) *outTimelineDiscontinuity = 0;
    return 0;
}
extern "C" int wrap_AudioQueuePrime(void* inAQ, uint32_t inNumberOfFramesToPrepare, uint32_t* outNumberOfFramesPrepared) {
    if (outNumberOfFramesPrepared) *outNumberOfFramesPrepared = inNumberOfFramesToPrepare;
    return 0;
}
extern "C" int wrap_AudioQueueSetParameter(void* inAQ, uint32_t inParamID, float inValue) { return 0; }

extern "C" int wrap_AudioSessionInitialize(void* inRunLoop, void* inRunLoopMode, void* inInterruptionListener, void* inClientData) { return 0; }
extern "C" int wrap_AudioSessionSetActive(uint8_t active) { return 0; }
extern "C" int wrap_AudioSessionAddPropertyListener(uint32_t inID, void* inProc, void* inClientData) { return 0; }
extern "C" int wrap_AudioSessionSetProperty(uint32_t inID, uint32_t inDataSize, const void* inData) { return 0; }
extern "C" int wrap_AudioSessionGetProperty(uint32_t inID, uint32_t* ioDataSize, void* outData) { 
    if (inID == 0x63687372) { // 'chsr' (CurrentHardwareSampleRate)
        if (ioDataSize) *ioDataSize = 8;
        if (outData) { double sr = 44100.0; memcpy(outData, &sr, 8); }
        return 0;
    }
    if (inID == 0x63686f76) { // 'chov' (CurrentHardwareOutputVolume)
        if (ioDataSize) *ioDataSize = 4;
        if (outData) { float vol = 1.0f; memcpy(outData, &vol, 4); }
        return 0;
    }
    if (outData && ioDataSize) memset(outData, 0, *ioDataSize); 
    return 0; 
}

struct HLE_AudioUnit {
    void* renderCallback = nullptr;
    void* renderRefCon = nullptr;
    double sampleRate = 44100.0;
    int channels = 2;
    int bitsPerChannel = 16;
    int bytesPerFrame = 4;
    uint32_t formatFlags = 0xC;
    bool isPlaying = false;
    pthread_t thread = 0;
};
std::map<void*, HLE_AudioUnit*> g_audioUnits;

struct HLE_AudioTimeStamp { double mSampleTime; uint64_t mHostTime; double mRateScalar; uint64_t mWordClockTime; uint32_t SMPTETime[9]; uint32_t mFlags; uint32_t mReserved; };
struct HLE_AudioBuffer { uint32_t mNumberChannels; uint32_t mDataByteSize; void* mData; };
struct HLE_AudioBufferList { uint32_t mNumberBuffers; HLE_AudioBuffer mBuffers[1]; };

void* AudioUnitRenderThread(void* arg) {
    HLE_AudioUnit* au = (HLE_AudioUnit*)arg;
    usleep(100000); 

    JNIEnv* env; g_jvm->AttachCurrentThread(&env, nullptr);
    AudioUnitStreamInitToJava((int)au->sampleRate, au->channels);
    
    uint32_t frames = 1024;
    bool isNonInterleaved = (au->formatFlags & 0x20) != 0;
    int bytesPerSample = au->bitsPerChannel / 8;
    if (bytesPerSample == 0) bytesPerSample = 2;
    
    int numBuffers = isNonInterleaved ? au->channels : 1;
    uint32_t bytesPerBuffer = frames * bytesPerSample * (isNonInterleaved ? 1 : au->channels);
    
    std::vector<std::vector<uint8_t>> buffers(numBuffers);
    for (int i = 0; i < numBuffers; i++) {
        buffers[i].resize(bytesPerBuffer, 0);
    }
    
    size_t listSize = sizeof(uint32_t) + numBuffers * sizeof(HLE_AudioBuffer);
    HLE_AudioBufferList* bufList = (HLE_AudioBufferList*)malloc(listSize);

    uint32_t flags = 0; uint64_t sampleTime = 0;
    typedef int (*AURenderCB)(void*, uint32_t*, HLE_AudioTimeStamp*, uint32_t, uint32_t, HLE_AudioBufferList*);
    AURenderCB cb = (AURenderCB)au->renderCallback;

    bool isValidCode = false;
    if (cb) {
        uintptr_t cbAddr = (uintptr_t)cb & ~1;
        for (const auto& sec : g_machoSections) {
            if (sec.name.find("__text") != std::string::npos || sec.name.find("__TEXT") != std::string::npos) {
                if (cbAddr >= sec.start && cbAddr < sec.end) {
                    isValidCode = true; break;
                }
            }
        }
    }

    if (!isValidCode && cb != nullptr) {
        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "HLE_WARNING: AudioUnit callback 0x%x is NOT in __TEXT segment! Ignoring.", (uint32_t)(uintptr_t)cb);
        LogToJava(logBuf);
    }

    while (au->isPlaying) {
        bufList->mNumberBuffers = numBuffers;
        for (int i = 0; i < numBuffers; i++) {
            bufList->mBuffers[i].mNumberChannels = isNonInterleaved ? 1 : au->channels;
            bufList->mBuffers[i].mDataByteSize = bytesPerBuffer;
            bufList->mBuffers[i].mData = buffers[i].data();
            memset(buffers[i].data(), 0, bytesPerBuffer);
        }
        
        if (isValidCode) {
            HLE_AudioTimeStamp ts = {0}; ts.mSampleTime = sampleTime; ts.mFlags = 1; 
            int status = cb(au->renderRefCon, &flags, &ts, 0, frames, bufList);
            if (status != 0) {
                for (int i = 0; i < numBuffers; i++) memset(buffers[i].data(), 0, bytesPerBuffer);
            }
        }

        int totalSamples = frames * au->channels;
        std::vector<int16_t> pcm16(totalSamples, 0);

        if (au->bitsPerChannel == 32) {
            if (au->formatFlags & 0x1) { // Float32
                for (int ch = 0; ch < au->channels; ch++) {
                    const float* fData = isNonInterleaved ? (const float*)bufList->mBuffers[ch].mData : (const float*)bufList->mBuffers[0].mData + ch;
                    int step = isNonInterleaved ? 1 : au->channels;
                    for (int i = 0; i < frames; i++) {
                        float s = fData[i * step];
                        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                        pcm16[i * au->channels + ch] = (int16_t)(s * 32767.0f);
                    }
                }
            } else { // 8.24 Fixed Point
                for (int ch = 0; ch < au->channels; ch++) {
                    const int32_t* iData = isNonInterleaved ? (const int32_t*)bufList->mBuffers[ch].mData : (const int32_t*)bufList->mBuffers[0].mData + ch;
                    int step = isNonInterleaved ? 1 : au->channels;
                    for (int i = 0; i < frames; i++) {
                        int32_t sample = iData[i * step] >> 9;
                        if (sample > 32767) sample = 32767;
                        else if (sample < -32768) sample = -32768;
                        pcm16[i * au->channels + ch] = (int16_t)sample;
                    }
                }
            }
        } else if (au->bitsPerChannel == 8) {
            for (int ch = 0; ch < au->channels; ch++) {
                const uint8_t* bData = isNonInterleaved ? (const uint8_t*)bufList->mBuffers[ch].mData : (const uint8_t*)bufList->mBuffers[0].mData + ch;
                int step = isNonInterleaved ? 1 : au->channels;
                for (int i = 0; i < frames; i++) {
                    pcm16[i * au->channels + ch] = (int16_t)((bData[i * step] - 128) * 256);
                }
            }
        } else { // Native 16-bit (SMB2)
            for (int ch = 0; ch < au->channels; ch++) {
                const int16_t* sData = isNonInterleaved ? (const int16_t*)bufList->mBuffers[ch].mData : (const int16_t*)bufList->mBuffers[0].mData + ch;
                int step = isNonInterleaved ? 1 : au->channels;
                for (int i = 0; i < frames; i++) {
                    pcm16[i * au->channels + ch] = sData[i * step];
                }
            }
        }
        
        AudioUnitStreamWriteToJava((const uint8_t*)pcm16.data(), pcm16.size() * 2);
        sampleTime += frames;
        usleep(1000); // Оставляем 1000us для идеальной синхронизации SMB2
    }
    free(bufList);
    g_jvm->DetachCurrentThread(); return nullptr;
}

extern "C" int wrap_AudioUnitInitialize(void* inUnit) { if (!g_audioUnits.count(inUnit)) g_audioUnits[inUnit] = new HLE_AudioUnit(); return 0; }
extern "C" int wrap_AudioUnitUninitialize(void* inUnit) { return 0; }
extern "C" int wrap_AudioUnitGetProperty(void* inUnit, uint32_t inID, uint32_t inScope, uint32_t inElement, void* outData, uint32_t* ioDataSize) { 
    if (!ioDataSize) return 0; // По стандарту iOS размер обязателен
    
    if (inID == 8) { // kAudioUnitProperty_StreamFormat
        *ioDataSize = 40; // Важно: сообщаем размер даже если outData == NULL (pre-flight check)
        if (outData) {
            memset(outData, 0, 40);
            double* sampleRate = (double*)outData;
            uint32_t* props = (uint32_t*)((uint8_t*)outData + 8);
            *sampleRate = 44100.0;
            props[0] = 0x6C70636D; // 'lpcm'
            props[1] = 0xC;        // kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
            props[2] = 4;          // mBytesPerPacket
            props[3] = 1;          // mFramesPerPacket
            props[4] = 4;          // mBytesPerFrame
            props[5] = 2;          // mChannelsPerFrame
            props[6] = 16;         // mBitsPerChannel
        }
        return 0;
    }
    if (inID == 14) { // kAudioUnitProperty_MaximumFramesPerSlice
        *ioDataSize = 4;
        if (outData) *(uint32_t*)outData = 4096;
        return 0;
    }
    
    if (outData) memset(outData, 0, *ioDataSize); 
    return 0; 
}
extern "C" int wrap_AudioUnitSetProperty(void* inUnit, uint32_t inID, uint32_t inScope, uint32_t inElement, const void* inData, uint32_t inDataSize) {
    if (!g_audioUnits.count(inUnit)) g_audioUnits[inUnit] = new HLE_AudioUnit();
    HLE_AudioUnit* au = g_audioUnits[inUnit];
    
    if (inID == 23 && inData) { // kAudioUnitProperty_SetRenderCallback
        uint32_t* cbStruct = (uint32_t*)inData;
        au->renderCallback = (void*)cbStruct[0]; au->renderRefCon = (void*)cbStruct[1];
    } else if (inID == 8 && inData) { // kAudioUnitProperty_StreamFormat
        double sr; memcpy(&sr, inData, 8); au->sampleRate = sr;
        uint32_t flags; memcpy(&flags, (uint8_t*)inData + 12, 4); au->formatFlags = flags;
        uint32_t bpf; memcpy(&bpf, (uint8_t*)inData + 24, 4); au->bytesPerFrame = bpf;
        uint32_t ch; memcpy(&ch, (uint8_t*)inData + 28, 4); au->channels = ch;
        uint32_t bpc; memcpy(&bpc, (uint8_t*)inData + 32, 4); au->bitsPerChannel = bpc;
    }
    return 0;
}

extern "C" int wrap_AudioUnitGetParameter(void* inUnit, uint32_t inID, uint32_t inScope, uint32_t inElement, float* outValue) { if (outValue) *outValue = 0.0f; return 0; }
extern "C" int wrap_AudioOutputUnitStart(void* inUnit) {
    if (g_audioUnits.count(inUnit) && !g_audioUnits[inUnit]->isPlaying) {
        HLE_AudioUnit* au = g_audioUnits[inUnit]; au->isPlaying = true;
        pthread_create(&au->thread, nullptr, AudioUnitRenderThread, au);
    }
    return 0;
}
extern "C" int wrap_AudioOutputUnitStop(void* inUnit) {
    if (g_audioUnits.count(inUnit) && g_audioUnits[inUnit]->isPlaying) {
        g_audioUnits[inUnit]->isPlaying = false;
        pthread_join(g_audioUnits[inUnit]->thread, nullptr);
    }
    return 0;
}

// --- FORWARD DECLARATIONS FOR NEW C++ STD WRAPPERS ---
extern "C" size_t wrap_cxx_wstring_find_ptr_len(void*, const int32_t*, size_t);
extern "C" size_t wrap_cxx_wstring_find_char(void*, int32_t, size_t);
extern "C" size_t wrap_cxx_wstring_size(void*);
extern "C" bool wrap_cxx_wstring_empty(void*);
extern "C" size_t wrap_cxx_wstring_rfind_char(void*, int32_t, size_t);
extern "C" void* wrap_cxx_wstring_substr(void*, void*, size_t, size_t);
extern "C" int wrap_cxx_wstring_compare_ptr(void*, const int32_t*);
extern "C" int wrap_cxx_wstring_compare_str(void*, void*);
extern "C" int32_t* wrap_cxx_wstring_operator_index(void*, size_t);
extern "C" size_t wrap_cxx_string_find_first_of_ptr_len(void*, const char*, size_t, size_t);
extern "C" size_t wrap_cxx_string_find_ptr_len(void*, const char*, size_t, size_t); extern "C" size_t wrap_cxx_string_find_ptr(void*, const char*, size_t); extern "C" size_t wrap_cxx_string_rfind_ptr(void*, const char*, size_t);
extern "C" size_t wrap_cxx_string_size(void*);
extern "C" const char* wrap_cxx_string_c_str(void*);
extern "C" bool wrap_cxx_string_empty(void*);
extern "C" size_t wrap_cxx_string_rfind_ptr_len(void*, const char*, size_t, size_t);
extern "C" int wrap_cxx_string_compare_pos_len_ptr(void*, size_t, size_t, const char*);
extern "C" void* wrap_cxx_basic_stringbuf_str(void*, void*);
extern "C" size_t wrap_cxx_locale_id_M_id(void*);
extern "C" char wrap_cxx_basic_ios_widen(void*, char);
extern "C" void* wrap_cxx_wstring_M_leak_hard(void*);
extern "C" int32_t* wrap_cxx_wstring_begin(void*);
extern "C" void* wrap_cxx_wstring_clear(void*);
extern "C" void* wrap_cxx_wstring_erase_iter(void*, int32_t*);
extern "C" void* wrap_cxx_wstring_append_ptr(void*, const int32_t*);
extern "C" void* wrap_cxx_wstring_append_ptr_len(void*, const int32_t*, size_t);
extern "C" void* wrap_cxx_wstring_append_str(void*, void*);
extern "C" void* wrap_cxx_wstring_append_len_char(void*, size_t, int32_t);
extern "C" void wrap_cxx_wstring_M_mutate(void*, size_t, size_t, size_t);
extern "C" void* wrap_cxx_wstring_ctor_ptr_len_alloc(void*, const int32_t*, size_t, void*);
extern "C" void* wrap_cxx_wstring_ctor_str_pos_len(void*, void*, size_t, size_t);
extern "C" void* wrap_cxx_wstring_ctor_len_char_alloc(void*, size_t, int32_t, void*);
extern "C" void* wrap_cxx_wstring_operator_plus_assign_ptr(void*, const int32_t*);
extern "C" void* wrap_cxx_wstring_operator_plus_assign_str(void*, void*);
extern "C" void* wrap_cxx_wstring_operator_plus_assign_char(void*, int32_t);
extern "C" void* wrap_cxx_ostream_put(void*, char);
extern "C" void* wrap_cxx_ostream_flush(void*);
extern "C" void* wrap_cxx_ostream_insert_double(void*, double);
extern "C" void* wrap_cxx_ostream_insert_longlong(void*, long long);
extern "C" void* wrap_cxx_string_Rep_S_create(size_t, size_t, void*);
extern "C" void* wrap_cxx_string_clear(void*);
extern "C" void* wrap_cxx_string_append_ptr(void*, const char*);
extern "C" void* wrap_cxx_string_append_len_char(void*, size_t, char);
extern "C" void* wrap_cxx_string_resize_char(void*, size_t, char);
extern "C" void* wrap_cxx_string_operator_assign(void*, void*);
extern "C" char* wrap_cxx_string_operator_index(void*, size_t);
extern "C" void* wrap_cxx_string_operator_plus_assign(void*, void*);
extern "C" void wrap_List_node_base_hook(void*, void*);
extern "C" void wrap_List_node_base_unhook(void*);
extern "C" void wrap_List_node_base_transfer(void*, void*, void*);
extern "C" void* wrap_cxx_locale_ctor_str(void*, const char*);
extern "C" void* wrap_cxx_locale_ctor(void*);
extern "C" void* wrap_cxx_locale_dtor(void*);
extern "C" void* wrap_cxx_ios_base_init_ctor(void*);
extern "C" void* wrap_cxx_ios_base_init_dtor(void*);
extern "C" void* wrap_cxx_ios_base_ctor(void*);
extern "C" void* wrap_cxx_ios_base_dtor(void*);
extern "C" void wrap_cxx_basic_ios_init(void*, void*);
extern "C" void wrap_cxx_basic_ios_clear(void*, int);
extern "C" void* wrap_cxx_ostream_insert_char_ptr(void*, const char*, int);
extern "C" void wrap_Rb_tree_rebalance_for_erase(void*, void*);
extern "C" void* wrap_cxx_use_facet_ctype_char(void*);
extern "C" void* wrap_cxx_use_facet_ctype_wchar(void*);
extern "C" char* wrap_cxx_string_begin(void*);
extern "C" char* wrap_cxx_string_end(void*);
extern "C" size_t wrap_cxx_string_find_last_of_ptr_len(void*, const char*, size_t);
extern "C" void* wrap_cxx_string_substr(void*, void*, size_t, size_t);
extern "C" int wrap_cxx_string_compare_pos_len_str(void*, size_t, size_t, void*);
extern "C" char* wrap_cxx_string_at(void*, size_t);
extern "C" void* wrap_cxx_string_replace_pos_len_ptr(void*, size_t, size_t, const char*);
extern "C" void* wrap_cxx_string_replace_pos_len_str(void*, size_t, size_t, void*);
extern "C" size_t wrap_cxx_string_find_string(void*, void*, size_t);
extern "C" void* wrap_cxx_string_ctor_len_char(void*, size_t, char, void*);
extern "C" void* wrap_cxx_ostream_insert_string(void*, void*);
extern "C" void* wrap_cxx_ostream_insert_char_ptr_simple(void*, const char*);
extern "C" void* wrap_cxx_ostream_insert_int(void*, int);
extern "C" void* wrap_cxx_ostream_insert_uint(void*, unsigned int);
extern "C" void* wrap_cxx_ostream_insert_ulong(void*, unsigned long);
extern "C" void* wrap_cxx_ostream_iomanip(void*, int);
extern "C" void wrap_List_node_base_swap(void*, void*);
extern "C" void* wrap_new_array_nothrow(size_t, void*);
extern "C" void* wrap_cxx_basic_ios_operator_void_ptr(void*);
extern "C" void* wrap_cxx_getline(void*, void*);
extern "C" void* wrap_cxx_stringstream_ctor(void*, void*, int);
extern "C" void* wrap_cxx_ios_init(void*, void*);
extern char wrap_ZSt7nothrow;

struct HLE_CFBitVector {
    uint32_t isa;
    std::vector<uint8_t> bits;
    uint32_t numBits;
};

extern "C" void* wrap_CFBitVectorCreate(void* alloc, const uint8_t* bytes, uint32_t numBits) {
    HLE_CFBitVector* bv = new HLE_CFBitVector();
    bv->isa = 0xDEADBEEF;
    bv->numBits = numBits;
    uint32_t numBytes = (numBits + 7) / 8;
    if (bytes && numBytes > 0) bv->bits.assign(bytes, bytes + numBytes);
    else bv->bits.resize(numBytes, 0);
    return bv;
}

extern "C" void* wrap_CFBitVectorCreateMutableCopy(void* alloc, uint32_t capacity, void* bv) {
    if (!bv) return nullptr;
    HLE_CFBitVector* src = (HLE_CFBitVector*)bv;
    HLE_CFBitVector* dst = new HLE_CFBitVector();
    dst->isa = src->isa;
    dst->numBits = src->numBits;
    dst->bits = src->bits;
    return dst;
}

extern "C" uint8_t wrap_CFBitVectorGetBitAtIndex(void* bv, uint32_t idx) {
    if (!bv) return 0;
    HLE_CFBitVector* bitVec = (HLE_CFBitVector*)bv;
    if (idx >= bitVec->numBits) return 0;
    return (bitVec->bits[idx / 8] & (1 << (7 - (idx % 8)))) ? 1 : 0;
}

extern "C" void wrap_CFBitVectorSetBitAtIndex(void* bv, uint32_t idx, uint32_t value) {
    if (!bv) return;
    HLE_CFBitVector* bitVec = (HLE_CFBitVector*)bv;
    if (idx >= bitVec->numBits) return;
    if (value) bitVec->bits[idx / 8] |= (1 << (7 - (idx % 8)));
    else bitVec->bits[idx / 8] &= ~(1 << (7 - (idx % 8)));
}

extern "C" char* wrap___strcat_chk(char* dest, const char* src, size_t destlen) {
    if (src && (strstr(src, "pngConf") || strstr(src, "jungle"))) {
        LogToJava(std::string("C-API-DEBUG: [__strcat_chk] Собрана строка: [") + dest + src + "] src: [" + src + "]");
    }
    return strcat(dest, src);
}

extern "C" uLong wrap_crc32(uLong crc, const Bytef *buf, uInt len) { return crc32(crc, buf, len); }
extern "C" int wrap_deflate(z_streamp strm, int flush) { return deflate(strm, flush); }
extern "C" int wrap_deflateEnd(z_streamp strm) { return deflateEnd(strm); }
extern "C" int wrap_deflateInit2_(z_streamp strm, int level, int method, int windowBits, int memLevel, int strategy, const char *version, int stream_size) {
    return deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, stream_size);
}
extern "C" int wrap_inflate(z_streamp strm, int flush) { return inflate(strm, flush); }
extern "C" int wrap_inflateEnd(z_streamp strm) { return inflateEnd(strm); }
extern "C" int wrap_inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size) {
    return inflateInit2_(strm, windowBits, version, stream_size);
}
extern "C" int wrap_inflateInit_(z_streamp strm, const char *version, int stream_size) {
    return inflateInit_(strm, version, stream_size);
}
extern "C" int wrap_inflateReset(z_streamp strm) { return inflateReset(strm); }

#define STB_S(n) {"_" #n, (void*)Stub_##n}
#define STB_W(n) {"_" #n, (void*)wrap_##n}
#define STB_D(n) {"_" #n, (void*)n}
#define STB_V(n) {"_" #n, (void*)&hle_##n}
#define STB_DSTR(n) {"_" #n, (void*)&hle_dummy_str_ptr}
#define STB_DSTRC(n) {"_" #n, (void*)&hle_dummy_struct}

extern "C" size_t wrap_cxx_locale_id_M_id(void*);
extern "C" uint64_t wrap_cxx_ostream_tellp(void*);
extern "C" void wrap_cxx_basic_ios_clear(void*, int);
extern "C" void* wrap_cxx_use_facet_ctype_char(void*);
extern "C" void* wrap_cxx_use_facet_ctype_wchar(void*);

// Предварительные объявления для новых функций
extern "C" int wrap_AudioComponentInstanceDispose(void*);
extern "C" void wrap__dyld_register_func_for_add_image(void (*func)(const void*, intptr_t));
extern "C" int wrap_asprintf(char**, const char*, ...);
extern "C" void* wrap_dlopen(const char*, int);
extern "C" int wrap_dlclose(void*);
extern "C" void* wrap_hash_create(int, void*);
extern "C" void* wrap_hash_search(void*, void*, int);
extern "C" uint8_t wrap_class_addMethod(void*, void*, void*, const char*);
extern "C" uint8_t wrap_class_addProperty(void*, const char*, const void*, uint32_t);
extern "C" uint8_t wrap_class_addProtocol(void*, void*);
extern "C" void* wrap_class_getInstanceVariable(void*, const char*);
extern "C" const uint8_t* wrap_class_getIvarLayout(void*);
extern "C" uint8_t wrap_class_isMetaClass(void*);
extern "C" uint8_t wrap_class_respondsToSelector(void*, void*);
extern "C" void* wrap_objc_getClass(const char*);
extern "C" void* wrap_objc_getMetaClass(const char*);
extern "C" void* wrap_objc_getProtocol(const char*);
extern "C" void* wrap_objc_getRequiredClass(const char*);
extern "C" void wrap_objc_initializeClassPair(void*);
extern "C" void wrap_objc_registerClassPair(void*);
extern "C" void* wrap_object_getIvar(void*, void*);
extern "C" void wrap_object_setIvar(void*, void*, void*);
extern "C" void* wrap_property_copyAttributeList(void*, uint32_t*);
extern "C" void* wrap_sel_getUid(const char*);

std::map<std::string, void*> g_hleStubs = {
    STB_S(UIApplicationMain), STB_S(objc_msgSend), STB_S(objc_msgSendSuper2), STB_S(objc_msgSend_stret), STB_S(objc_msgSendSuper2_stret), STB_S(objc_setProperty), STB_S(NSLog), STB_W(NSLogv),
    STB_S(exit), {"___stack_chk_guard", (void*)&hle_stack_chk_guard_val}, {"___stack_chk_fail", (void*)Stub_exit}, STB_D(sinf), STB_D(cosf), STB_D(tanf), 
    {"_NSDefaultRunLoopMode", (void*)&hle_NSDefaultRunLoopMode_ptr}, {"_kEAGLColorFormatRGBA8", (void*)&hle_kEAGLColorFormatRGBA8_ptr}, {"_kEAGLColorFormatRGB565", (void*)&hle_kEAGLColorFormatRGB565_ptr}, {"_kEAGLDrawablePropertyColorFormat", (void*)&hle_kEAGLDrawablePropertyColorFormat_ptr}, {"_kEAGLDrawablePropertyRetainedBacking", (void*)&hle_kEAGLDrawablePropertyRetainedBacking_ptr}, 
    STB_D(glAttachShader), STB_S(glBindBuffer), STB_S(glBufferData), STB_S(glBufferSubData), STB_S(glDeleteBuffers), STB_S(glBindFramebuffer), {"_glBindFramebufferOES", (void*)Stub_glBindFramebuffer}, STB_S(glBindRenderbuffer), {"_glBindRenderbufferOES", (void*)Stub_glBindRenderbuffer}, STB_S(glBindAttribLocation), {"_glClear", (void*)MegaDebug_glClear}, STB_S(glClearColor), STB_S(glCompileShader),
    STB_D(glCreateProgram), STB_D(glCreateShader), {"_glDrawElements", (void*)MegaDebug_glDrawElements}, {"_glEnable", (void*)MegaDebug_glEnable}, STB_S(glEnableVertexAttribArray), STB_S(glFramebufferRenderbuffer), {"_glFramebufferRenderbufferOES", (void*)Stub_glFramebufferRenderbuffer}, STB_D(glGenFramebuffers), {"_glGenFramebuffersOES", (void*)glGenFramebuffers}, STB_D(glGenRenderbuffers), {"_glGenRenderbuffersOES", (void*)glGenRenderbuffers}, {"_glGetAttribLocation", (void*)MegaDebug_glGetAttribLocation}, STB_S(glBindAttribLocation), {"_glGetBufferParameteriv", (void*)Stub_glGetBufferParameteriv}, {"_glGetShaderInfoLog", (void*)MegaDebug_glGetShaderInfoLog}, STB_D(glGetShaderiv), {"_glGetUniformLocation", (void*)MegaDebug_glGetUniformLocation},
    STB_S(glLinkProgram), STB_S(glRenderbufferStorage), {"_glRenderbufferStorageOES", (void*)Stub_glRenderbufferStorage}, STB_S(glGetRenderbufferParameteriv), {"_glGetRenderbufferParameterivOES", (void*)Stub_glGetRenderbufferParameteriv}, STB_S(glCheckFramebufferStatus), {"_glCheckFramebufferStatusOES", (void*)Stub_glCheckFramebufferStatus}, STB_S(glShaderSource), STB_W(glUniformMatrix4fv), {"_glUseProgram", (void*)MegaDebug_glUseProgram}, STB_S(glVertexAttribPointer), STB_S(glViewport), STB_S(NSHomeDirectory), STB_S(NSTemporaryDirectory), STB_S(NSSearchPathForDirectoriesInDomains),
    STB_W(SCNetworkReachabilityCreateWithName), {"_SCNetworkReachabilityCreateWithAddress", (void*)+[](void* a, void* b) -> void* { return (void*)0xDEADBEEF; }}, {"_SCNetworkReachabilityGetFlags", (void*)+[](void* r, uint32_t* f) -> bool { if(f) *f = 2; return true; }},
    {"_AVAudioSessionCategoryPlayback", (void*)&hle_AVAudioSessionCategoryPlayback_ptr}, {"_AVAudioSessionCategoryAmbient", (void*)&hle_AVAudioSessionCategoryAmbient_ptr}, {"_NSUserDefaultsDidChangeNotification", (void*)&hle_NSUserDefaultsDidChangeNotification_ptr}, {"_kCFCoreFoundationVersionNumber", (void*)&wrap_kCFCoreFoundationVersionNumber}, {"_CGSizeZero", (void*)&wrap_CGSizeZero}, {"_CGPointZero", (void*)&wrap_CGPointZero}, {"__objc_empty_cache", (void*)&hle_objc_empty_cache}, STB_S(objc_enumerationMutation),
    {"__dispatch_main_q", (void*)&hle_dispatch_main_q_struct}, STB_W(dispatch_get_main_queue), STB_W(dispatch_async), STB_W(dispatch_sync), STB_W(dispatch_once),
    STB_W(mach_absolute_time), STB_W(mach_timebase_info), STB_S(CFAbsoluteTimeGetCurrent), {"___cxa_pure_virtual", (void*)Stub_cxa_pure_virtual},
    
    STB_W(malloc), STB_W(free), STB_W(calloc), STB_W(realloc), STB_D(memcpy), STB_D(memmove), STB_D(memset), STB_D(memcmp), {"_memchr", (void*)(void*(*)(void*, int, size_t))memchr},
    STB_D(strcpy), STB_D(strncpy), STB_D(strcmp), STB_D(strncmp), STB_D(strlen), STB_W(strcat), STB_W(strncat), {"_strchr", (void*)(char*(*)(char*, int))strchr}, {"_strrchr", (void*)(char*(*)(char*, int))strrchr}, {"_strstr", (void*)(char*(*)(char*, const char*))strstr},
    STB_D(strdup), STB_D(strcasecmp), STB_D(strncasecmp), STB_D(strcspn), {"_strpbrk", (void*)(char*(*)(char*, const char*))strpbrk},
    STB_D(atoi), STB_D(atof), STB_D(atol), STB_D(strtol), STB_D(strtod), STB_D(strtoul), STB_W(strtoll), STB_D(sprintf), STB_D(snprintf), STB_D(vsprintf), STB_D(vsnprintf), STB_D(sscanf), STB_W(printf), STB_W(puts), STB_D(putchar), STB_W(vprintf), STB_W(vfprintf),
    STB_W(fopen), STB_W(fclose), STB_W(fread), STB_W(fwrite), STB_W(fseek), STB_W(ftell), STB_W(fgetpos), STB_W(fsetpos), STB_W(fputc), STB_W(fscanf), STB_W(fflush), STB_W(fputs), STB_W(fprintf), STB_W(fgetc), STB_W(fgets), STB_W(feof), STB_W(ferror), STB_W(fileno), {"___srget", (void*)wrap___srget},
    {"_sqrt", (void*)(double(*)(double))sqrt}, STB_D(sqrtf), {"_pow", (void*)(double(*)(double, double))pow}, STB_D(powf), {"_exp", (void*)(double(*)(double))exp}, STB_D(expf), {"_log", (void*)(double(*)(double))log}, STB_D(logf), {"_log10", (void*)(double(*)(double))log10}, STB_D(log10f), {"_log2", (void*)(double(*)(double))log2}, STB_D(log2f),
    {"_ceil", (void*)(double(*)(double))ceil}, STB_D(ceilf), {"_floor", (void*)(double(*)(double))floor}, STB_D(floorf), {"_round", (void*)(double(*)(double))round}, STB_D(roundf), {"_fmod", (void*)(double(*)(double, double))fmod}, STB_D(fmodf), {"_fmin", (void*)(double(*)(double, double))fmin}, STB_D(fminf), {"_fmax", (void*)(double(*)(double, double))fmax}, STB_D(fmaxf),
    {"_sin", (void*)(double(*)(double))sin}, {"_cos", (void*)(double(*)(double))cos}, {"_tan", (void*)(double(*)(double))tan}, {"_asin", (void*)(double(*)(double))asin}, {"_acos", (void*)(double(*)(double))acos}, {"_atan", (void*)(double(*)(double))atan}, {"_atan2", (void*)(double(*)(double, double))atan2}, STB_D(atan2f), STB_D(atanf),
    {"_sinh", (void*)(double(*)(double))sinh}, {"_cosh", (void*)(double(*)(double))cosh}, {"_tanh", (void*)(double(*)(double))tanh}, {"_abs", (void*)(int(*)(int))abs}, {"_fabs", (void*)(double(*)(double))fabs}, STB_D(fabsf),
    STB_D(srand), STB_D(srandom), STB_D(random), STB_D(rand), STB_D(gettimeofday), STB_D(clock), STB_D(time), STB_S(bzero),
    STB_W(getenv), STB_D(getcwd), STB_D(chdir), STB_W(stat), STB_W(fstat), STB_W(lstat), STB_D(mkdir), STB_D(remove), STB_D(rename), STB_W(abort), STB_W(mmap), STB_W(munmap), STB_W(rmdir), STB_W(unlink), STB_W(ecvt), STB_W(fcvt), STB_W(fnmatch), STB_W(mbstowcs), STB_W(aio_read), STB_W(aio_error), STB_W(aio_return), STB_W(ioctl), STB_W(select), STB_W(statfs), STB_W(getifaddrs), STB_W(freeifaddrs),
    {"_ldexp", (void*)(double(*)(double, int))ldexp}, STB_D(ldexpf), {"_modf", (void*)(double(*)(double, double*))modf}, STB_D(modff),
    STB_W(pthread_create), STB_D(pthread_join), STB_W(pthread_mutex_init), STB_W(pthread_mutex_lock), STB_W(pthread_mutex_unlock), STB_W(pthread_mutex_destroy), STB_W(pthread_cond_init), STB_W(pthread_cond_wait), STB_W(pthread_cond_signal), STB_W(pthread_cond_broadcast), STB_W(pthread_cond_destroy), STB_D(pthread_self), STB_D(pthread_equal), STB_W(pthread_once), STB_D(pthread_attr_init), STB_D(pthread_attr_destroy), STB_D(pthread_attr_setdetachstate), STB_W(pthread_attr_setstacksize), STB_W(pthread_mach_thread_np), STB_W(thread_get_state), STB_W(thread_set_state), STB_D(pthread_key_create), STB_D(pthread_key_delete), STB_D(pthread_setspecific), STB_D(pthread_getspecific),
    STB_D(sqlite3_open), STB_D(sqlite3_close), STB_D(sqlite3_prepare_v2), STB_D(sqlite3_step), STB_D(sqlite3_finalize), STB_D(sqlite3_bind_int), STB_D(sqlite3_bind_text), STB_D(sqlite3_free_table), STB_D(sqlite3_get_table),
    STB_W(sel_getName), STB_W(sel_registerName), STB_W(setjmp), STB_D(qsort), STB_W(readdir), {"_realpath$DARWIN_EXTSN", (void*)wrap_realpath_darwin}, STB_D(sleep), STB_D(sched_yield),
    STB_W(socket), STB_W(send), STB_W(sendto), STB_W(recv), STB_W(recvfrom), STB_W(setsockopt),
    
    STB_D(acosf), STB_D(asinf), STB_D(strlcpy), STB_D(strtok), STB_D(strerror_r), STB_D(wcscmp), STB_D(wcscpy), STB_D(wcslen), {"_wcschr", (void*)(wchar_t*(*)(wchar_t*, wchar_t))wcschr}, STB_D(wcsncpy), STB_D(wcstombs), STB_D(wcstol), STB_W(memset_pattern16),
    {"_wmemchr", (void*)(wchar_t*(*)(wchar_t*, wchar_t, size_t))wmemchr}, STB_D(wmemcmp), STB_D(wmemcpy), STB_D(wmemmove), STB_D(swprintf), STB_W(vswprintf), STB_W(swscanf), STB_W(wcsncmp), STB_W(wcstof),
    STB_D(close), STB_D(closedir), STB_W(opendir), STB_W(access), STB_D(open), STB_D(read), STB_D(write), STB_D(lseek), STB_D(usleep), STB_D(nanosleep), STB_D(accept), STB_W(bind), STB_W(connect), STB_W(listen),
    {"_div", (void*)(div_t(*)(int, int))div}, STB_D(gethostbyaddr), STB_W(gethostbyname), STB_W(gethostname), STB_D(getnameinfo), STB_D(getpeername), STB_W(getsockname), STB_W(getsockopt), STB_D(if_nametoindex), STB_D(inet_addr),
    STB_W(SecItemAdd), STB_W(SecItemCopyMatching), STB_W(SecItemUpdate), STB_D(inet_aton), STB_D(inet_ntoa), STB_W(longjmp), STB_D(perror), STB_D(sigaction), STB_W(sigprocmask), STB_D(utimes), STB_D(vprintf), STB_W(fcntl), STB_D(system), STB_D(uname), STB_D(dladdr), STB_D(dlsym), STB_D(arc4random), STB_D(localtime), STB_D(localtime_r),
    STB_W(sysctl), STB_W(sysctlbyname), STB_W(sysconf), STB_W(asprintf), STB_W(dlopen), STB_W(dlclose), STB_W(hash_create), STB_W(hash_search),
    
    STB_D(pthread_exit), STB_D(pthread_detach), STB_D(pthread_kill), STB_W(pthread_mutex_trylock), STB_W(pthread_cond_timedwait), STB_D(pthread_getschedparam), STB_D(pthread_setschedparam), STB_D(pthread_mutexattr_init), STB_D(pthread_mutexattr_destroy), STB_D(pthread_mutexattr_settype), STB_D(sched_get_priority_max), STB_D(sched_get_priority_min),
    {"_class_copyMethodList", (void*)Stub_GenericUnimplemented}, STB_W(class_getName), STB_W(class_getInstanceSize), STB_W(class_getSuperclass), {"_class_getProperty", (void*)Stub_GenericUnimplemented}, STB_W(class_getInstanceMethod), STB_W(class_getClassMethod), STB_W(ivar_getName), STB_W(ivar_getOffset), STB_W(method_getImplementation), STB_W(method_getName), STB_W(objc_copyStruct), STB_W(objc_getClassList), STB_W(objc_getProperty), STB_W(objc_lookUpClass), STB_W(object_getClass), STB_W(protocol_getMethodDescription), STB_W(objc_getAssociatedObject), STB_W(objc_setAssociatedObject), STB_W(objc_retain),
    STB_W(class_addMethod), STB_W(class_addProperty), STB_W(class_addProtocol), STB_W(class_getInstanceVariable), STB_W(class_getIvarLayout), STB_W(class_isMetaClass), STB_W(class_respondsToSelector), STB_W(objc_getClass), STB_W(objc_getMetaClass), STB_W(objc_getProtocol), STB_W(objc_getRequiredClass), STB_W(objc_initializeClassPair), STB_W(objc_registerClassPair), STB_W(object_getIvar), STB_W(object_setIvar), STB_W(property_copyAttributeList), STB_W(sel_getUid),
    STB_W(OSAtomicOr32Barrier), STB_W(OSAtomicTestAndClearBarrier), STB_W(OSSpinLockLock), STB_W(OSSpinLockTry), STB_W(OSSpinLockUnlock),

    STB_D(glActiveTexture), STB_S(glBindBuffer), STB_S(glBindTexture),    STB_D(glBlendColor), STB_D(glBlendEquation), {"_glBlendEquationOES", (void*)glBlendEquation}, {"_glBlendFunc", (void*)MegaDebug_glBlendFunc}, {"_glBlendFuncSeparate", (void*)MegaDebug_glBlendFuncSeparate}, {"_glBlendFuncSeparateOES", (void*)MegaDebug_glBlendFuncSeparate}, STB_S(glBufferData), STB_S(glBufferSubData), STB_D(glClearDepthf), STB_S(glCompressedTexImage2D), STB_S(glCompressedTexSubImage2D), STB_D(glCopyTexImage2D), STB_D(glCopyTexSubImage2D), STB_D(glClearStencil), {"_glColorMask", (void*)MegaDebug_glColorMask}, {"_glCullFace", (void*)MegaDebug_glCullFace}, STB_S(glDeleteBuffers), STB_D(glDeleteFramebuffers), {"_glDeleteFramebuffersOES", (void*)glDeleteFramebuffers}, STB_D(glDeleteProgram), STB_D(glDeleteRenderbuffers), {"_glDeleteRenderbuffersOES", (void*)glDeleteRenderbuffers}, STB_D(glDeleteShader), STB_D(glDeleteTextures), {"_glDepthFunc", (void*)MegaDebug_glDepthFunc}, {"_glDepthMask", (void*)MegaDebug_glDepthMask}, STB_D(glDepthRangef), {"_glDisable", (void*)MegaDebug_glDisable}, STB_S(glDisableVertexAttribArray), {"_glDrawArrays", (void*)MegaDebug_glDrawArrays}, STB_D(glFlush), {"_glFramebufferTexture2D", (void*)Stub_glFramebufferTexture2D}, {"_glFramebufferTexture2DOES", (void*)Stub_glFramebufferTexture2D}, {"_glFrontFace", (void*)MegaDebug_glFrontFace}, {"_glGenBuffers", (void*)Stub_glGenBuffers}, STB_D(glGenTextures), STB_D(glGenerateMipmap), {"_glGenerateMipmapOES", (void*)glGenerateMipmap}, STB_D(glGetActiveAttrib), STB_D(glGetActiveUniform), {"_glGetError", (void*)MegaDebug_glGetError}, STB_W(glGetFloatv), {"_glGetIntegerv", (void*)MegaDebug_glGetIntegerv}, {"_glGetProgramInfoLog", (void*)MegaDebug_glGetProgramInfoLog}, STB_D(glGetProgramiv), {"_glGetString", (void*)MegaDebug_glGetString}, STB_D(glHint), STB_D(glLineWidth), STB_W(glMapBufferOES), STB_D(glPixelStorei), STB_D(glPolygonOffset), STB_D(glReadPixels), STB_S(glRenderbufferStorageMultisampleAPPLE), STB_D(glSampleCoverage), STB_W(glScissor), STB_D(glStencilFunc), STB_D(glStencilMask), STB_D(glStencilOp), STB_S(glTexImage2D), STB_D(glTexParameterf), STB_D(glTexParameteri), STB_S(glTexSubImage2D), STB_D(glUniform1f), STB_D(glUniform1fv), STB_W(glUniform1i), STB_D(glUniform1iv),     STB_D(glUniform2fv), STB_D(glUniform2iv), STB_D(glUniform3fv), STB_D(glUniform3iv), STB_W(glUniformMatrix3fv), STB_W(glUniform4fv), STB_D(glUniform4iv), STB_W(glUnmapBufferOES), STB_W(glValidateProgram), {"_glVertexAttrib4f", (void*)Stub_glVertexAttrib4f}, {"_glVertexAttrib4fv", (void*)Stub_glVertexAttrib4fv}, {"_glGetVertexAttribiv", (void*)Stub_glGetVertexAttribiv}, {"_glGetVertexAttribPointerv", (void*)Stub_glGetVertexAttribPointerv},


    STB_W(CFRetain), STB_W(CFRelease), STB_W(CFStringCreateWithCString), STB_W(CFStringGetLength),
 STB_W(CFStringGetCStringPtr), STB_W(CFStringGetCString), STB_W(CFBooleanGetTypeID), STB_W(CFBooleanGetValue), STB_W(CFDataCreate), STB_W(CFDataGetBytePtr), STB_W(CFDataGetBytes), STB_W(CFDataGetLength), STB_W(CFErrorCopyDescription), STB_W(CFGetTypeID), STB_W(CFHTTPMessageCopyHeaderFieldValue), STB_W(CFHTTPMessageCopyResponseStatusLine), STB_W(CFHTTPMessageCopySerializedMessage), STB_W(CFHTTPMessageCreateRequest), STB_W(CFHTTPMessageGetResponseStatusCode), STB_W(CFHTTPMessageSetBody), STB_W(CFHTTPMessageSetHeaderFieldValue), STB_W(CFNumberCreate), STB_W(CFNumberFormatterCreate), STB_W(CFNumberFormatterGetValueFromString), STB_W(CFNumberGetType), STB_W(CFNumberGetTypeID), STB_W(CFNumberGetValue), STB_W(CFPreferencesCopyAppValue), STB_W(CFPreferencesCopyKeyList), STB_W(CFPreferencesSetValue), STB_W(CFPreferencesSynchronize), STB_W(CFReadStreamClose), STB_W(CFReadStreamCreateForHTTPRequest), STB_W(CFReadStreamOpen), STB_W(CFReadStreamScheduleWithRunLoop), STB_W(CFReadStreamSetClient), STB_W(CFReadStreamSetProperty), STB_W(CFRunLoopAddTimer), STB_W(CFRunLoopRunInMode), STB_W(CFRunLoopTimerCreate), STB_W(CFRunLoopTimerInvalidate), STB_W(CFAllocatorGetDefault), STB_W(CFBundleGetVersionNumber), STB_W(CFDictionaryGetValueIfPresent), STB_W(CFNotificationCenterGetLocalCenter), STB_W(CFStringAppend), STB_W(CFStringAppendFormat), STB_W(CFStringAppendCharacters), STB_W(CFStringCompare), STB_W(CFStringConvertEncodingToIANACharSetName), STB_W(CFStringConvertEncodingToNSStringEncoding), STB_W(CFStringConvertIANACharSetNameToEncoding), STB_W(CFStringConvertNSStringEncodingToEncoding), STB_W(CFStringCreateArrayBySeparatingStrings), STB_W(CFStringCreateCopy), STB_W(CFStringCreateMutable), STB_W(CFStringCreateMutableCopy), STB_W(CFStringCreateWithFormat), STB_W(CFStringCreateWithSubstring), STB_W(CFStringFind), STB_W(CFStringHasPrefix), STB_W(CFStringHasSuffix), STB_W(CFURLCreateStringByReplacingPercentEscapesUsingEncoding),
 STB_W(CFStringCreateExternalRepresentation), STB_W(CFStringCreateFromExternalRepresentation), STB_W(CFStringCreateWithCStringNoCopy), STB_W(CFStringCreateWithCharacters), STB_W(CFStringGetCharacters), STB_W(CFStringGetMaximumSizeForEncoding), STB_W(CFStringGetTypeID), STB_W(CFURLCreateFromFileSystemRepresentation), STB_W(CFURLCreateStringByAddingPercentEscapes), STB_W(CFURLCreateStringByReplacingPercentEscapes), STB_W(CFURLCreateWithString), STB_W(CFArrayCreate), STB_W(CFArrayCreateMutable), STB_W(CFArrayGetCount), STB_W(CFArrayGetValueAtIndex), STB_W(CFArrayAppendValue), STB_W(CFArrayRemoveAllValues), STB_W(CFDictionaryCreateMutable), STB_W(CFDictionaryGetValue), STB_W(CFDictionarySetValue), STB_W(CFDictionaryRemoveValue), STB_W(CFBundleGetInfoDictionary), STB_W(CFBundleGetMainBundle), STB_W(CFBundleGetIdentifier), STB_W(CFBundleGetValueForInfoDictionaryKey), STB_W(CFBundleCopyResourcesDirectoryURL), STB_W(CFURLCopyFileSystemPath), STB_W(CFURLGetFileSystemRepresentation), STB_W(CFRunLoopGetCurrent), STB_W(CFRunLoopGetMain), STB_W(CFUUIDCreate), STB_W(CFUUIDCreateString), STB_W(CFTimeZoneCopySystem), STB_W(CFTimeZoneCopyDefault), STB_W(CFTimeZoneGetName), STB_W(CFTimeZoneGetSecondsFromGMT), STB_W(CFAbsoluteTimeGetGregorianDate), STB_W(CFAbsoluteTimeGetDayOfWeek), STB_W(CC_MD5), STB_W(CC_SHA1), STB_W(CC_SHA256), STB_W(CCCrypt), STB_W(CCCryptorCreate), STB_W(CCCryptorUpdate), STB_W(CCCryptorFinal), STB_W(CCCryptorRelease), STB_W(CCCryptorGetOutputLength), STB_W(AudioComponentFindNext), STB_W(AudioComponentInstanceNew), STB_W(UIGraphicsGetCurrentContext), STB_W(UIGraphicsPushContext), STB_W(UIGraphicsPopContext), STB_W(UIGraphicsBeginImageContext), STB_W(UIGraphicsEndImageContext), STB_W(UIGraphicsGetImageFromCurrentImageContext), STB_W(UIRectFill), STB_W(UIImagePNGRepresentation), STB_W(NSAllocateObject), STB_W(NSStringFromClass), STB_W(NSStringFromSelector), STB_W(CFBitVectorCreate), STB_W(CFBitVectorCreateMutableCopy), STB_W(CFBitVectorGetBitAtIndex), STB_W(CFBitVectorSetBitAtIndex),
    STB_V(kCFAllocatorDefault), STB_V(kCFAllocatorNull), STB_V(kCFBooleanFalse), STB_V(kCFBooleanTrue), STB_V(kCFErrorDescriptionKey), STB_V(kCFHTTPVersion1_1), STB_V(kCFPreferencesAnyHost), STB_V(kCFPreferencesCurrentUser), STB_V(kCFStreamPropertyHTTPResponseHeader), STB_V(kCFStreamPropertyHTTPShouldAutoredirect), STB_V(kSecAttrAccessGroup), STB_V(kSecAttrAccount), STB_V(kSecAttrGeneric), STB_V(kSecAttrService), STB_V(kSecClass), STB_V(kSecClassGenericPassword), STB_V(kSecMatchLimit), STB_V(kSecMatchLimitOne), STB_V(kSecReturnAttributes), STB_V(kSecReturnData), STB_V(kSecValueData), STB_V(kCFRunLoopDefaultMode), STB_V(kCFRunLoopCommonModes), STB_V(kCFTypeArrayCallBacks), STB_V(kCFTypeDictionaryKeyCallBacks), STB_V(kCFTypeDictionaryValueCallBacks), STB_V(kCFBundleIdentifierKey), STB_V(kCFNumberNaN), STB_V(kCFNumberNegativeInfinity), STB_V(kCFNumberPositiveInfinity), {"_ADBannerContentSizeIdentifierLandscape", (void*)&hle_ADBannerContentSizeIdentifierLandscape_ptr}, {"_ADBannerContentSizeIdentifierPortrait", (void*)&hle_ADBannerContentSizeIdentifierPortrait_ptr},
    
    STB_W(alcOpenDevice), STB_W(alcCreateContext), STB_W(alcMakeContextCurrent), STB_W(alGenSources), STB_W(alGenBuffers), STB_W(alSourcei), STB_W(alSourcef), STB_W(alBufferData), STB_W(alSourceQueueBuffers), STB_W(alSourcePlay), STB_W(alSourceStop), STB_W(alDeleteBuffers), STB_W(alDeleteSources), STB_W(alGetSourcei), STB_W(alListenerf), STB_W(alSource3f), STB_W(alcCloseDevice), STB_W(alcDestroyContext), STB_W(alcGetString), STB_W(alcProcessContext), STB_W(alcSuspendContext), STB_W(AudioServicesPlaySystemSound), STB_W(AudioServicesCreateSystemSoundID), STB_W(AudioServicesDisposeSystemSoundID), STB_W(CGBitmapContextCreate), STB_W(CGBitmapContextCreateImage), STB_W(CGBitmapContextGetData), STB_W(CGColorSpaceCreateDeviceRGB), STB_W(CGColorSpaceRelease), STB_W(CGContextDrawImage), STB_W(CGImageGetHeight), STB_W(CGImageGetWidth), STB_W(CGImageGetAlphaInfo), STB_W(CGImageGetBitsPerComponent), STB_W(CGContextRelease), STB_W(CGImageRelease), STB_W(CGColorGetComponents), STB_W(CGColorGetColorSpace), STB_W(CGColorSpaceGetModel), STB_W(CGGradientCreateWithColors), STB_W(CGContextSaveGState), STB_W(CGContextRestoreGState), STB_W(CGContextScaleCTM), STB_W(CGContextTranslateCTM), STB_W(CGContextSetFillColor), STB_W(CGContextSetRGBFillColor), STB_W(CGContextSetFillColorWithColor), STB_W(CGContextSetStrokeColor), STB_W(CGContextSetRGBStrokeColor), STB_W(CGContextSetStrokeColorWithColor), STB_W(CGContextSetLineWidth), STB_W(CGContextBeginPath), STB_W(CGContextClosePath), STB_W(CGContextMoveToPoint), STB_W(CGContextAddLineToPoint), STB_W(CGContextAddRect), STB_W(CGContextAddArc), STB_W(CGContextAddArcToPoint), STB_W(CGContextStrokePath), STB_W(CGContextFillPath), STB_W(CGContextFillRect), STB_W(CGContextStrokeLineSegments), STB_W(CGContextSetStrokeColorSpace), STB_W(NSClassFromString), STB_W(NSSelectorFromString),
    {"_SCNetworkReachabilityScheduleWithRunLoop", (void*)+[](void* target, void* runLoop, void* runLoopMode) -> bool { return true; }}, {"_SCNetworkReachabilitySetCallback", (void*)+[](void* target, void* callout, void* context) -> bool { return true; }}, {"_SCNetworkReachabilityUnscheduleFromRunLoop", (void*)+[](void* target, void* runLoop, void* runLoopMode) -> bool { return true; }},
    {"__Block_object_assign", (void*)wrap_Block_object_assign}, {"__Block_copy", (void*)wrap_Block_copy}, {"__Block_release", (void*)wrap_Block_release},

    STB_DSTR(MPMediaItemPropertyAlbumTitle), STB_DSTR(MPMediaItemPropertyArtist), STB_DSTR(MPMediaItemPropertyPersistentID), STB_DSTR(MPMediaItemPropertyPlaybackDuration), STB_DSTR(MPMediaItemPropertyTitle), STB_DSTR(MPMediaLibraryDidChangeNotification), STB_DSTR(MPMediaPlaylistPropertyName), STB_DSTR(MPMoviePlayerContentPreloadDidFinishNotification), STB_DSTR(MPMoviePlayerDidExitFullscreenNotification), STB_DSTR(MPMoviePlayerLoadStateDidChangeNotification), STB_DSTR(MPMoviePlayerPlaybackStateDidChangeNotification), STB_DSTR(MPMoviePlayerScalingModeDidChangeNotification), STB_DSTR(MPMoviePlayerWillExitFullscreenNotification), STB_DSTR(MPMusicPlayerControllerNowPlayingItemDidChangeNotification), STB_DSTR(MPMusicPlayerControllerPlaybackStateDidChangeNotification), STB_DSTR(NSErrorFailingURLStringKey), STB_DSTR(NSFileSystemFreeSize), STB_DSTR(NSFileSystemSize), {"_NSFileModificationDate", (void*)&hle_NSFileModificationDate_ptr}, STB_DSTR(NSGregorianCalendar), STB_DSTR(NSHTTPCookieDomain), STB_DSTR(NSHTTPCookieName), STB_DSTR(NSHTTPCookiePath), STB_DSTR(NSHTTPCookieValue), STB_DSTR(NSInvalidArgumentException), STB_DSTR(NSLocaleCountryCode), STB_DSTR(NSLocaleLanguageCode), STB_DSTR(NSLocalizedDescriptionKey), STB_DSTR(NSLocalizedFailureReasonErrorKey), {"_NSNetServicesErrorCode", (void*)&hle_NSNetServicesErrorCode_ptr}, STB_DSTR(NSUnderlyingErrorKey), STB_DSTR(OBJC_EHTYPE_$_NSException), STB_DSTR(OBJC_EHTYPE_id), STB_DSTR(UIApplicationDidBecomeActiveNotification), STB_DSTR(UIApplicationDidEnterBackgroundNotification), STB_DSTR(UIApplicationDidFinishLaunchingNotification), STB_DSTR(UIApplicationLaunchOptionsRemoteNotificationKey), STB_DSTR(UIApplicationWillEnterForegroundNotification), STB_DSTR(UIApplicationWillResignActiveNotification), STB_DSTR(UIApplicationWillTerminateNotification), {"_GKPlayerAuthenticationDidChangeNotificationName", (void*)&hle_GKPlayerAuthenticationDidChangeNotificationName_ptr}, STB_DSTR(UIDeviceOrientationDidChangeNotification), STB_DSTR(UIKeyboardDidHideNotification), STB_DSTR(UIKeyboardDidShowNotification), STB_DSTR(UIKeyboardWillHideNotification), STB_DSTR(UIKeyboardWillShowNotification), STB_DSTR(UITextFieldTextDidChangeNotification), STB_DSTR(kABPersonEmailProperty), STB_DSTR(kABPersonFirstNameProperty), STB_DSTR(kABPersonLastNameProperty), STB_DSTR(kABPersonPhoneProperty), STB_DSTR(kABWorkLabel), STB_DSTR(kCAMediaTimingFunctionEaseInEaseOut), STB_DSTR(kCAMediaTimingFunctionEaseOut), STB_DSTR(kCAMediaTimingFunctionLinear), STB_DSTR(kCATransitionFromBottom), STB_DSTR(kCATransitionFromLeft), STB_DSTR(kCATransitionFromRight), STB_DSTR(kCATransitionFromTop), STB_DSTR(kCATransitionMoveIn), STB_DSTR(kCATransitionPush), STB_DSTR(kCATransitionReveal), STB_DSTR(kCFBundleVersionKey), STB_DSTR(kCLLocationAccuracyThreeKilometers),

    STB_DSTRC(UIBackgroundTaskInvalid), STB_DSTRC(UIEdgeInsetsZero), STB_DSTRC(UIWindowLevelStatusBar), STB_DSTRC(_DefaultRuneLocale),
    {"__NSConcreteGlobalBlock", (void*)&hle_NSConcreteGlobalBlock_class}, {"__NSConcreteStackBlock", (void*)&hle_NSConcreteStackBlock_class},
    STB_DSTRC(_objc_empty_vtable), {"__ZNSs4_Rep20_S_empty_rep_storageE", (void*)&hle_empty_string_rep}, {"__ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE", (void*)&hle_empty_string_rep},

    {"_CGAffineTransformIdentity", (void*)&wrap_CGAffineTransformIdentity}, STB_W(CGAffineTransformMakeRotation), STB_W(CGAffineTransformMakeScale), STB_W(CGAffineTransformMakeTranslation), STB_W(CGAffineTransformRotate), STB_W(CGAffineTransformScale),     STB_W(CGAffineTransformTranslate), STB_W(CACurrentMediaTime), STB_W(NSStringFromCGSize), STB_W(CGRectContainsPoint), STB_W(CGRectGetHeight),
 STB_W(CGRectGetMidX), STB_W(CGRectGetMidY), STB_W(CGRectGetMinX), STB_W(CGRectGetMinY), STB_W(CGRectGetWidth), STB_W(CGRectInset), STB_W(CGRectIntersection), STB_W(CGRectIsEmpty), STB_W(CGRectIsNull), STB_W(CGRectOffset), {"_CGRectZero", (void*)&wrap_CGRectZero},
    STB_W(glEnableClientState), STB_W(glDisableClientState), STB_W(glVertexPointer), STB_W(glColorPointer), STB_W(glTexCoordPointer), STB_W(glNormalPointer), STB_W(glClientActiveTexture), STB_W(glColor4f), STB_W(glColor4ub), STB_W(glMultiTexCoord4f), STB_W(glNormal3f), STB_W(glAlphaFunc), STB_W(glLoadIdentity), STB_W(glLoadMatrixf), STB_W(glMatrixMode), STB_W(glPopMatrix), STB_W(glPushMatrix), STB_W(glScalef), STB_W(glTranslatef), STB_W(glRotatef), STB_W(glOrthof), STB_W(glFrustumf), STB_W(glMultMatrixf), STB_W(glShadeModel), STB_W(glTexEnvf), STB_W(glTexEnvfv), STB_W(glTexEnvi), STB_W(glClipPlanef), STB_W(glDiscardFramebufferEXT), STB_W(glFogf), STB_W(glFogfv), STB_W(glLightModelf), STB_W(glLightModelfv), STB_W(glLightf), STB_W(glLightfv), STB_W(glMaterialf), STB_W(glMaterialfv),     STB_W(glPointParameterf), STB_W(glPointParameterfv), STB_W(glPointSize), STB_W(glResolveMultisampleFramebufferAPPLE), STB_W(glTexEnvx), STB_W(glCurrentPaletteMatrixOES), STB_W(glLoadPaletteFromModelViewMatrixOES), STB_W(glMatrixIndexPointerOES), STB_W(glWeightPointerOES), STB_W(glPointSizePointerOES),
    {"_glCurrentPaletteMatrix", (void*)wrap_glCurrentPaletteMatrixOES}, {"_glLoadPaletteFromModelViewMatrix", (void*)wrap_glLoadPaletteFromModelViewMatrixOES}, {"_glMatrixIndexPointer", (void*)wrap_glMatrixIndexPointerOES}, {"_glWeightPointer", (void*)wrap_glWeightPointerOES},

    STB_W(__assert_rtn), STB_W(__error), STB_W(__memset_chk), STB_W(strnstr), STB_W(__memcpy_chk), STB_W(__memmove_chk), STB_W(__strcpy_chk), STB_W(__strcat_chk), STB_W(__sprintf_chk), STB_W(__snprintf_chk), STB_W(__tolower), STB_W(__toupper),
    {"__Znwm", (void*)wrap_malloc}, {"__Znwj", (void*)wrap_malloc}, {"__Znam", (void*)wrap_malloc}, {"__Znaj", (void*)wrap_malloc}, {"__ZdlPv", (void*)wrap_free}, {"__ZdaPv", (void*)wrap_free},
    {"___dynamic_cast", (void*)wrap_dynamic_cast}, {"___cxa_throw", (void*)wrap_cxa_throw}, {"___cxa_allocate_exception", (void*)malloc}, {"___cxa_free_exception", (void*)free}, STB_W(__cxa_guard_acquire), STB_W(__cxa_guard_release), STB_W(__cxa_begin_catch), STB_W(__cxa_call_unexpected), STB_W(__cxa_rethrow), STB_W(objc_begin_catch), STB_W(_Unwind_SjLj_Resume), STB_W(dyld_stub_binder), STB_W(__divsi3), STB_W(__fixdfdi), STB_W(__fixsfdi), STB_W(__fixunsdfdi), STB_W(__floatdidf), STB_W(__floatundidf), STB_W(__floatundisf), STB_W(__gxx_personality_sj0), STB_W(__maskrune), STB_W(__modsi3), STB_W(__objc_personality_v0), STB_W(__udivdi3), STB_W(__udivsi3), STB_W(__divdi3), STB_W(__fixunssfdi), STB_W(__cxa_demangle), STB_W(__umoddi3), STB_W(__umodsi3),
    STB_W(crc32), STB_W(deflate), STB_W(deflateEnd), STB_W(deflateInit2_), STB_W(inflate), STB_W(inflateEnd), STB_W(inflateInit2_), STB_W(inflateInit_), STB_W(inflateReset),
    {"__dyld_get_image_header", (void*)Stub_ReturnZero}, {"__dyld_get_image_name", (void*)Stub_ReturnZero}, {"__dyld_image_count", (void*)Stub_ReturnZero}, STB_W(_NSGetExecutablePath), {"__dyld_register_func_for_add_image", (void*)wrap__dyld_register_func_for_add_image},
    
    {"__ZNSsC1Ev", (void*)wrap_cxx_string_default_ctor}, {"__ZNSsC1EPKcRKSaIcE", (void*)wrap_cxx_string_ctor}, {"__ZNSsC1EPKcmRKSaIcE", (void*)wrap_cxx_string_ctor_ptr_len}, {"__ZNSsC1ERKSs", (void*)wrap_cxx_string_copy_ctor}, {"__ZNSsC1ERKSsmm", (void*)wrap_cxx_string_ctor_sub}, {"__ZNKSs5rfindEcm", (void*)wrap_cxx_string_rfind_char}, {"__ZNKSs7compareEPKc", (void*)wrap_cxx_string_compare_char}, {"__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", (void*)wrap_Rb_tree_insert_and_rebalance}, {"__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", (void*)wrap_Rb_tree_decrement}, {"__ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base", (void*)wrap_Rb_tree_decrement}, {"__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", (void*)wrap_Rb_tree_increment}, {"__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", (void*)wrap_Rb_tree_increment}, {"__ZNSsD1Ev", (void*)wrap_cxx_string_dtor}, {"__ZNSsD2Ev", (void*)wrap_cxx_string_dtor}, {"__ZNSs7reserveEm", (void*)wrap_cxx_string_reserve}, {"__ZNSs6appendERKSs", (void*)wrap_cxx_string_append}, {"__ZNSs6appendEPKcm", (void*)wrap_cxx_string_append_ptr_len}, {"__ZNKSs7compareERKSs", (void*)wrap_cxx_string_compare_string}, {"__ZNSs9push_backEc", (void*)wrap_cxx_string_push_back}, {"__ZNSs4_Rep10_M_disposeERKSaIcE", (void*)wrap_cxx_string_dispose}, {"__ZNSs6assignEPKcm", (void*)wrap_cxx_string_assign_ptr_len}, {"__ZNSs6assignERKSs", (void*)wrap_cxx_string_assign_string}, {"__ZNKSs4findEcm", (void*)wrap_cxx_string_find_char}, {"__ZNKSs6lengthEv", (void*)wrap_cxx_string_length}, {"__ZNSsaSEPKc", (void*)wrap_cxx_string_assign_c_str}, {"__ZNSs12_M_leak_hardEv", (void*)wrap_cxx_string_M_leak_hard}, {"__ZNSs9_M_mutateEmmm", (void*)wrap_cxx_string_M_mutate}, {"__ZNSs14_M_replace_auxEmmmc", (void*)wrap_cxx_string_M_replace_aux}, {"__ZNSs4_Rep10_M_destroyERKSaIcE", (void*)wrap_cxx_string_M_destroy}, {"__ZNSaIcEC1Ev", (void*)wrap_allocator_ctor}, {"__ZNSaIcED1Ev", (void*)wrap_allocator_dtor},

    {"__ZNSbIwSt11char_traitsIwESaIwEEC1Ev", (void*)wrap_cxx_wstring_default_ctor}, {"__ZNSbIwSt11char_traitsIwESaIwEEC1EPKwRKSaIwE", (void*)wrap_cxx_wstring_ctor}, {"__ZNSbIwSt11char_traitsIwESaIwEEC1EPKwRKS1_", (void*)wrap_cxx_wstring_ctor}, {"__ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_", (void*)wrap_cxx_wstring_copy_ctor}, {"__ZNSbIwSt11char_traitsIwESaIwEED1Ev", (void*)wrap_cxx_string_dtor}, {"__ZNSbIwSt11char_traitsIwESaIwEED2Ev", (void*)wrap_cxx_string_dtor}, {"__ZNSbIwSt11char_traitsIwESaIwEE7reserveEm", (void*)wrap_cxx_wstring_reserve}, {"__ZNSbIwSt11char_traitsIwESaIwEE6assignEPKwm", (void*)wrap_cxx_wstring_assign_ptr_len}, {"__ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_", (void*)wrap_cxx_string_assign_string}, {"__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_disposeERKSaIwE", (void*)wrap_cxx_string_dispose}, {"__ZNKSbIwSt11char_traitsIwESaIwEE5c_strEv", (void*)wrap_cxx_wstring_c_str}, {"__ZNSbIwSt11char_traitsIwESaIwEEaSEPKw", (void*)wrap_cxx_wstring_assign_c_str}, {"__ZNSbIwSt11char_traitsIwESaIwEEaSERKS2_", (void*)wrap_cxx_wstring_assign_string}, {"__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_destroyERKS1_", (void*)wrap_cxx_string_M_destroy},

    {"__ZNKSbIwSt11char_traitsIwESaIwEE4findEPKwm", (void*)wrap_cxx_wstring_find_ptr_len}, {"__ZNKSbIwSt11char_traitsIwESaIwEE4findEwm", (void*)wrap_cxx_wstring_find_char}, {"__ZNKSbIwSt11char_traitsIwESaIwEE4sizeEv", (void*)wrap_cxx_wstring_size}, {"__ZNKSbIwSt11char_traitsIwESaIwEE5emptyEv", (void*)wrap_cxx_wstring_empty}, {"__ZNKSbIwSt11char_traitsIwESaIwEE5rfindEwm", (void*)wrap_cxx_wstring_rfind_char}, {"__ZNKSbIwSt11char_traitsIwESaIwEE6lengthEv", (void*)wrap_cxx_wstring_size}, {"__ZNKSbIwSt11char_traitsIwESaIwEE6substrEmm", (void*)(void*(*)(void*, void*, size_t, size_t))wrap_cxx_wstring_substr}, {"__ZNKSbIwSt11char_traitsIwESaIwEE7compareEPKw", (void*)wrap_cxx_wstring_compare_ptr}, {"__ZNKSbIwSt11char_traitsIwESaIwEE7compareERKS2_", (void*)wrap_cxx_wstring_compare_str}, {"__ZNKSbIwSt11char_traitsIwESaIwEEixEm", (void*)wrap_cxx_wstring_operator_index}, {"__ZNKSs13find_first_ofEPKcmm", (void*)wrap_cxx_string_find_first_of_ptr_len}, {"__ZNKSs4findEPKcm", (void*)wrap_cxx_string_find_ptr}, {"__ZNKSs4findEPKcmm", (void*)wrap_cxx_string_find_ptr_len}, {"__ZNKSs4sizeEv", (void*)wrap_cxx_string_size}, {"__ZNKSs5c_strEv", (void*)wrap_cxx_string_c_str}, {"__ZNKSs5emptyEv", (void*)wrap_cxx_string_empty}, {"__ZNKSs5rfindEPKcm", (void*)wrap_cxx_string_rfind_ptr}, {"__ZNKSs5rfindEPKcmm", (void*)wrap_cxx_string_rfind_ptr_len}, {"__ZNKSs7compareEmmPKc", (void*)wrap_cxx_string_compare_pos_len_ptr}, {"__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", (void*)wrap_cxx_basic_stringbuf_str}, {"__ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc", (void*)wrap_cxx_basic_ios_widen}, {"__ZNSaIcEC1ERKS_", (void*)wrap_allocator_ctor}, {"__ZNSaIcEC2ERKS_", (void*)wrap_allocator_ctor}, {"__ZNSaIcED2Ev", (void*)wrap_allocator_dtor}, {"__ZNSaIwEC1Ev", (void*)wrap_allocator_ctor}, {"__ZNSaIwED1Ev", (void*)wrap_allocator_dtor}, {"__ZNSbIwSt11char_traitsIwESaIwEE12_M_leak_hardEv", (void*)wrap_cxx_wstring_M_leak_hard}, {"__ZNSbIwSt11char_traitsIwESaIwEE4_Rep11_S_terminalE", (void*)&hle_empty_string_rep}, {"__ZNSbIwSt11char_traitsIwESaIwEE5beginEv", (void*)wrap_cxx_wstring_begin}, {"__ZNSbIwSt11char_traitsIwESaIwEE5clearEv", (void*)wrap_cxx_wstring_clear}, {"__ZNSbIwSt11char_traitsIwESaIwEE5eraseEN9__gnu_cxx17__normal_iteratorIPwS2_EE", (void*)wrap_cxx_wstring_erase_iter}, {"__ZNSbIwSt11char_traitsIwESaIwEE6appendEPKw", (void*)wrap_cxx_wstring_append_ptr}, {"__ZNSbIwSt11char_traitsIwESaIwEE6appendEPKwm", (void*)wrap_cxx_wstring_append_ptr_len}, {"__ZNSbIwSt11char_traitsIwESaIwEE6appendERKS2_", (void*)wrap_cxx_wstring_append_str}, {"__ZNSbIwSt11char_traitsIwESaIwEE6appendEmw", (void*)wrap_cxx_wstring_append_len_char}, {"__ZNSbIwSt11char_traitsIwESaIwEE9_M_mutateEmmm", (void*)wrap_cxx_wstring_M_mutate}, {"__ZNSbIwSt11char_traitsIwESaIwEEC1EPKwmRKS1_", (void*)wrap_cxx_wstring_ctor_ptr_len_alloc}, {"__ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_mm", (void*)wrap_cxx_wstring_ctor_str_pos_len}, {"__ZNSbIwSt11char_traitsIwESaIwEEC1EmwRKS1_", (void*)wrap_cxx_wstring_ctor_len_char_alloc}, {"__ZNSbIwSt11char_traitsIwESaIwEEixEm", (void*)wrap_cxx_wstring_operator_index}, {"__ZNSbIwSt11char_traitsIwESaIwEEpLEPKw", (void*)wrap_cxx_wstring_operator_plus_assign_ptr}, {"__ZNSbIwSt11char_traitsIwESaIwEEpLERKS2_", (void*)wrap_cxx_wstring_operator_plus_assign_str}, {"__ZNSbIwSt11char_traitsIwESaIwEEpLEw", (void*)wrap_cxx_wstring_operator_plus_assign_char}, {"__ZNSo3putEc", (void*)wrap_cxx_ostream_put}, {"__ZNSo5flushEv", (void*)wrap_cxx_ostream_flush}, {"__ZNSo9_M_insertIdEERSoT_", (void*)wrap_cxx_ostream_insert_double}, {"__ZNSo9_M_insertIxEERSoT_", (void*)wrap_cxx_ostream_insert_longlong}, {"__ZNSs4_Rep11_S_terminalE", (void*)&hle_empty_string_rep}, {"__ZNSs4_Rep9_S_createEmmRKSaIcE", (void*)wrap_cxx_string_Rep_S_create}, {"__ZNSs5clearEv", (void*)wrap_cxx_string_clear}, {"__ZNSs6appendEPKc", (void*)wrap_cxx_string_append_ptr}, {"__ZNSs6appendEmc", (void*)wrap_cxx_string_append_len_char}, {"__ZNSs6resizeEmc", (void*)wrap_cxx_string_resize_char}, {"__ZNSsaSERKSs", (void*)wrap_cxx_string_operator_assign}, {"__ZNSsixEm", (void*)wrap_cxx_string_operator_index}, {"__ZNSspLERKSs", (void*)wrap_cxx_string_operator_plus_assign}, {"__ZNSt15_List_node_base4hookEPS_", (void*)wrap_List_node_base_hook}, {"__ZNSt15_List_node_base6unhookEv", (void*)wrap_List_node_base_unhook}, {"__ZNSt15_List_node_base8transferEPS_S0_", (void*)wrap_List_node_base_transfer}, {"__ZNSt6localeC1EPKc", (void*)wrap_cxx_locale_ctor_str}, {"__ZNSt6localeC1Ev", (void*)wrap_cxx_locale_ctor}, {"__ZNSt6localeD1Ev", (void*)wrap_cxx_locale_dtor}, {"__ZNSt8ios_base4InitC1Ev", (void*)wrap_cxx_ios_base_init_ctor}, {"__ZNSt8ios_base4InitD1Ev", (void*)wrap_cxx_ios_base_init_dtor}, {"__ZNSt8ios_baseC2Ev", (void*)wrap_cxx_ios_base_ctor}, {"__ZNSt8ios_baseD2Ev", (void*)wrap_cxx_ios_base_dtor}, {"__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i", (void*)wrap_cxx_ostream_insert_char_ptr}, {"__ZSt16__throw_bad_castv", (void*)wrap_ZSt16__throw_bad_castv}, {"__ZSt17__throw_bad_allocv", (void*)wrap_ZSt17__throw_bad_allocv}, {"__ZSt19__throw_logic_errorPKc", (void*)wrap_ZSt19__throw_logic_errorPKc}, {"__ZSt20__throw_length_errorPKc", (void*)wrap_ZSt20__throw_length_errorPKc}, {"__ZSt20__throw_out_of_rangePKc", (void*)wrap_ZSt20__throw_out_of_rangePKc}, {"__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", (void*)wrap_Rb_tree_rebalance_for_erase},

    {"__ZSt4cerr", (void*)&wrap_ZSt4cerr}, {"__ZSt4cout", (void*)&wrap_ZSt4cout}, {"__ZSt9terminatev", (void*)wrap_abort}, {"__ZTTSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE", (void*)&wrap_ZTTSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE}, {"__ZTVN10__cxxabiv117__class_type_infoE", (void*)&wrap_ZTVN10__cxxabiv117__class_type_infoE}, {"__ZTVN10__cxxabiv120__si_class_type_infoE", (void*)&wrap_ZTVN10__cxxabiv120__si_class_type_infoE}, {"__ZTVN10__cxxabiv121__vmi_class_type_infoE", (void*)&wrap_ZTVN10__cxxabiv121__vmi_class_type_infoE}, {"__ZTVSt15basic_streambufIcSt11char_traitsIcEE", (void*)&wrap_ZTVSt15basic_streambufIcSt11char_traitsIcEE}, {"__ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE", (void*)&wrap_ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE}, {"__ZTVSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE", (void*)&wrap_ZTVSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE}, {"__ZTVSt9basic_iosIcSt11char_traitsIcEE", (void*)&wrap_ZTVSt9basic_iosIcSt11char_traitsIcEE}, {"__ZTISt9bad_alloc", (void*)&wrap_ZTISt9bad_alloc},

    {"___stderrp", (void*)&hle_stderrp_ptr}, {"___stdoutp", (void*)&hle_stdoutp_ptr},

    STB_W(AudioComponentInstanceDispose), STB_W(AudioConverterDispose), STB_W(AudioConverterFillComplexBuffer), STB_W(AudioConverterNew), STB_W(AudioFileClose), STB_W(AudioFileGetProperty), STB_W(AudioFileGetPropertyInfo), STB_W(AudioFileOpenURL), STB_W(AudioFileOpenWithCallbacks), STB_W(AudioFileReadPackets), STB_W(AudioFileReadBytes), STB_W(AudioQueueAllocateBuffer), STB_W(AudioQueueAllocateBufferWithPacketDescriptions), STB_W(AudioQueueDispose), STB_W(AudioQueueEnqueueBuffer), STB_W(AudioQueueEnqueueBufferWithParameters), STB_W(AudioQueueFreeBuffer), STB_W(AudioQueueGetProperty), STB_W(AudioQueueGetCurrentTime), STB_W(AudioQueuePrime), STB_W(AudioQueueSetParameter), STB_W(AudioQueueNewOutput), STB_W(AudioQueuePause), STB_W(AudioQueueStart), STB_W(AudioQueueStop), STB_W(AudioSessionInitialize), STB_W(AudioSessionSetActive), STB_W(AudioSessionAddPropertyListener), STB_W(AudioSessionGetProperty), STB_W(AudioSessionSetProperty), STB_W(AudioUnitInitialize), STB_W(AudioUnitUninitialize), STB_W(AudioUnitGetParameter), STB_W(AudioUnitGetProperty), STB_W(AudioUnitSetProperty), STB_W(AudioOutputUnitStart), STB_W(AudioOutputUnitStop),
    
    {"__ZNKSs12find_last_ofEPKcm", (void*)wrap_cxx_string_find_last_of_ptr_len},
    {"__ZNKSs4findERKSsm", (void*)wrap_cxx_string_find_string},
    {"__ZNKSsixEm", (void*)wrap_cxx_string_operator_index},
    {"__ZNSs7replaceEmmRKSs", (void*)wrap_cxx_string_replace_pos_len_str},
    {"__ZNSsC1EmcRKSaIcE", (void*)wrap_cxx_string_ctor_len_char},
    {"__ZNSspLEPKc", (void*)wrap_cxx_string_append_ptr},
    {"__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", (void*)wrap_cxx_ios_base_ctor},
    {"__ZNKSt18basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv", (void*)wrap_cxx_basic_stringbuf_str},
    {"__ZNKSs3endEv", (void*)wrap_cxx_string_end},
    {"__ZNKSs5beginEv", (void*)wrap_cxx_string_begin},
    {"__ZNKSs6substrEmm", (void*)(void*(*)(void*, void*, size_t, size_t))wrap_cxx_string_substr},
    {"__ZNKSs7compareEmmRKSs", (void*)wrap_cxx_string_compare_pos_len_str},
    {"__ZNKSt19basic_ostringstreamIcSt11char_traitsIcESaIcEE3strEv", (void*)wrap_cxx_basic_stringbuf_str},
    {"__ZNKSt9basic_iosIcSt11char_traitsIcEEcvPvEv", (void*)wrap_cxx_basic_ios_operator_void_ptr},
    {"__ZNSolsEi", (void*)wrap_cxx_ostream_insert_int},
    {"__ZNSolsEj", (void*)wrap_cxx_ostream_insert_uint},
    {"__ZNSolsEm", (void*)wrap_cxx_ostream_insert_ulong},
    {"__ZNSs2atEm", (void*)wrap_cxx_string_at},
    {"__ZNSs3endEv", (void*)wrap_cxx_string_end},
    {"__ZNSs5beginEv", (void*)wrap_cxx_string_begin},
    {"__ZNSs7replaceEmmPKc", (void*)wrap_cxx_string_replace_pos_len_ptr},
    {"__ZNSt15_List_node_base4swapERS_S0_", (void*)wrap_List_node_base_swap},
    {"__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ERKSsSt13_Ios_Openmode", (void*)wrap_cxx_stringstream_ctor},
    {"__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", (void*)wrap_cxx_ios_base_ctor},
    {"__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E", (void*)wrap_cxx_ios_init},
    {"__ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_", (void*)wrap_cxx_getline},
    {"__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc", (void*)wrap_cxx_ostream_insert_char_ptr_simple},
    {"__ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw", (void*)wrap_cxx_ostream_iomanip},
    {"__ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E", (void*)wrap_cxx_ostream_iomanip},
    {"__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E", (void*)wrap_cxx_ostream_insert_string},
    {"__ZnamRKSt9nothrow_t", (void*)wrap_new_array_nothrow},
    {"__ZSt7nothrow", (void*)&wrap_ZSt7nothrow},
    {"__ZNKSt6locale2id5_M_idEv", (void*)wrap_cxx_locale_id_M_id},
    {"__ZNSo5tellpEv", (void*)wrap_cxx_ostream_tellp},
    {"__ZNSt8numpunctIwE2idE", (void*)&wrap_ZNSt8numpunctIwE2idE},
    {"__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate", (void*)wrap_cxx_basic_ios_clear},
    {"__ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale", (void*)wrap_cxx_use_facet_ctype_char},
    {"__ZSt9use_facetISt5ctypeIwEERKT_RKSt6locale", (void*)wrap_cxx_use_facet_ctype_wchar},
};

// Контейнеры для сортировки (map сам сортирует ключи по алфавиту)
std::map<std::string, bool> g_implementedSymbols;
std::map<std::string, bool> g_stubbedSymbols;
bool g_machOLoaded = false; // Флаг, чтобы знать, когда закончилась партионная загрузка

// --- ГЕНЕРАТОР ДИНАМИЧЕСКИХ C-API ЗАГЛУШЕК (JIT TRAMPOLINE) ---
extern "C" void LogDynamicStub(const char* name, uint32_t caller_lr) {
    if (g_disableLogging) return;
    static std::map<std::string, int> stub_counts;
    if (stub_counts[name]++ < 10) {
        LogToJava(std::string("C-API-STUB (DYNAMIC): ") + name + " -> 0 | Caller: " + GetModuleInfoForAddress(caller_lr));
    }
}

void* CreateDynamicStub(const std::string& name) {
    static uint8_t* exec_mem = nullptr;
    static int offset = 0;
    if (!exec_mem) {
        exec_mem = (uint8_t*)mmap(nullptr, 1024 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (exec_mem == MAP_FAILED) {
            LogToJava("FATAL: Не удалось выделить исполняемую память для JIT-заглушек!");
            return (void*)Stub_ReturnZero; // Fallback
        }
    }
    if (offset >= (1024 * 1024) - 64) return (void*)Stub_ReturnZero;
    
    char* name_str = strdup(name.c_str());
    uint32_t* stub = (uint32_t*)(exec_mem + offset);
    
    // ARM32 Машинный код: Безопасный вызов LogDynamicStub(name_str, lr) и возврат 0
    stub[0] = 0xE92D401F; // push {r0, r1, r2, r3, r4, lr}
    stub[1] = 0xE59F0014; // ldr r0, [pc, #20] -> name_str
    stub[2] = 0xE1A0100E; // mov r1, lr -> оригинальный LR
    stub[3] = 0xE59FC010; // ldr r12, [pc, #16] -> LogDynamicStub
    stub[4] = 0xE12FFF3C; // blx r12
    stub[5] = 0xE8BD401F; // pop {r0, r1, r2, r3, r4, lr}
    stub[6] = 0xE3A00000; // mov r0, #0
    stub[7] = 0xE12FFF1E; // bx lr
    stub[8] = (uint32_t)name_str;
    stub[9] = (uint32_t)(void*)LogDynamicStub;
    
    __builtin___clear_cache((char*)stub, (char*)(stub + 10));
    
    offset += 64;
    g_missingSymbolAddrs[(uintptr_t)stub] = name;
    return stub;
}

// --- ТРАМПЛИН ДЛЯ ВЫРАВНИВАНИЯ СТЕКА (АДАПТАЦИЯ IOS ABI -> ANDROID AAPCS) ---
// Спасает драйвера MTK/Mali от SIGBUS/SIGSEGV при вызове тяжелых OpenGL функций
void* CreateAlignedTrampoline(void* real_func) {
    static uint8_t* exec_mem = nullptr;
    static int offset = 0;
    if (!exec_mem) {
        exec_mem = (uint8_t*)mmap(nullptr, 1024 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (exec_mem == MAP_FAILED) return real_func;
    }
    if (offset >= (1024 * 1024) - 80) return real_func;
    
    uint32_t* stub = (uint32_t*)(exec_mem + offset);
    
    stub[0] = 0xE92D000F; // push {r0-r3}           ; Безопасно сохраняем аргументы R0-R3
    stub[1] = 0xE92D4010; // push {r4, lr}          ; Сохраняем R4 и LR
    stub[2] = 0xE1A0400D; // mov r4, sp             ; R4 = оригинальный SP (до выравнивания)
    stub[3] = 0xE3CDD007; // bic sp, sp, #7         ; Выравниваем SP до 8 байт (AAPCS)
    stub[4] = 0xE24DD020; // sub sp, sp, #32        ; Выделяем 32 байта (8 аргументов) на новом стеке
    
    // --- БЕЗОПАСНОЕ КОПИРОВАНИЕ СТЕКА (Без использования R5-R11) ---
    stub[5] = 0xE284C018; // add ip, r4, #24        ; IP указывает на аргументы в оригинальном стеке
    stub[6] = 0xE8BC000F; // ldmia ip!, {r0-r3}     ; Читаем первые 16 байт аргументов (используя R0-R3 как буфер)
    stub[7] = 0xE8AD000F; // stmia sp!, {r0-r3}     ; Пишем их в новый выровненный стек
    stub[8] = 0xE89C000F; // ldmia ip, {r0-r3}      ; Читаем следующие 16 байт
    stub[9] = 0xE88D000F; // stmia sp, {r0-r3}      ; Пишем их
    stub[10]= 0xE24DD010; // sub sp, sp, #16        ; Возвращаем SP на начало выделенных 32 байт
    // ---------------------------------------------------------------
    
    stub[11]= 0xE284C008; // add ip, r4, #8         ; IP указывает на сохраненные в начале R0-R3
    stub[12]= 0xE89C000F; // ldmia ip, {r0-r3}      ; Восстанавливаем оригинальные аргументы для функции
    
    stub[13]= 0xE59FC010; // ldr ip, [pc, #16]      ; Загружаем указатель на C++ обертку
    stub[14]= 0xE12FFF3C; // blx ip                 ; ПРЫЖОК В C++ (С идеальным стеком и целыми регистрами)
    
    stub[15]= 0xE1A0D004; // mov sp, r4             ; Возвращаем старый (невыровненный) SP
    stub[16]= 0xE8BD4010; // pop {r4, lr}           ; Восстанавливаем контекст
    stub[17]= 0xE28DD010; // add sp, sp, #16        ; Снимаем R0-R3 со стека
    stub[18]= 0xE12FFF1E; // bx lr                  ; Возврат в игру
    stub[19]= (uint32_t)real_func;                  // Адрес C++ функции
    
    __builtin___clear_cache((char*)stub, (char*)(stub + 20));
    offset += 80;
    return stub;
}
// --------------------------------------------------------------

void* ResolveSymbol(const std::string& name) {
    if (g_appSymbols.count(name)) return (void*)g_appSymbols[name];
    if (g_appSymbols.count("_" + name)) return (void*)g_appSymbols["_" + name];
    
    std::string hleName = g_hleStubs.count(name) ? name : (g_hleStubs.count("_" + name) ? "_" + name : "");
    if (!hleName.empty()) {
        void* stubPtr = g_hleStubs[hleName];
        
        // ПЕРЕХВАТ: Если функция привязана к массовой заглушке ReturnZero, генерируем для нее уникальный трамплин
        if (stubPtr == (void*)Stub_ReturnZero) {
            if (!g_machOLoaded) g_stubbedSymbols[hleName] = true;
            return CreateDynamicStub(hleName);
        }
        
        if (stubPtr != (void*)Stub_GenericUnimplemented) {
            if (!g_machOLoaded) g_implementedSymbols[hleName] = true;
            else if (!g_implementedSymbols.count(hleName)) { LogToJava("C-API-IMPLEMENTED: " + hleName); g_implementedSymbols[hleName] = true; }
            
            // ВАЖНО: Применяем защиту выравнивания стека ко всем тяжелым функциям OpenGL/OpenAL.
            bool needs_align = (hleName.find("_gl") == 0 || hleName.find("_alc") == 0 || hleName.find("_alS") == 0);
            
            // Исключаем функции, которые используют макрос __builtin_return_address для дебаггера
            if (hleName == "_glGetIntegerv" || hleName == "_glGetFloatv" || hleName == "_glScissor" || 
                hleName == "_glGetRenderbufferParameteriv" || hleName == "_glGetRenderbufferParameterivOES") {
                needs_align = false;
            }
            
            if (needs_align) {
                static std::map<void*, void*> trampolines;
                if (!trampolines.count(stubPtr)) trampolines[stubPtr] = CreateAlignedTrampoline(stubPtr);
                return trampolines[stubPtr];
            }
            
            return stubPtr;
        }
    }
    
    if (name.find("OBJC_CLASS_$_") != std::string::npos || name.find("OBJC_METACLASS_$_") != std::string::npos) {
        std::string clsName = name.substr(name.find("$_") + 2);
        if (!g_hleClasses.count(clsName)) { 
            HLEClass* cls = new HLEClass; cls->magic = 0xDEADBEEF; cls->className = strdup(clsName.c_str()); g_hleClasses[clsName] = cls; 
        }
        return g_hleClasses[clsName];
    }
    
    if (!g_machOLoaded) {
        g_stubbedSymbols[name] = true;
    } else if (!g_stubbedSymbols.count(name)) {
        LogToJava("C-API-STUBBED: " + name);
        g_stubbedSymbols[name] = true;
    }
    
    // Заменяем краш-инструкции 0xE7F001F0 на безопасный динамический трамплин
    return CreateDynamicStub(name);
}

// --- Структуры Mach-O ---
struct fat_header { uint32_t magic; uint32_t nfat_arch; }; struct fat_arch { uint32_t cputype; uint32_t cpusubtype; uint32_t offset; uint32_t size; uint32_t align; }; struct mach_header { uint32_t magic; uint32_t cputype; uint32_t cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; }; struct load_command { uint32_t cmd; uint32_t cmdsize; }; struct segment_command { uint32_t cmd; uint32_t cmdsize; char segname[16]; uint32_t vmaddr; uint32_t vmsize; uint32_t fileoff; uint32_t filesize; uint32_t maxprot; uint32_t initprot; uint32_t nsects; uint32_t flags; }; struct section { char sectname[16]; char segname[16]; uint32_t addr; uint32_t size; uint32_t offset; uint32_t align; uint32_t reloff; uint32_t nreloc; uint32_t flags; uint32_t reserved1; uint32_t reserved2; }; struct symtab_command { uint32_t cmd; uint32_t cmdsize; uint32_t symoff; uint32_t nsyms; uint32_t stroff; uint32_t strsize; }; struct dysymtab_command { uint32_t cmd; uint32_t cmdsize; uint32_t ilocalsym; uint32_t nlocalsym; uint32_t iextdefsym; uint32_t nextdefsym; uint32_t iundefsym; uint32_t nundefsym; uint32_t tocoff; uint32_t ntoc; uint32_t modtaboff; uint32_t nmodtab; uint32_t extrefsymoff; uint32_t nextrefsyms; uint32_t indirectsymoff; uint32_t nindirectsyms; uint32_t extreloff; uint32_t nextrel; uint32_t locreloff; uint32_t nlocrel; }; struct thread_command { uint32_t cmd; uint32_t cmdsize; uint32_t flavor; uint32_t count; }; struct nlist { union { uint32_t n_strx; } n_un; uint8_t n_type; uint8_t n_sect; int16_t n_desc; uint32_t n_value; }; struct dyld_info_command { uint32_t cmd; uint32_t cmdsize; uint32_t rebase_off; uint32_t rebase_size; uint32_t bind_off; uint32_t bind_size; uint32_t weak_bind_off; uint32_t weak_bind_size; uint32_t lazy_bind_off; uint32_t lazy_bind_size; uint32_t export_off; uint32_t export_size; };
struct encryption_info_command { uint32_t cmd; uint32_t cmdsize; uint32_t cryptoff; uint32_t cryptsize; uint32_t cryptid; };
uint64_t read_uleb128(const uint8_t** p) { uint64_t result = 0; int bit = 0; do { result |= ((**p & 0x7f) << bit); bit += 7; } while (*(*p)++ & 0x80); return result; } int64_t read_sleb128(const uint8_t** p) { int64_t result = 0; int bit = 0; uint8_t byte; do { byte = *(*p)++; result |= ((byte & 0x7f) << bit); bit += 7; } while (byte & 0x80); if (byte & 0x40) result |= (-1ULL) << bit; return result; }

void ProcessRebaseOpcodes(int fd, uint32_t arch_offset, uint32_t rebase_off, uint32_t rebase_size, const std::vector<segment_command>& segments, uint32_t slide) {
    std::vector<uint8_t> rebase_data(rebase_size); lseek(fd, arch_offset + rebase_off, SEEK_SET); read(fd, rebase_data.data(), rebase_size);
    const uint8_t* p = rebase_data.data(); const uint8_t* end = p + rebase_size;
    uint32_t segment_idx = 0; uint64_t offset = 0; uint32_t type = 0;
    while (p < end) {
        uint8_t opcode = *p & 0xF0; uint8_t imm = *p & 0x0F; p++;
        switch (opcode) {
            case 0x00: p = end; break;
            case 0x10: type = imm; break;
            case 0x20: segment_idx = imm; offset = read_uleb128(&p); break;
            case 0x30: offset += read_uleb128(&p); break;
            case 0x40: offset += imm * 4; break;
            case 0x50: {
                for (int i = 0; i < imm; i++) {
                    // FIX: guard against out-of-segment offset before write
                    if (segment_idx < segments.size()) { const auto& seg = segments[segment_idx]; if (offset + 4 <= seg.vmsize) { uint32_t addr = seg.vmaddr + slide + offset; *((uint32_t*)addr) += slide; } }
                    offset += 4;
                } break;
            }
            case 0x60: {
                uint64_t count = read_uleb128(&p);
                for (uint64_t i = 0; i < count; i++) {
                    if (segment_idx < segments.size()) { const auto& seg = segments[segment_idx]; if (offset + 4 <= seg.vmsize) { uint32_t addr = seg.vmaddr + slide + offset; *((uint32_t*)addr) += slide; } }
                    offset += 4;
                } break;
            }
            case 0x70: {
                if (segment_idx < segments.size()) { const auto& seg = segments[segment_idx]; if (offset + 4 <= seg.vmsize) { uint32_t addr = seg.vmaddr + slide + offset; *((uint32_t*)addr) += slide; } }
                offset += 4 + read_uleb128(&p); break;
            }
            case 0x80: {
                uint64_t count = read_uleb128(&p); uint64_t skip = read_uleb128(&p);
                for (uint64_t i = 0; i < count; i++) {
                    if (segment_idx < segments.size()) { const auto& seg = segments[segment_idx]; if (offset + 4 <= seg.vmsize) { uint32_t addr = seg.vmaddr + slide + offset; *((uint32_t*)addr) += slide; } }
                    offset += 4 + skip;
                } break;
            }
            default: p = end; break;
        }
    }
}

void ProcessBindOpcodes(int fd, uint32_t arch_offset, uint32_t bind_off, uint32_t bind_size, const std::vector<segment_command>& segments, uint32_t slide) {
    std::vector<uint8_t> bind_data(bind_size); lseek(fd, arch_offset + bind_off, SEEK_SET); read(fd, bind_data.data(), bind_size);
    const uint8_t* p = bind_data.data(); const uint8_t* end = p + bind_size; uint32_t segment_idx = 0; uint64_t offset = 0; std::string symbol_name = "";
    while (p < end) {
        uint8_t opcode = *p & 0xF0; uint8_t imm = *p & 0x0F; p++;
        switch (opcode) {
            case 0x00: case 0x10: case 0x30: case 0x50: break;
            case 0x20: read_uleb128(&p); break;
            case 0x40: symbol_name = (const char*)p; while (*p != '\0') p++; p++; break;
            case 0x60: read_sleb128(&p); break;
            case 0x70: segment_idx = imm; offset = read_uleb128(&p); break;
            case 0x80: offset += read_uleb128(&p); break;
            case 0x90: case 0xA0: case 0xB0: {
                // FIX: guard offset before write
                if (segment_idx < segments.size()) { const auto& seg = segments[segment_idx]; if (offset + 4 <= seg.vmsize) { uint32_t addr = seg.vmaddr + slide + offset; *((uint32_t*)addr) = (uint32_t)ResolveSymbol(symbol_name); } }
                if (opcode == 0x90) offset += 4; else if (opcode == 0xA0) offset += 4 + read_uleb128(&p); else offset += 4 + (imm * 4); break;
            }
            case 0xC0: {
                uint64_t count = read_uleb128(&p); uint64_t skip = read_uleb128(&p);
                for (uint64_t i = 0; i < count; i++) {
                    if (segment_idx < segments.size()) { const auto& seg = segments[segment_idx]; if (offset + 4 <= seg.vmsize) { uint32_t addr = seg.vmaddr + slide + offset; *((uint32_t*)addr) = (uint32_t)ResolveSymbol(symbol_name); } }
                    offset += 4 + skip;
                } break;
            }
            default: p = end; break;
        }
    }
}

// Вспомогательная функция для определения длины Thumb-инструкции
inline size_t GetThumbInstructionSize(uint16_t hw) {
    uint16_t masked = hw & 0xF800;
    if (masked == 0xE800 || masked == 0xF000 || masked == 0xF800) {
        return 4;
    }
    return 2;
}

void LoadMachO(const std::string& bundlePath) {
    HLEClass* cls_NSString = new HLEClass{0xDEADBEEF, "NSString"};    g_hleClasses["NSString"] = cls_NSString; g_hleClasses["__CFConstantStringClassReference"] = cls_NSString; g_hleStubs["___CFConstantStringClassReference"] = cls_NSString;    g_hleClasses["CADisplayLink"] = new HLEClass{0xDEADBEEF, "CADisplayLink"}; g_hleClasses["UIButton"] = new HLEClass{0xDEADBEEF, "UIButton"}; g_hleClasses["UISwitch"] = new HLEClass{0xDEADBEEF, "UISwitch"}; g_hleClasses["UILabel"] = new HLEClass{0xDEADBEEF, "UILabel"}; g_hleClasses["UIColor"] = new HLEClass{0xDEADBEEF, "UIColor"};     g_hleClasses["UIView"] = new HLEClass{0xDEADBEEF, "UIView"}; g_hleClasses["UITextView"] = new HLEClass{0xDEADBEEF, "UITextView"}; g_hleClasses["UITextField"] = new HLEClass{0xDEADBEEF, "UITextField"}; g_hleClasses["UIWindow"] = new HLEClass{0xDEADBEEF, "UIWindow"}; g_hleClasses["UIScreen"] = new HLEClass{0xDEADBEEF, "UIScreen"}; g_hleClasses["UINib"] = new HLEClass{0xDEADBEEF, "UINib"}; g_hleClasses["NSBundle"] = new HLEClass{0xDEADBEEF, "NSBundle"}; g_hleClasses["NSURL"] = new HLEClass{0xDEADBEEF, "NSURL"}; g_hleClasses["NSThread"] = new HLEClass{0xDEADBEEF, "NSThread"};     g_hleClasses["NSURLConnection"] = new HLEClass{0xDEADBEEF, "NSURLConnection"}; g_hleClasses["NSAutoreleasePool"] = new HLEClass{0xDEADBEEF, "NSAutoreleasePool"}; g_hleClasses["GKLocalPlayer"] = new HLEClass{0xDEADBEEF, "GKLocalPlayer"}; g_hleClasses["UIViewController"] = new HLEClass{0xDEADBEEF, "UIViewController"}; g_hleClasses["UIImageView"] = new HLEClass{0xDEADBEEF, "UIImageView"}; g_hleClasses["UIImage"] = new HLEClass{0xDEADBEEF, "UIImage"};
    g_hleClasses["GKAchievement"] = new HLEClass{0xDEADBEEF, "GKAchievement"};
    g_hleClasses["UIPasteboard"] = new HLEClass{0xDEADBEEF, "UIPasteboard"};
    g_hleClasses["UITableView"] = new HLEClass{0xDEADBEEF, "UITableView"};
    g_hleClasses["UITableViewCell"] = new HLEClass{0xDEADBEEF, "UITableViewCell"};
    g_hleClasses["NSIndexPath"] = new HLEClass{0xDEADBEEF, "NSIndexPath"};
    g_hleClasses["UIScrollView"] = new HLEClass{0xDEADBEEF, "UIScrollView"};
    g_hleClasses["UIActivityIndicatorView"] = new HLEClass{0xDEADBEEF, "UIActivityIndicatorView"};
    g_hleClasses["UIBarButtonItem"] = new HLEClass{0xDEADBEEF, "UIBarButtonItem"};
    g_hleClasses["NSMutableDictionary"] = new HLEClass{0xDEADBEEF, "NSMutableDictionary"};
    g_hleClasses["NSRunLoop"] = new HLEClass{0xDEADBEEF, "NSRunLoop"};
    g_hleClasses["UIFont"] = new HLEClass{0xDEADBEEF, "UIFont"};
    g_hleClasses["NSFileManager"] = new HLEClass{0xDEADBEEF, "NSFileManager"};
    g_hleClasses["NSArray"] = new HLEClass{0xDEADBEEF, "NSArray"};
    g_hleClasses["NSDictionary"] = new HLEClass{0xDEADBEEF, "NSDictionary"};
    g_hleClasses["NSNumber"] = new HLEClass{0xDEADBEEF, "NSNumber"};
    g_hleClasses["NSBundle"] = new HLEClass{0xDEADBEEF, "NSBundle"};
    g_hleClasses["NSURL"] = new HLEClass{0xDEADBEEF, "NSURL"};
    g_hleClasses["AVAudioSession"] = new HLEClass{0xDEADBEEF, "AVAudioSession"};
    g_hleClasses["AVAudioPlayer"] = new HLEClass{0xDEADBEEF, "AVAudioPlayer"};
    g_hleClasses["NSData"] = new HLEClass{0xDEADBEEF, "NSData"};
    g_hleClasses["NSURLRequest"] = new HLEClass{0xDEADBEEF, "NSURLRequest"};
    g_hleClasses["NSMutableURLRequest"] = new HLEClass{0xDEADBEEF, "NSMutableURLRequest"};
    g_hleClasses["MPMoviePlayerController"] = new HLEClass{0xDEADBEEF, "MPMoviePlayerController"};
    g_hleClasses["MPMoviePlayerViewController"] = new HLEClass{0xDEADBEEF, "MPMoviePlayerViewController"};
    g_hleClasses["UINavigationController"] = new HLEClass{0xDEADBEEF, "UINavigationController"};
    g_hleClasses["UINavigationBar"] = new HLEClass{0xDEADBEEF, "UINavigationBar"};
    g_hleClasses["NSNotification"] = new HLEClass{0xDEADBEEF, "NSNotification"};
    g_hleClasses["NSNotificationCenter"] = new HLEClass{0xDEADBEEF, "NSNotificationCenter"};
    g_hleClasses["UIApplication"] = new HLEClass{0xDEADBEEF, "UIApplication"};
    g_hleClasses["UIAccelerometer"] = new HLEClass{0xDEADBEEF, "UIAccelerometer"};
    g_hleClasses["CMMotionManager"] = new HLEClass{0xDEADBEEF, "CMMotionManager"};
    g_hleClasses["CMAccelerometerData"] = new HLEClass{0xDEADBEEF, "CMAccelerometerData"};
    g_hleClasses["NSLocale"] = new HLEClass{0xDEADBEEF, "NSLocale"};
    g_hleClasses["UIDevice"] = new HLEClass{0xDEADBEEF, "UIDevice"};
    g_hleClasses["UIScreenMode"] = new HLEClass{0xDEADBEEF, "UIScreenMode"};
    g_hleClasses["SKPaymentQueue"] = new HLEClass{0xDEADBEEF, "SKPaymentQueue"};
    g_hleClasses["NSEnumerator"] = new HLEClass{0xDEADBEEF, "NSEnumerator"};
    g_hleClasses["NSTimer"] = new HLEClass{0xDEADBEEF, "NSTimer"};
    g_hleClasses["NSDate"] = new HLEClass{0xDEADBEEF, "NSDate"};
    if (!hle_MPMoviePlayerPlaybackDidFinishNotification_ptr) {
        hle_MPMoviePlayerPlaybackDidFinishNotification_ptr = CreateNSString("MPMoviePlayerPlaybackDidFinishNotification");
        g_hleStubs["_MPMoviePlayerPlaybackDidFinishNotification"] = (void*)&hle_MPMoviePlayerPlaybackDidFinishNotification_ptr;
        
        hle_MPMoviePlayerContentPreloadDidFinishNotification_ptr = CreateNSString("MPMoviePlayerContentPreloadDidFinishNotification");
        g_hleStubs["_MPMoviePlayerContentPreloadDidFinishNotification"] = (void*)&hle_MPMoviePlayerContentPreloadDidFinishNotification_ptr;
        
        hle_MPMoviePlayerLoadStateDidChangeNotification_ptr = CreateNSString("MPMoviePlayerLoadStateDidChangeNotification");
        g_hleStubs["_MPMoviePlayerLoadStateDidChangeNotification"] = (void*)&hle_MPMoviePlayerLoadStateDidChangeNotification_ptr;
        
        hle_NSURLAuthenticationMethodClientCertificate_ptr = CreateNSString("NSURLAuthenticationMethodClientCertificate");
        g_hleStubs["_NSURLAuthenticationMethodClientCertificate"] = (void*)&hle_NSURLAuthenticationMethodClientCertificate_ptr;
        
        hle_NSURLAuthenticationMethodServerTrust_ptr = CreateNSString("NSURLAuthenticationMethodServerTrust");
        g_hleStubs["_NSURLAuthenticationMethodServerTrust"] = (void*)&hle_NSURLAuthenticationMethodServerTrust_ptr;
        
        hle_NSURLErrorDomain_ptr = CreateNSString("NSURLErrorDomain");
        g_hleStubs["_NSURLErrorDomain"] = (void*)&hle_NSURLErrorDomain_ptr;
        
        hle_NSURLErrorFailingURLErrorKey_ptr = CreateNSString("NSErrorFailingURLKey");
        g_hleStubs["_NSURLErrorFailingURLErrorKey"] = (void*)&hle_NSURLErrorFailingURLErrorKey_ptr;
        
        hle_MPMovieDurationAvailableNotification_ptr = CreateNSString("MPMovieDurationAvailableNotification");
        g_hleStubs["_MPMovieDurationAvailableNotification"] = (void*)&hle_MPMovieDurationAvailableNotification_ptr;
        
        hle_GCControllerDidConnectNotification_ptr = CreateNSString("GCControllerDidConnectNotification");
        g_hleStubs["_GCControllerDidConnectNotification"] = (void*)&hle_GCControllerDidConnectNotification_ptr;
        
        hle_GCControllerDidDisconnectNotification_ptr = CreateNSString("GCControllerDidDisconnectNotification");
        g_hleStubs["_GCControllerDidDisconnectNotification"] = (void*)&hle_GCControllerDidDisconnectNotification_ptr;
        
        hle_UILocalNotificationDefaultSoundName_ptr = CreateNSString("UILocalNotificationDefaultSoundName");
        g_hleStubs["_UILocalNotificationDefaultSoundName"] = (void*)&hle_UILocalNotificationDefaultSoundName_ptr;
    }
    
    if (!hle_NSDefaultRunLoopMode_ptr) {
        hle_NSRunLoopCommonModes_ptr = CreateNSString("NSRunLoopCommonModes");
        g_hleStubs["_NSRunLoopCommonModes"] = (void*)&hle_NSRunLoopCommonModes_ptr;
        hle_NSFileSize_ptr = CreateNSString("NSFileSize");
        g_hleStubs["_NSFileSize"] = (void*)&hle_NSFileSize_ptr;
        hle_NSGenericException_ptr = CreateNSString("NSGenericException");
        g_hleStubs["_NSGenericException"] = (void*)&hle_NSGenericException_ptr;
        hle_NSInternalInconsistencyException_ptr = CreateNSString("NSInternalInconsistencyException");
        g_hleStubs["_NSInternalInconsistencyException"] = (void*)&hle_NSInternalInconsistencyException_ptr;
        hle_NSLocalizedRecoverySuggestionErrorKey_ptr = CreateNSString("NSLocalizedRecoverySuggestionErrorKey");
        g_hleStubs["_NSLocalizedRecoverySuggestionErrorKey"] = (void*)&hle_NSLocalizedRecoverySuggestionErrorKey_ptr;
        hle_UIApplicationDidChangeStatusBarFrameNotification_ptr = CreateNSString("UIApplicationDidChangeStatusBarFrameNotification");
        g_hleStubs["_UIApplicationDidChangeStatusBarFrameNotification"] = (void*)&hle_UIApplicationDidChangeStatusBarFrameNotification_ptr;
        
           hle_kEAGLDrawablePropertyRetainedBacking_ptr = CreateNSString("kEAGLDrawablePropertyRetainedBacking");
        hle_AVAudioSessionCategoryPlayback_ptr = CreateNSString("AVAudioSessionCategoryPlayback");
        hle_AVAudioSessionCategoryAmbient_ptr = CreateNSString("AVAudioSessionCategoryAmbient");
        // FIX: Патчим слоты __nl_symbol_ptr напрямую значениями NSString* (а не адресами переменных).
        // Проблема: в таблице символов мы регистрировали (void*)&hle_..._ptr — адрес переменной.
        // Гостевой код (CDAudioManager) при загрузке _AVAudioSessionCategoryAmbient делает
        // ОДНО разыменование (ldr r0, [slot]) и сразу использует r0 как NSString*.
        // Это даёт r0 = &hle_..._ptr (адрес C++ переменной), а не сам NSString*.
        // GetNSString затем читает ptr[2] из области BSS → невалидный адрес → SIGSEGV.
        // Решение: после инициализации HLE обходим все nl_symbol_ptr слоты и заменяем
        // &hle_ptr → само значение hle_ptr, чтобы одинарный deref давал NSString* напрямую.
        {
            void* target_playback_addr = &hle_AVAudioSessionCategoryPlayback_ptr;
            void* target_ambient_addr  = &hle_AVAudioSessionCategoryAmbient_ptr;
            for (const auto& sect : g_dysymSections) {
                uint32_t* slot = (uint32_t*)sect.addr;
                uint32_t  count = sect.size / 4;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t val = slot[i];
                    if (val == (uint32_t)(uintptr_t)target_playback_addr) {
                        slot[i] = (uint32_t)(uintptr_t)hle_AVAudioSessionCategoryPlayback_ptr;
                        LogToJava("STR-FIX: Пропатчен nl_symbol_ptr слот _AVAudioSessionCategoryPlayback → прямой NSString*");
                    } else if (val == (uint32_t)(uintptr_t)target_ambient_addr) {
                        slot[i] = (uint32_t)(uintptr_t)hle_AVAudioSessionCategoryAmbient_ptr;
                        LogToJava("STR-FIX: Пропатчен nl_symbol_ptr слот _AVAudioSessionCategoryAmbient → прямой NSString*");
                    }
                }
            }
        }
        hle_NSUserDefaultsDidChangeNotification_ptr = CreateNSString("NSUserDefaultsDidChangeNotification");
        hle_GKPlayerAuthenticationDidChangeNotificationName_ptr = CreateNSString("GKPlayerAuthenticationDidChangeNotificationName");
        hle_kCFAllocatorDefault = (void*)1;
        hle_kCFAllocatorNull = (void*)2;
        hle_kCFBooleanFalse = (void*)0;
        hle_kCFBooleanTrue = (void*)1;
        hle_kCFErrorDescriptionKey = CreateNSString("NSLocalizedDescription");
        hle_kCFHTTPVersion1_1 = CreateNSString("HTTP/1.1");
        hle_kCFPreferencesAnyHost = CreateNSString("kCFPreferencesAnyHost");
        hle_kCFPreferencesCurrentUser = CreateNSString("kCFPreferencesCurrentUser");
        hle_kCFStreamPropertyHTTPResponseHeader = CreateNSString("kCFStreamPropertyHTTPResponseHeader");
        hle_kCFStreamPropertyHTTPShouldAutoredirect = CreateNSString("kCFStreamPropertyHTTPShouldAutoredirect");
        hle_kSecAttrAccessGroup = CreateNSString("agrp");
        hle_kSecAttrAccount = CreateNSString("acct");
        hle_kSecAttrGeneric = CreateNSString("gena");
        hle_kSecAttrService = CreateNSString("svce");
        hle_kSecClass = CreateNSString("class");
        hle_kSecClassGenericPassword = CreateNSString("genp");
        hle_kSecMatchLimit = CreateNSString("m_Limit");
        hle_kSecMatchLimitOne = CreateNSString("m_LimitOne");
        hle_kSecReturnAttributes = CreateNSString("r_Attributes");
        hle_kSecReturnData = CreateNSString("r_Data");
        hle_kSecValueData = CreateNSString("v_Data");
        hle_kCFTypeArrayCallBacks = (void*)1;
        hle_kCFTypeDictionaryKeyCallBacks = (void*)1;
        hle_kCFTypeDictionaryValueCallBacks = (void*)1;
        hle_NSFileModificationDate_ptr = CreateNSString("NSFileModificationDate");
        hle_NSNetServicesErrorCode_ptr = CreateNSString("NSNetServicesErrorCode");
        hle_kCFBundleIdentifierKey = CreateNSString("CFBundleIdentifier");
        float nan_val = std::nanf("");
        hle_kCFNumberNaN = wrap_CFNumberCreate(nullptr, 12, &nan_val);
        float ninf_val = -INFINITY;
        hle_kCFNumberNegativeInfinity = wrap_CFNumberCreate(nullptr, 12, &ninf_val);
        float pinf_val = INFINITY;
        hle_kCFNumberPositiveInfinity = wrap_CFNumberCreate(nullptr, 12, &pinf_val);
        hle_kCFRunLoopDefaultMode = CreateNSString("kCFRunLoopDefaultMode");
        hle_kCFRunLoopCommonModes = CreateNSString("kCFRunLoopCommonModes");
        hle_ADBannerContentSizeIdentifierLandscape_ptr = CreateNSString("ADBannerContentSizeIdentifierLandscape");
        hle_ADBannerContentSizeIdentifierPortrait_ptr = CreateNSString("ADBannerContentSizeIdentifierPortrait");
        hle_dummy_str_ptr = CreateNSString("DummyStringConstant");
        hle_stderrp_ptr = stderr;
        hle_stdoutp_ptr = stdout;
    }

    std::string execName = bundlePath.substr(bundlePath.find_last_of('/') + 1); execName = execName.substr(0, execName.find(".app")); std::string execPath = bundlePath + "/" + execName;
    g_execPath = execPath;
    int fd = open(execPath.c_str(), O_RDONLY); if (fd < 0) return;
    
    uint32_t magic; read(fd, &magic, sizeof(magic));
    uint32_t arch_offset = 0; bool isFat = false; bool hasArmv6 = false; bool hasArmv7 = false; bool hasArmv8 = false;
    
    if (magic == 0xbebafeca || magic == 0xcafebabe) {
        isFat = true; uint32_t nfat; read(fd, &nfat, sizeof(nfat)); nfat = __builtin_bswap32(nfat);
        for (uint32_t i = 0; i < nfat; i++) {
            fat_arch fa; read(fd, &fa, sizeof(fa));
            uint32_t type = __builtin_bswap32(fa.cputype); uint32_t subtype = __builtin_bswap32(fa.cpusubtype);
            if (type == 12) {
                if (subtype == 6) hasArmv6 = true;
                if (subtype == 9) { hasArmv7 = true; arch_offset = __builtin_bswap32(fa.offset); }
            } else if (type == 16777228) {
                hasArmv8 = true;
            }
        }
        if (!hasArmv7 && hasArmv6) {
            lseek(fd, 8, SEEK_SET);
            for (uint32_t i = 0; i < nfat; i++) {
                fat_arch fa; read(fd, &fa, sizeof(fa));
                if (__builtin_bswap32(fa.cputype) == 12 && __builtin_bswap32(fa.cpusubtype) == 6) { arch_offset = __builtin_bswap32(fa.offset); break; }
            }
        }
    } else if (magic == 0xfeedface || magic == 0xcefaedfe) {
        mach_header mh; lseek(fd, 0, SEEK_SET); read(fd, &mh, sizeof(mh));
        uint32_t ctype = (magic == 0xcefaedfe) ? __builtin_bswap32(mh.cputype) : mh.cputype;
        uint32_t csubtype = (magic == 0xcefaedfe) ? __builtin_bswap32(mh.cpusubtype) : mh.cpusubtype;
        if (ctype == 12) { 
            if (csubtype == 6) hasArmv6 = true; 
            if (csubtype == 9 || csubtype == 11) hasArmv7 = true; 
        } else if (ctype == 16777228 || ctype == 0x0100000C) {
            hasArmv8 = true;
        }
    }
    
    std::string archStr = "Unknown";
    if (isFat) {
        if (hasArmv7 && hasArmv6) archStr = "ARMv7 + ARMv6 (FAT)";
        else if (hasArmv7) archStr = "ARMv7 (FAT)";
        else if (hasArmv8) archStr = "ARMv8 (FAT)";
        else if (hasArmv6) archStr = "ARMv6 (FAT)";
    } else {
        if (hasArmv7) archStr = "ARMv7"; else if (hasArmv8) archStr = "ARMv8"; else if (hasArmv6) archStr = "ARMv6";
    }

    lseek(fd, arch_offset, SEEK_SET); mach_header mh; read(fd, &mh, sizeof(mh));

    // --- ПЕРВЫЙ ПРОХОД: Вычисляем min/max vmaddr и slide ---
    uint32_t min_vmaddr = 0xFFFFFFFF; uint32_t max_vmaddr = 0;
    uint32_t scan_offset = arch_offset + sizeof(mach_header);
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        load_command lc; lseek(fd, scan_offset, SEEK_SET); read(fd, &lc, sizeof(lc));
        if (lc.cmd == 0x21) { // LC_ENCRYPTION_INFO
            encryption_info_command enc; lseek(fd, scan_offset, SEEK_SET); read(fd, &enc, sizeof(enc));
            if (enc.cryptid != 0) {
                close(fd);
                LogToJava("HLE: Обнаружен Apple DRM (LC_ENCRYPTION_INFO cryptid = " + std::to_string(enc.cryptid) + ")");
                ShowDrmErrorToJava();
                return;
            }
        }
        if (lc.cmd == 1) {
            segment_command seg; lseek(fd, scan_offset, SEEK_SET); read(fd, &seg, sizeof(seg));
            if (seg.vmsize > 0) {
                if (seg.vmaddr < min_vmaddr) min_vmaddr = seg.vmaddr;
                if (seg.vmaddr + seg.vmsize > max_vmaddr) max_vmaddr = seg.vmaddr + seg.vmsize;
            }
        }
        scan_offset += lc.cmdsize;
    }
    g_appSlide = 0;
    if (max_vmaddr > min_vmaddr) {
        if (g_nativeRootMmap) {
            g_appSlide = 0;
        } else {
            // ФИКС: Запрашиваем адрес ниже 2GB (0x80000000), чтобы не сводить с ума 
            // кастомные аллокаторы игр со знаковым битом и тегированием указателей.
            void* base_map = mmap((void*)0x10000000, max_vmaddr - min_vmaddr, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            
            // Если ОС не приняла хинт и дала высокий адрес, пробуем другой пул
            if (base_map != MAP_FAILED && (uintptr_t)base_map >= 0x80000000) {
                munmap(base_map, max_vmaddr - min_vmaddr);
                base_map = mmap((void*)0x20000000, max_vmaddr - min_vmaddr, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            }
            
            // Безусловный фолбек, если всё пошло не так
            if (base_map == MAP_FAILED || (uintptr_t)base_map >= 0x80000000) {
                if (base_map != MAP_FAILED) munmap(base_map, max_vmaddr - min_vmaddr);
                base_map = mmap(nullptr, max_vmaddr - min_vmaddr, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            }

            if (base_map != MAP_FAILED) {
                g_appSlide = (uint32_t)base_map - min_vmaddr;
            }
        }
    }
    // --------------------------------------------------------

    uint32_t cmd_offset = arch_offset + sizeof(mach_header); symtab_command symtab = {0}; dysymtab_command dysymtab = {0}; uint32_t dyld_bind_off = 0, dyld_bind_size = 0; uint32_t dyld_lazy_bind_off = 0, dyld_lazy_bind_size = 0; uint32_t dyld_rebase_off = 0, dyld_rebase_size = 0; std::vector<segment_command> loaded_segments; std::vector<section> dysym_sections; std::vector<section> classlist_sections; std::vector<section> init_func_sections;
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        lseek(fd, cmd_offset, SEEK_SET); load_command lc; read(fd, &lc, sizeof(lc));
        if (lc.cmd == 1) { 
            segment_command seg; lseek(fd, cmd_offset, SEEK_SET); read(fd, &seg, sizeof(seg)); loaded_segments.push_back(seg);
            if (seg.vmsize > 0) {
                uint32_t target_addr = seg.vmaddr + g_appSlide;
                void* mapped = mmap((void*)target_addr, seg.vmsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
                if (mapped != MAP_FAILED && seg.filesize > 0) { lseek(fd, arch_offset + seg.fileoff, SEEK_SET); read(fd, mapped, seg.filesize); }
                uint32_t sect_offset = cmd_offset + sizeof(segment_command);
                for(uint32_t s = 0; s < seg.nsects; s++) { 
                    section sect; lseek(fd, sect_offset, SEEK_SET); read(fd, &sect, sizeof(sect)); 
                    sect.addr += g_appSlide; // Сдвигаем адрес секции
                    uint8_t type = sect.flags & 0xff; 
                    if (type == 6 || type == 7) { dysym_sections.push_back(sect); g_dysymSections.push_back({sect.addr, sect.size}); }
                    if (strncmp(sect.sectname, "__objc_classlist", 16) == 0) classlist_sections.push_back(sect);
                    if (strncmp(sect.sectname, "__mod_init_func", 16) == 0) init_func_sections.push_back(sect);
                    
                    MachOSectionInfo sinfo;
                    sinfo.name = std::string(sect.segname) + "," + std::string(sect.sectname);
                    sinfo.start = sect.addr;
                    sinfo.end = sect.addr + sect.size;
                    g_machoSections.push_back(sinfo);
                    
                    sect_offset += sizeof(section); 
                }
            }
        } else if (lc.cmd == 2) { lseek(fd, cmd_offset, SEEK_SET); read(fd, &symtab, sizeof(symtab));
        } else if (lc.cmd == 11) { lseek(fd, cmd_offset, SEEK_SET); read(fd, &dysymtab, sizeof(dysymtab));
        } else if (lc.cmd == 0x22 || lc.cmd == 0x80000022) { dyld_info_command dyld; lseek(fd, cmd_offset, SEEK_SET); read(fd, &dyld, sizeof(dyld)); dyld_bind_off = dyld.bind_off; dyld_bind_size = dyld.bind_size; dyld_lazy_bind_off = dyld.lazy_bind_off; dyld_lazy_bind_size = dyld.lazy_bind_size; dyld_rebase_off = dyld.rebase_off; dyld_rebase_size = dyld.rebase_size;
        } else if (lc.cmd == 5) { thread_command th; lseek(fd, cmd_offset, SEEK_SET); read(fd, &th, sizeof(th)); if (th.flavor == 1) { struct { uint32_t r[13]; uint32_t sp; uint32_t lr; uint32_t pc; uint32_t cpsr; } state; read(fd, &state, sizeof(state)); g_entryPoint = state.pc + g_appSlide; }
        } else if (lc.cmd == 0xC || lc.cmd == 0x18 || lc.cmd == 0x80000018) {
        }
        cmd_offset += lc.cmdsize;
    }
    if (symtab.cmdsize > 0) {
        std::vector<char> strTable(symtab.strsize); lseek(fd, arch_offset + symtab.stroff, SEEK_SET); read(fd, strTable.data(), symtab.strsize);
        std::vector<nlist> symTable(symtab.nsyms); lseek(fd, arch_offset + symtab.symoff, SEEK_SET); read(fd, symTable.data(), symtab.nsyms * sizeof(nlist));
        bool isES1 = false; bool isES2 = false;
        for (uint32_t i = 0; i < symtab.nsyms; i++) { 
            if (symTable[i].n_un.n_strx > 0) { 
                std::string symName = &strTable[symTable[i].n_un.n_strx]; 
                if (symTable[i].n_sect > 0) g_appSymbols[symName] = symTable[i].n_value + g_appSlide; 
                if (symName == "_glCompileShader") isES2 = true;
                if (symName == "_glEnableClientState" || symName == "_glVertexPointer") isES1 = true;
            } 
        }
        std::string renderStr = "Unknown";
        if (isES1 && isES2) renderStr = "OpenGL ES 1.1/2.0 (Dual)";
        else if (isES2) renderStr = "OpenGL ES 2.0 Only";
        else if (isES1) renderStr = "OpenGL ES 1.1 Only";
        
        if (isES1 && !isES2) g_activeESVersion = 1;
        else if (!isES1 && isES2) g_activeESVersion = 2;
        else if (isES1 && isES2) g_activeESVersion = g_esModeOption;
        else g_activeESVersion = 2;
        
        LogToJava("- Arch: " + archStr);
        LogToJava("- Render: " + renderStr);
        LogToJava("");
        LogToJava("Picked Arch: " + (hasArmv7 ? std::string("ARMv7") : (hasArmv8 ? std::string("ARMv8") : (hasArmv6 ? std::string("ARMv6") : std::string("Unknown")))));
        LogToJava("Picked render: " + (g_activeESVersion == 2 ? std::string("OpenGL ES 2.0") : std::string("OpenGL ES 1.1")));
        
        if (!hasArmv7) {
            close(fd);
            std::string badArch = hasArmv8 ? "8" : (hasArmv6 ? "6" : "?");
            ShowArchErrorToJava(badArch);
            return;
        }

        if (g_nativeRootMmap) {
            LogToJava("HLE: NATIVE ROOT MMAP: Вызов SU для обхода mmap_min_addr...");
            system("su -c 'echo 0 > /proc/sys/vm/mmap_min_addr'");
            LogToJava("HLE: NATIVE ROOT MMAP: Игры работают по оригинальным адресам. Ребазирование отключено.");
        } else if (dyld_rebase_size > 0) {
            LogToJava("HLE: Обнаружен нативный LC_DYLD_INFO rebase, применяем его вместо умного...");
            ProcessRebaseOpcodes(fd, arch_offset, dyld_rebase_off, dyld_rebase_size, loaded_segments, g_appSlide);
                } else {
            // --- TOTAL REBASE WITH EXTREME LOGGING ---
            if (g_appSlide > 0) {
                char hexSlide[32]; snprintf(hexSlide, sizeof(hexSlide), "0x%X", g_appSlide);
                LogToJava(std::string("HLE: Применяем Total Rebase (Custom Parser), смещение: ") + hexSlide);

                // ZLIB PROTECTOR: Сканируем память на наличие магических таблиц Zlib до начала ребейза
                std::vector<std::pair<uint32_t, uint32_t>> zlib_blacklist;
                auto add_to_blacklist = [&](const uint8_t* sig, size_t sig_len, size_t size_to_blacklist, const char* name) {
                    for (const auto& s : g_machoSections) {
                        if (s.name.find("__const") != std::string::npos || s.name.find("__data") != std::string::npos || s.name.find("__common") != std::string::npos) {
                            if (s.end <= s.start || s.end - s.start < sig_len) continue;
                            uint8_t* start = (uint8_t*)(s.start);
                            uint8_t* end = (uint8_t*)(s.end) - sig_len;
                            for (uint8_t* p = start; p <= end; p++) {
                                if (memcmp(p, sig, sig_len) == 0) {
                                    uint32_t found_addr = (uint32_t)p - g_appSlide;
                                    zlib_blacklist.push_back({found_addr, found_addr + size_to_blacklist});
                                    LogToJava(std::string("ZLIB PROTECTOR: Найдена таблица ") + name + " по адресу 0x" + std::to_string(found_addr));
                                }
                            }
                        }
                    }
                };
                
                uint8_t sig_lenfix[] = {0x60, 0x07, 0x00, 0x00, 0x00, 0x08, 0x50, 0x00, 0x00, 0x08, 0x10, 0x00, 0x14, 0x08, 0x73, 0x00, 0x12, 0x07, 0x1F, 0x00};
                add_to_blacklist(sig_lenfix, sizeof(sig_lenfix), 2048, "lenfix");
                uint8_t sig_distfix[] = {0x00, 0x05, 0x00, 0x00, 0x10, 0x05, 0x10, 0x00, 0x08, 0x05, 0x08, 0x00, 0x18, 0x05, 0x18, 0x00, 0x04, 0x05, 0x04, 0x00};
                add_to_blacklist(sig_distfix, sizeof(sig_distfix), 128, "distfix");
                uint8_t sig_order[] = {0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x00, 0x00, 0x08, 0x00, 0x07, 0x00, 0x09, 0x00, 0x06, 0x00, 0x0A, 0x00, 0x05, 0x00};
                add_to_blacklist(sig_order, sizeof(sig_order), 38, "order");
                uint8_t sig_lbase[] = {0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0D, 0x00};
                add_to_blacklist(sig_lbase, sizeof(sig_lbase), 62, "lbase");
                uint8_t sig_lext[] = {0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x11, 0x00, 0x11, 0x00};
                add_to_blacklist(sig_lext, sizeof(sig_lext), 62, "lext");
                uint8_t sig_dbase[] = {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x07, 0x00, 0x09, 0x00, 0x0D, 0x00, 0x11, 0x00, 0x19, 0x00};
                add_to_blacklist(sig_dbase, sizeof(sig_dbase), 64, "dbase");
                uint8_t sig_dext[] = {0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x11, 0x00, 0x11, 0x00, 0x12, 0x00, 0x12, 0x00, 0x13, 0x00, 0x13, 0x00};
                add_to_blacklist(sig_dext, sizeof(sig_dext), 64, "dext");

                LogToJava("CUSTOM-PARSER: Движок успешно инициализирован (ARM_THUMB).");

                uint32_t scan_cmd_offset = arch_offset + sizeof(mach_header);
                for (uint32_t i = 0; i < mh.ncmds; i++) {
                    load_command lc; lseek(fd, scan_cmd_offset, SEEK_SET); read(fd, &lc, sizeof(lc));
                    if (lc.cmd == 1) { // LC_SEGMENT
                        segment_command seg; lseek(fd, scan_cmd_offset, SEEK_SET); read(fd, &seg, sizeof(seg));
                        uint32_t sect_offset = scan_cmd_offset + sizeof(segment_command);
                        for(uint32_t s = 0; s < seg.nsects; s++) {
                            section sect; lseek(fd, sect_offset, SEEK_SET); read(fd, &sect, sizeof(sect));
                            std::string sectname = sect.sectname;
                            
                            bool isCode = (sectname == "__text" || sectname == "__symbol_stub" || sectname == "__stub_helper" || sectname == "__picsymbolstub");
                            bool isString = (sectname == "__cstring" || sectname == "__objc_methname" || sectname == "__objc_classname" || sectname == "__objc_methtype");
                            
                                if (isCode && sect.size > 0) {
                                LogToJava("REBASE-TRACE: Кастомный парсер сканирует секцию: " + sectname + " (размер: " + std::to_string(sect.size) + ")");
                                uint32_t current_addr = sect.addr;
                                const uint8_t* code = (const uint8_t*)(sect.addr + g_appSlide);
                                size_t code_size = sect.size;
                                
                                int parsed_count = 0;
                                int modified_literals = 0;
                                
                                const uint16_t* ptr = (const uint16_t*)code;
                                size_t remaining_bytes = code_size;
                                
                                while (remaining_bytes >= 2) {
                                    uint16_t hw1 = *ptr;
                                    size_t insn_size = GetThumbInstructionSize(hw1);
                                    
                                    if (insn_size == 4 && remaining_bytes < 4) break;
                                    
                                    uint32_t literal_addr = 0;
                                    bool is_ldr_pc = false;
                                    uint32_t target_rd = 0;
                                    
                                    // 16-bit LDR Rx, [PC, #imm8] (0x4800)
                                    if (insn_size == 2 && (hw1 & 0xF800) == 0x4800) {
                                        uint32_t imm8 = hw1 & 0x00FF;
                                        uint32_t offset = imm8 * 4;
                                        uint32_t pc = current_addr + 4; 
                                        literal_addr = (pc & ~3) + offset;
                                        target_rd = (hw1 >> 8) & 7;
                                        is_ldr_pc = true;
                                    }
                                    // 32-bit LDR.W Rx, [PC, +/-imm12] (0xF85F or 0xF8DF)
                                    else if (insn_size == 4) {
                                        uint16_t hw2 = *(ptr + 1);
                                        if ((hw1 & 0xFF7F) == 0xF85F) {
                                            uint32_t offset = hw2 & 0x0FFF;
                                            uint32_t pc = current_addr + 4;
                                            // Проверяем U-бит (направление смещения)
                                            if ((hw1 & 0x0080) == 0) literal_addr = (pc & ~3) - offset;
                                            else literal_addr = (pc & ~3) + offset;
                                            
                                            target_rd = (hw2 >> 12) & 15;
                                            is_ldr_pc = true;
                                        }
                                    }
                                    
                                    if (is_ldr_pc && literal_addr >= min_vmaddr && literal_addr < max_vmaddr) {
                                        // Интеллектуальный Lookahead: Ищем ADD Rd, PC (паттерн позиционно-независимого кода)
                                        bool is_pic_offset = false;
                                        const uint16_t* scan_ptr = ptr + (insn_size / 2);
                                        for (int i = 0; i < 12 && (const uint8_t*)scan_ptr < code + code_size; i++) {
                                            uint16_t scan_hw = *scan_ptr;
                                            if (target_rd < 8 && scan_hw == (0x4478 | target_rd)) {
                                                is_pic_offset = true; break;
                                            } else if (target_rd >= 8 && scan_hw == (0x44F8 | (target_rd & 7))) {
                                                is_pic_offset = true; break;
                                            }
                                            scan_ptr += GetThumbInstructionSize(scan_hw) / 2;
                                        }

                                        // Если это НЕ относительное PIC смещение, значит это абсолютный указатель - делаем ребейз
                                        if (!is_pic_offset) {
                                            bool is_valid_memory = false;
                                            uint32_t shifted_literal = literal_addr + g_appSlide;
                                            for (const auto& sInfo : g_machoSections) {
                                                if (shifted_literal >= sInfo.start && shifted_literal < sInfo.end) {
                                                    is_valid_memory = true;
                                                    break;
                                                }
                                            }
                                            
                                            if (is_valid_memory) {
                                                uint32_t val = 0;
                                                memcpy(&val, (void*)shifted_literal, 4);
                                                
                                                if (val >= min_vmaddr && val < max_vmaddr && val > 0x1000) {
                                                    val += g_appSlide;
                                                    memcpy((void*)shifted_literal, &val, 4);
                                                    modified_literals++;
                                                }
                                            }
                                        }
                                    }
                                    
                                    ptr += (insn_size / 2);
                                    current_addr += insn_size;
                                    remaining_bytes -= insn_size;
                                    parsed_count++;
                                }
                                LogToJava("CUSTOM-PARSER: Успешно разобрано " + std::to_string(parsed_count) + " инструкций.");
                                LogToJava("CUSTOM-PARSER: Изменено абсолютных указателей в пулах: " + std::to_string(modified_literals));
                            } else if (!isString && sect.size > 0) {
                                LogToJava("REBASE-TRACE: Эвристический ребейз секции данных: " + sectname);
                                uint32_t* ptr = (uint32_t*)(sect.addr + g_appSlide);
                                uint32_t count = sect.size / 4;
                                int data_rebased_count = 0;
                                
                                bool is_const_or_data = (sectname == "__const" || sectname == "__data");
                                FILE* f_diag = nullptr;
                                if (is_const_or_data) {
                                    std::string diag_path = g_workDir + "rebase_diag_" + sectname + ".txt";
                                    f_diag = fopen(diag_path.c_str(), "w");
                                    if (f_diag) fprintf(f_diag, "DIAGNOSTICS FOR %s\n", sectname.c_str());
                                }

                                for (uint32_t j = 0; j < count; j++) {
                                    uint32_t val = ptr[j];
                                    
                                    if (val == 18362952 || val == 18321096 || val == 7339996) {
                                        char alert_buf[256];
                                        snprintf(alert_buf, sizeof(alert_buf), "REBASE-ALERT: Целевая переменная %u (0x%X) найдена по адресу 0x%X (секция %s)", val, val, sect.addr + j*4, sectname.c_str());
                                        LogToJava(alert_buf);
                                    }
                                    
                                    if (val >= min_vmaddr && val < max_vmaddr && val > 0x1000) {
                                        bool safe_to_rebase = true;
                                        std::string target_section = "Unknown";
                                        std::string reason = "OK";
                                        uint32_t shifted_val = val + g_appSlide;
                                        for (const auto& sInfo : g_machoSections) {
                                            if (shifted_val >= sInfo.start && shifted_val < sInfo.end) {
                                                target_section = sInfo.name;
                                                break;
                                            }
                                        }

                                        if (target_section != "Unknown") {
                                            bool is_code_target = (target_section.find("__text") != std::string::npos || 
                                                                   target_section.find("__symbol_stub") != std::string::npos || 
                                                                   target_section.find("__stub_helper") != std::string::npos || 
                                                                   target_section.find("__picsymbolstub") != std::string::npos);
                                            
                                            bool is_raw_string_target = (target_section.find("__cstring") != std::string::npos || 
                                                                         target_section.find("__objc_methname") != std::string::npos || 
                                                                         target_section.find("__objc_classname") != std::string::npos || 
                                                                         target_section.find("__objc_methtype") != std::string::npos);
                                                                         
                                            bool is_struct_target = (target_section.find("__cfstring") != std::string::npos || 
                                                                     target_section.find("__objc_const") != std::string::npos || 
                                                                     target_section.find("__objc_classrefs") != std::string::npos || 
                                                                     target_section.find("__objc_selrefs") != std::string::npos || 
                                                                     target_section.find("__objc_data") != std::string::npos || 
                                                                     target_section.find("__objc_superrefs") != std::string::npos || 
                                                                     target_section.find("__objc_protolist") != std::string::npos || 
                                                                     target_section.find("__objc_classlist") != std::string::npos || 
                                                                     target_section.find("__objc_nlclslist") != std::string::npos || 
                                                                     target_section.find("__objc_catlist") != std::string::npos || 
                                                                     target_section.find("__objc_protorefs") != std::string::npos || 
                                                                     target_section.find("__objc_ivar") != std::string::npos || 
                                                                     target_section.find("__gcc_except_tab") != std::string::npos || 
                                                                     target_section.find("__program_vars") != std::string::npos || 
                                                                     target_section.find("__nl_symbol_ptr") != std::string::npos || 
                                                                     target_section.find("__lazy_symbol") != std::string::npos || 
                                                                     target_section.find("__mod_init_func") != std::string::npos);

                                            if (is_code_target) {
                                                // Убрали фильтр "Code: Even": ARM32 функции имеют чётные адреса (бит 0 = 0),
                                                // Thumb-функции — нечётные (бит 0 = 1). Оба варианта валидны в ARMv7 бинарях.
                                                // Вместо фильтра по чётности проверяем что shifted_val попадает в mapped секцию.
                                                if ((val & 1) == 0) {
                                                    // ARM32 pointer — проверяем что адрес (без учёта Thumb-бита) реально в секции
                                                    bool in_mapped = false;
                                                    for (const auto& sInfo : g_machoSections) { if (shifted_val >= sInfo.start && shifted_val < sInfo.end) { in_mapped = true; break; } }
                                                    if (!in_mapped) { safe_to_rebase = false; reason = "Code: Even (Unmapped)"; }
                                                    // иначе — это ARM32 функция, ребейзим
                                                }
                                                else if (((val >> 16) & 0xFFFF) == (val & 0xFFFF)) { safe_to_rebase = false; reason = "Code: Symmetric"; }
                                            } else if (is_raw_string_target) {
                                                // FIX: Trust sv_mapped only. isValidString is unreliable for ObjC string
                                                // sections: __objc_classname__TEXT can have zero-padding before the first
                                                // real string, so isValidString returns false for a perfectly valid name_ptr
                                                // offset. That leaves the pointer un-rebased, and the classlist scanner
                                                // later reads a pre-slide address -> SIGSEGV. If shifted_val falls inside
                                                // a known mapped section it is a legitimate ObjC string pointer; rebase it.
                                                bool sv_mapped = false;
                                                for (const auto& sInfo : g_machoSections) { if (shifted_val >= sInfo.start && shifted_val < sInfo.end) { sv_mapped = true; break; } }
                                                if (!sv_mapped) { safe_to_rebase = false; reason = "String: Unmapped Ptr"; }
                                            } else if (is_struct_target) {
                                                if ((val & 3) != 0) { safe_to_rebase = false; reason = "Struct: Unaligned"; }
                                                else if (target_section.find("__cfstring") != std::string::npos) {
                                                    // FIX: Verify shifted_val is mapped before reading cfstr fields
                                                    bool sv_mapped = false;
                                                    for (const auto& sInfo : g_machoSections) { if (shifted_val >= sInfo.start && (shifted_val + 16) <= sInfo.end) { sv_mapped = true; break; } }
                                                    if (!sv_mapped) { safe_to_rebase = false; reason = "CFString: Unmapped Ptr"; }
                                                    else {
                                                        uint32_t* cfstr = (uint32_t*)shifted_val;
                                                        uint32_t str_ptr = cfstr[2];
                                                        if (str_ptr < min_vmaddr || str_ptr >= max_vmaddr) { safe_to_rebase = false; reason = "CFString: Invalid Ptr"; }
                                                        else {
                                                            uint32_t shifted_str_ptr = str_ptr + g_appSlide;
                                                            bool sp_mapped = false;
                                                            for (const auto& sInfo : g_machoSections) { if (shifted_str_ptr >= sInfo.start && shifted_str_ptr < sInfo.end) { sp_mapped = true; break; } }
                                                            if (!sp_mapped) { safe_to_rebase = false; reason = "CFString: Unmapped Str"; }
                                                            else if (!isValidString((const char*)shifted_str_ptr)) { safe_to_rebase = false; reason = "CFString: Invalid Str"; }
                                                        }
                                                    }
                                                }
                                            } else {
                                                if ((val & 3) != 0) {
                                                    // FIX: Verify shifted_val is mapped before dereferencing
                                                    bool sv_mapped = false;
                                                    for (const auto& sInfo : g_machoSections) { if (shifted_val >= sInfo.start && shifted_val < sInfo.end) { sv_mapped = true; break; } }
                                                    if (!sv_mapped) { safe_to_rebase = false; reason = "Data: Unmapped Ptr"; }
                                                    else if (!isValidString((const char*)shifted_val)) { safe_to_rebase = false; reason = "Data: Unaligned & Bad ASCII"; }
                                                }
                                            }
                                        } else {
                                            safe_to_rebase = false;
                                            reason = "Unknown Target";
                                        }

                                        uint32_t curr_addr = sect.addr + j*4;
                                        for (const auto& range : zlib_blacklist) {
                                            if (curr_addr >= range.first && curr_addr < range.second) {
                                                safe_to_rebase = false;
                                                reason = "Blacklisted (ZLIB)";
                                                break;
                                            }
                                        }

                                        if (f_diag) {
                                            fprintf(f_diag, "ADDR: 0x%08X | VAL: 0x%08X | TGT: %-25s | RESULT: %s (%s)\n", sect.addr + j*4, val, target_section.c_str(), safe_to_rebase ? "REBASED" : "IGNORED", reason.c_str());
                                        }

                                        if (safe_to_rebase) {
                                            ptr[j] = val + g_appSlide;
                                            data_rebased_count++;
                                        }
                                    }
                                }
                                if (f_diag) fclose(f_diag);
                                LogToJava("REBASE-TRACE: Сдвинуто " + std::to_string(data_rebased_count) + " указателей в " + sectname);
                            }
                            sect_offset += sizeof(section);
                        }
                    }
                    scan_cmd_offset += lc.cmdsize;
                }
                LogToJava("CUSTOM-PARSER: Обработка Mach-O завершена.");
            }
            // ------------------------------------
        }

        if (dyld_bind_size > 0) ProcessBindOpcodes(fd, arch_offset, dyld_bind_off, dyld_bind_size, loaded_segments, g_appSlide);
        if (dyld_lazy_bind_size > 0) ProcessBindOpcodes(fd, arch_offset, dyld_lazy_bind_off, dyld_lazy_bind_size, loaded_segments, g_appSlide);
        if (dysymtab.cmdsize > 0) {
            std::vector<uint32_t> indirectSyms(dysymtab.nindirectsyms); lseek(fd, arch_offset + dysymtab.indirectsymoff, SEEK_SET); read(fd, indirectSyms.data(), dysymtab.nindirectsyms * sizeof(uint32_t));
            for (const auto& sect : dysym_sections) {
                uint32_t* ptr_table = (uint32_t*)sect.addr; uint32_t num_pointers = sect.size / 4;
                for (uint32_t i = 0; i < num_pointers; i++) { uint32_t sym_idx = indirectSyms[sect.reserved1 + i]; if (sym_idx == 0x80000000 || sym_idx == 0x40000000) continue; std::string symName = &strTable[symTable[sym_idx].n_un.n_strx]; ptr_table[i] = (uint32_t)ResolveSymbol(symName); }
            }
        }
    }
    cmd_offset = arch_offset + sizeof(mach_header);
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        load_command lc; lseek(fd, cmd_offset, SEEK_SET); read(fd, &lc, sizeof(lc));
        if (lc.cmd == 1) { segment_command seg; lseek(fd, cmd_offset, SEEK_SET); read(fd, &seg, sizeof(seg)); if (seg.vmsize > 0) { int prot = PROT_READ; if (seg.initprot & 2) prot |= PROT_WRITE; if (seg.initprot & 4) prot |= PROT_EXEC; mprotect((void*)(seg.vmaddr + g_appSlide), seg.vmsize, prot); } }
        cmd_offset += lc.cmdsize;
    }
    close(fd); 

    // --- ПАРСИНГ __objc_classlist (Восстановление скрытых/stripped классов) ---
    for (const auto& sect : classlist_sections) {
        uint32_t* ptr_table = (uint32_t*)sect.addr; 
        uint32_t num_classes = sect.size / 4;
        for (uint32_t i = 0; i < num_classes; i++) {
            uint32_t cls_addr = ptr_table[i];
            if (cls_addr > 0x1000) {
                // FIX: verify cls_addr is in a mapped section (guards against un-rebased classlist entry)
                bool cls_mapped = false;
                for (const auto& si : g_machoSections) { if (cls_addr >= si.start && cls_addr < si.end) { cls_mapped = true; break; } }
                if (!cls_mapped) continue;
                uint32_t* cls = (uint32_t*)cls_addr;
                uint32_t data_ptr = cls[4] & ~3;
                if (data_ptr > 0x1000) {
                    // FIX: verify data_ptr is mapped before reading class_ro_t fields
                    bool dp_mapped = false;
                    for (const auto& si : g_machoSections) { if (data_ptr >= si.start && (data_ptr + 20) <= si.end) { dp_mapped = true; break; } }
                    if (!dp_mapped) continue;
                    uint32_t name_ptr = ((uint32_t*)data_ptr)[4];
                    // FIX: verify name_ptr is in a mapped section before passing to isValidString
                    bool np_mapped = false;
                    for (const auto& si : g_machoSections) { if (name_ptr >= si.start && name_ptr < si.end) { np_mapped = true; break; } }
                    if (np_mapped && isValidString((const char*)name_ptr)) {
                        std::string cName = (const char*)name_ptr;
                        g_appSymbols["_OBJC_CLASS_$_" + cName] = cls_addr;
                        if (g_logHiddenClasses) {
                            LogToJava("OBJC-CLASS-FOUND: Нашёлся скрытый класс " + cName);
                        }
                    }
                }
            }
        }
    }

    // --- ПАРСИНГ __mod_init_func (Сбор C++ конструкторов) ---
    for (const auto& sect : init_func_sections) {
        uint32_t* ptr_table = (uint32_t*)sect.addr; 
        uint32_t num_funcs = sect.size / 4;
        for (uint32_t i = 0; i < num_funcs; i++) {
            uint32_t func_addr = ptr_table[i];
            if (func_addr > 0x1000) g_initFuncs.push_back(func_addr);
        }
    }
    
    // --- ВЫВОД ОТСОРТИРОВАННОГО КЭША C-API ---
    if (g_logFuncList) {
        std::string batchLog = "\n=== РЕАЛИЗОВАННЫЕ C-API СИМВОЛЫ ===\n";
        int counter = 0;
        for (auto const& pair : g_implementedSymbols) {
            batchLog += "C-API-IMPLEMENTED: " + pair.first + "\n";
            if (++counter >= 100) { LogToJava(batchLog); batchLog = ""; counter = 0; }
        }
        if (!batchLog.empty()) { LogToJava(batchLog); batchLog = ""; counter = 0; }

        batchLog = "\n=== C-API ЗАГЛУШКИ (STUBS) ===\n";
        for (auto const& pair : g_stubbedSymbols) {
            batchLog += "C-API-STUBBED: " + pair.first + "\n";
            if (++counter >= 100) { LogToJava(batchLog); batchLog = ""; counter = 0; }
        }
        batchLog += "==================================\n";
        if (!batchLog.empty()) LogToJava(batchLog);
    }

    g_machOLoaded = true; // С этого момента любые новые функции пишутся в лог мгновенно
    
    SwitchToRenderView();
}

void* NativeExecutionThread(void* arg) {
    LogToJava("NativeExecutionThread: Поток запущен, настраиваем EGL...");
    if (!eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext)) { LogToJava("КРИТИЧЕСКАЯ ОШИБКА: eglMakeCurrent не сработал!"); return nullptr; }
    
    // --- MEGA DEBUG: ПРОВЕРКА СОСТОЯНИЯ EGL СРАЗУ ПОСЛЕ ИНИЦИАЛИЗАЦИИ ---
    EGLContext ctx = eglGetCurrentContext();
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    char eglBuf[256];
    snprintf(eglBuf, sizeof(eglBuf), "[MEGA-DEBUG] NativeExecutionThread init: Ctx=%p, Dpy=%p, Surf=%p, SurfSize=%dx%d", ctx, dpy, surf, w, h);
    LogToJava(eglBuf);
    // --------------------------------------------------------------------

    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    
    if (g_entryPoint == 0) { 
        LogToJava("КРИТИЧЕСКАЯ ОШИБКА: g_entryPoint равен 0!"); 
        return nullptr; 
    }
    
    LogToJava("NativeExecutionThread: Подготовка к прыжку в сырой _start (XNU ABI). g_entryPoint = 0x" + std::to_string(g_entryPoint));
    LogToJava("NativeExecutionThread: ---> ПРЫЖОК В IOS <---");

#if defined(__arm__)
    const char* exe_name = g_execPath.c_str();
    uint32_t entry = g_entryPoint;
    
    // ВАЖНО: Стартовая точка iOS-приложений (crt1.o _start) не использует C-соглашение о вызовах!
    // ИСПРАВЛЕНИЕ: ЖЕСТКОЕ ВЫРАВНИВАНИЕ СТЕКА К 8 БАЙТАМ для защиты драйверов MediaTek/Mali
    asm volatile (
        "mov r0, #0\n"
        "mov r12, sp\n"
        "bic r12, r12, #7\n"
        "mov sp, r12\n"
   // <--- КРИТИЧЕСКИЙ ФИКС: Сбрасываем 3 младших бита SP (выравнивание до 8)
        "push {r0}\n"        // Padding для выравнивания стека (8 байт по стандарту ARM AAPCS)
        "push {r0}\n"        // apple[0] (доп. данные Mach-O, оставляем NULL)
        "push {r0}\n"        // envp[0]  (переменные окружения, NULL)
        "push {r0}\n"        // argv[1]  (конец списка аргументов, NULL)
        "push {%0}\n"        // argv[0]  (Указатель на имя экзешника)
        "mov r0, #1\n"       // argc = 1
        "push {r0}\n"        // Кладем argc на саму вершину стека
        "bx %1\n"            // Прыгаем в iOS! Функция _start сама вызовет exit() в конце.
        :: "r"(exe_name), "r"(entry)
        : "r0", "memory"
    );
#endif

    LogToJava("NativeExecutionThread: Выход из iOS.");
    return nullptr;
}

// --- ЗАГЛУШКИ C++ ABI ДЛЯ STD::STRING (libstdc++ GCC 4.2) ---
struct LibStdStringRep {
    size_t length;
    size_t capacity;
    int refcount;
};

char* AllocateLibStdString(const char* cstr) {
    if (!cstr) cstr = "";
    size_t len = strlen(cstr);
    LibStdStringRep* rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + len + 1);
    rep->length = len;
    rep->capacity = len;
    rep->refcount = 1;
    char* data = (char*)(rep + 1);
    memcpy(data, cstr, len + 1);
    return data;
}

extern "C" void* wrap_cxx_string_default_ctor(void* this_ptr) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    char** str_obj = (char**)this_ptr;
    // Используем глобальную пустую строку, как это делает GCC 4.2
    *str_obj = (char*)(&hle_empty_string_rep[3]); 
    LogToJava("C-API-DEBUG: [cxx_string_default_ctor] this=" + std::to_string((uintptr_t)this_ptr) + " std::string() - Используем глобальную пустую строку. Caller: " + GetModuleInfoForAddress(lr));
    return this_ptr;
}

extern "C" void* wrap_cxx_string_ctor(void* this_ptr, void* str_ptr, void* alloc) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    const char* input_str = (const char*)str_ptr;
    std::string preview = input_str ? std::string(input_str, strnlen(input_str, 256)) : "null";
    std::string hex = input_str ? DumpHexToString(input_str, strnlen(input_str, 32)) : "null";
    LogToJava("C-API-DEBUG: [cxx_string_ctor] this=" + std::to_string((uintptr_t)this_ptr) + " string(const char*). Preview: [" + preview + "] HEX: (" + hex + ") Caller: " + GetModuleInfoForAddress(lr));
    
    char** str_obj = (char**)this_ptr;
    *str_obj = AllocateLibStdString(input_str);
    return this_ptr;
}
extern "C" void* wrap_cxx_string_copy_ctor(void* this_ptr, void* other_ptr) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    char** src = (char**)other_ptr;
    std::string preview = "null";
    int ref_count = -999;
    size_t len = 0;
    
    if (src && *src) {
        char* c_str = *src;
        LibStdStringRep* rep = (LibStdStringRep*)c_str - 1;
        ref_count = rep->refcount;
        len = rep->length;
        
        // Читаем безопасно, чтобы не улететь в Segfault если len = 4 миллиарда
        size_t safe_len = (len < 256) ? len : 256;
        preview = std::string(c_str, strnlen(c_str, safe_len));
    }
    std::string hex = (src && *src) ? DumpHexToString(*src, 32) : "null";
    LogToJava("C-API-DEBUG: [cxx_string_copy_ctor] this=" + std::to_string((uintptr_t)this_ptr) + " src=" + std::to_string((uintptr_t)other_ptr) + " len=" + std::to_string(len) + " ref=" + std::to_string(ref_count) + " Preview: [" + preview + "] HEX: (" + hex + ") Caller: " + GetModuleInfoForAddress(lr));
    
    char** dest = (char**)this_ptr;
    if (src && *src) {
        LibStdStringRep* rep = (LibStdStringRep*)(*src) - 1;
        if (rep->refcount >= 0) {
            __sync_fetch_and_add(&rep->refcount, 1);
        }
        *dest = *src;
    } else {
        *dest = AllocateLibStdString("");
    }
    return this_ptr;
}
extern "C" void* wrap_cxx_string_dtor(void* this_ptr) {
    char** str_obj = (char**)this_ptr;
    if (str_obj && *str_obj && *str_obj != (char*)(&hle_empty_string_rep[3])) {
        LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        // Защита от мусора
        if (rep->length < 0x100000 && rep->refcount > 0 && rep->refcount < 100000) {
            if (__sync_sub_and_fetch(&rep->refcount, 1) <= 0) {
                free(rep);
            }
        }
    }
    return this_ptr;
}
extern "C" void* wrap_cxx_string_reserve(void* this_ptr, size_t res_arg) {
    char** str_obj = (char**)this_ptr;
    if (str_obj && *str_obj) {
        LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        if (res_arg > rep->capacity || rep->refcount < 0 || rep->refcount > 1) {
            size_t target_cap = (res_arg > rep->capacity) ? res_arg : rep->capacity;
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + target_cap + 1);
            if (new_rep) {
                new_rep->length = rep->length;
                new_rep->capacity = target_cap;
                new_rep->refcount = 1;
                char* new_data = (char*)(new_rep + 1);
                memcpy(new_data, *str_obj, rep->length + 1);
                if (rep->refcount >= 0) {
                    if (__sync_sub_and_fetch(&rep->refcount, 1) <= 0) free(rep);
                }
                *str_obj = new_data;
            }
        }
    }
    return this_ptr;
}
extern "C" void* wrap_cxx_string_append(void* this_ptr, void* other_ptr) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    char** dest = (char**)this_ptr;
    char** src = (char**)other_ptr;
    
    std::string s_dest = (dest && *dest) ? std::string(*dest, strnlen(*dest, 256)) : "null";
    std::string hex_dest = (dest && *dest) ? DumpHexToString(*dest, 32) : "null";
    std::string s_src = (src && *src) ? std::string(*src, strnlen(*src, 256)) : "null";
    std::string hex_src = (src && *src) ? DumpHexToString(*src, 32) : "null";
    LogToJava("C-API-DEBUG: [cxx_string_append] this=" + std::to_string((uintptr_t)this_ptr) + " Склеиваем: [" + s_dest + "] HEX_DEST: (" + hex_dest + ") + [" + s_src + "] HEX_SRC: (" + hex_src + ") Caller: " + GetModuleInfoForAddress(lr));

    if (dest && *dest && src && *src) {
        LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
        LibStdStringRep* src_rep = (LibStdStringRep*)(*src) - 1;
        size_t new_len = dest_rep->length + src_rep->length;
        if (new_len > dest_rep->capacity || dest_rep->refcount < 0 || dest_rep->refcount > 1) {
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + new_len + 1);
            if (!new_rep) return this_ptr;
            new_rep->length = new_len;
            new_rep->capacity = new_len;
            new_rep->refcount = 1;
            char* new_data = (char*)(new_rep + 1);
            memcpy(new_data, *dest, dest_rep->length);
            memcpy(new_data + dest_rep->length, *src, src_rep->length);
            new_data[new_len] = '\0';
            if (dest_rep->refcount >= 0) {
                if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep);
            }
            *dest = new_data;
        } else {
            memcpy((*dest) + dest_rep->length, *src, src_rep->length);
            dest_rep->length = new_len;
            (*dest)[new_len] = '\0';
        }
    }
    return this_ptr;
}
extern "C" void* wrap_cxx_string_append_ptr_len(void* this_ptr, const char* str_ptr, size_t len) {
    char** dest = (char**)this_ptr;
    if (dest && *dest && str_ptr && len > 0) {
        LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
        size_t new_len = dest_rep->length + len;
        if (new_len > dest_rep->capacity || dest_rep->refcount < 0 || dest_rep->refcount > 1) {
            size_t new_cap = (dest_rep->capacity * 2) > new_len ? (dest_rep->capacity * 2) : new_len;
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + new_cap + 1);
            if (new_rep) {
                new_rep->length = new_len;
                new_rep->capacity = new_cap;
                new_rep->refcount = 1;
                char* new_data = (char*)(new_rep + 1);
                memcpy(new_data, *dest, dest_rep->length);
                memcpy(new_data + dest_rep->length, str_ptr, len);
                new_data[new_len] = '\0';
                if (dest_rep->refcount >= 0) {
                    if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep);
                }
                *dest = new_data;
            }
        } else {
            memcpy((*dest) + dest_rep->length, str_ptr, len);
            dest_rep->length = new_len;
            (*dest)[new_len] = '\0';
        }
    }
    return this_ptr;
}

extern "C" int wrap_cxx_string_compare_string(void* this_ptr, void* other_ptr) {
    char** dest = (char**)this_ptr;
    char** src = (char**)other_ptr;
    const char* s1 = "";
    const char* s2 = "";
    // Жёсткая защита от мусорных указателей, вроде 0x10 от кривых итераторов
    if ((uintptr_t)dest > 0x1000 && *dest && (uintptr_t)(*dest) > 0x1000) s1 = *dest;
    if ((uintptr_t)src > 0x1000 && *src && (uintptr_t)(*src) > 0x1000) s2 = *src;
    return strcmp(s1, s2);
}

extern "C" void wrap_cxx_string_push_back(void* this_ptr, char c) {
    char** dest = (char**)this_ptr;
    if (dest && *dest) {
        LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
        size_t new_len = dest_rep->length + 1;
        if (new_len > dest_rep->capacity || dest_rep->refcount < 0 || dest_rep->refcount > 1) {
            size_t new_cap = (dest_rep->capacity * 2) > new_len ? (dest_rep->capacity * 2) : new_len;
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + new_cap + 1);
            if (!new_rep) return;
            new_rep->length = new_len;
            new_rep->capacity = new_cap;
            new_rep->refcount = 1;
            char* new_data = (char*)(new_rep + 1);
            memcpy(new_data, *dest, dest_rep->length);
            new_data[dest_rep->length] = c;
            new_data[new_len] = '\0';
            if (dest_rep->refcount >= 0) {
                if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep);
            }
            *dest = new_data;
        } else {
            (*dest)[dest_rep->length] = c;
            dest_rep->length = new_len;
            (*dest)[new_len] = '\0';
        }
    }
}
extern "C" void wrap_cxx_string_dispose(void* rep_ptr, void* alloc_ptr) {
    if (rep_ptr) {
        LibStdStringRep* rep = (LibStdStringRep*)rep_ptr;
        if (rep->refcount >= 0) {
            if (__sync_sub_and_fetch(&rep->refcount, 1) <= 0) free(rep);
        }
    }
}

extern "C" void* wrap_cxx_string_assign_ptr_len(void* this_ptr, const char* str_ptr, size_t len) {
    char** dest = (char**)this_ptr;
    if (dest && *dest) {
        LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
        if (len > dest_rep->capacity || dest_rep->refcount < 0 || dest_rep->refcount > 1) {
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + len + 1);
            if (new_rep) {
                new_rep->length = len;
                new_rep->capacity = len;
                new_rep->refcount = 1;
                char* new_data = (char*)(new_rep + 1);
                if (str_ptr) memcpy(new_data, str_ptr, len);
                new_data[len] = '\0';
                if (dest_rep->refcount >= 0) {
                    if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep);
                }
                *dest = new_data;
            }
        } else {
            if (str_ptr) memcpy(*dest, str_ptr, len);
            dest_rep->length = len;
            (*dest)[len] = '\0';
        }
    }
    return this_ptr;
}

extern "C" void* wrap_cxx_string_assign_string(void* this_ptr, void* other_ptr) {
    char** dest = (char**)this_ptr;
    char** src = (char**)other_ptr;
    if (dest && src && *src) {
        LibStdStringRep* src_rep = (LibStdStringRep*)(*src) - 1;
        if (src_rep->refcount >= 0) __sync_fetch_and_add(&src_rep->refcount, 1);
        if (*dest) {
            LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
            if (dest_rep->refcount >= 0) {
                if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep);
            }
        }
        *dest = *src;
    }
    return this_ptr;
}

extern "C" void* wrap_cxx_string_ctor_ptr_len(void* this_ptr, void* str_ptr, size_t len, void* alloc) {
    char** str_obj = (char**)this_ptr;
    LibStdStringRep* rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + len + 1);
    rep->length = len;
    rep->capacity = len;
    rep->refcount = 1;
    char* data = (char*)(rep + 1);
    if (str_ptr) memcpy(data, str_ptr, len);
    data[len] = '\0';
    *str_obj = data;
    return this_ptr;
}

extern "C" size_t wrap_cxx_string_rfind_char(void* this_ptr, char c, size_t pos) {
    char** str_obj = (char**)this_ptr;
    if (str_obj && *str_obj) {
        LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        size_t len = rep->length;
        if (len == 0) return (size_t)-1;
        if (pos >= len) pos = len - 1;
        for (size_t i = pos + 1; i > 0; --i) {
            if ((*str_obj)[i - 1] == c) return i - 1;
        }
    }
    return (size_t)-1;
}

extern "C" void* wrap_cxx_string_ctor_sub(void* this_ptr, void* str_ptr, size_t pos, size_t n) {
    char** dest = (char**)this_ptr;
    char** src = (char**)str_ptr;
    if (src && *src) {
        LibStdStringRep* src_rep = (LibStdStringRep*)(*src) - 1;
        size_t len = src_rep->length;
        if (pos > len) {
            *dest = AllocateLibStdString("");
            return this_ptr;
        }
        size_t copy_len = len - pos;
        if (n < copy_len) copy_len = n;
        
        LibStdStringRep* rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + copy_len + 1);
        rep->length = copy_len;
        rep->capacity = copy_len;
        rep->refcount = 1;
        char* data = (char*)(rep + 1);
        memcpy(data, (*src) + pos, copy_len);
        data[copy_len] = '\0';
        *dest = data;
    } else {
        *dest = AllocateLibStdString("");
    }
    return this_ptr;
}

extern "C" int wrap_cxx_string_compare_char(void* this_ptr, const char* s) {
    char** str_obj = (char**)this_ptr;
    const char* my_str = (str_obj && *str_obj) ? *str_obj : "";
    const char* other_str = s ? s : "";
    return strcmp(my_str, other_str);
}

struct Rb_tree_node_base {
    int color; // 0 = red, 1 = black
    Rb_tree_node_base* parent;
    Rb_tree_node_base* left;
    Rb_tree_node_base* right;
};

extern "C" void* wrap_Rb_tree_increment(void* x_node) {
    Rb_tree_node_base* x = (Rb_tree_node_base*)x_node;
    if (!x) return nullptr;
    if (x->right != nullptr) {
        x = x->right;
        while (x->left != nullptr) x = x->left;
    } else {
        Rb_tree_node_base* y = x->parent;
        while (x == y->right) {
            x = y;
            y = y->parent;
        }
        if (x->right != y) x = y;
    }
    return x;
}

extern "C" void* wrap_Rb_tree_decrement(void* x_node) {
    Rb_tree_node_base* x = (Rb_tree_node_base*)x_node;
    if (!x) return nullptr;
    if (x->color == 0 && x->parent->parent == x) {
        x = x->right;
    } else if (x->left != nullptr) {
        Rb_tree_node_base* y = x->left;
        while (y->right != nullptr) y = y->right;
        x = y;
    } else {
        Rb_tree_node_base* y = x->parent;
        while (x == y->left) {
            x = y;
            y = y->parent;
        }
        x = y;
    }
    return x;
}

extern "C" void wrap_Rb_tree_insert_and_rebalance(bool insert_left, void* x_node, void* p_node, void* header_node) {
    Rb_tree_node_base* x = (Rb_tree_node_base*)x_node;
    Rb_tree_node_base* p = (Rb_tree_node_base*)p_node;
    Rb_tree_node_base* header = (Rb_tree_node_base*)header_node;
    
    x->parent = p;
    x->right = nullptr;
    x->left = nullptr;
    x->color = 0; // red
    
    if (insert_left) {
        p->left = x;
        if (p == header) {
            header->parent = x;
            header->right = x;
        } else if (p == header->left) {
            header->left = x;
        }
    } else {
        p->right = x;
        if (p == header->right) {
            header->right = x;
        }
    }
    
    while (x != header->parent && x->parent->color == 0) {
        Rb_tree_node_base* xpp = x->parent->parent;
        if (x->parent == xpp->left) {
            Rb_tree_node_base* y = xpp->right;
            if (y && y->color == 0) {
                x->parent->color = 1;
                y->color = 1;
                xpp->color = 0;
                x = xpp;
            } else {
                if (x == x->parent->right) {
                    x = x->parent;
                    Rb_tree_node_base* y_rot = x->right;
                    x->right = y_rot->left;
                    if (y_rot->left != nullptr) y_rot->left->parent = x;
                    y_rot->parent = x->parent;
                    if (x == header->parent) header->parent = y_rot;
                    else if (x == x->parent->left) x->parent->left = y_rot;
                    else x->parent->right = y_rot;
                    y_rot->left = x;
                    x->parent = y_rot;
                }
                x->parent->color = 1;
                xpp->color = 0;
                Rb_tree_node_base* y_rot = xpp->left;
                xpp->left = y_rot->right;
                if (y_rot->right != nullptr) y_rot->right->parent = xpp;
                y_rot->parent = xpp->parent;
                if (xpp == header->parent) header->parent = y_rot;
                else if (xpp == xpp->parent->right) xpp->parent->right = y_rot;
                else xpp->parent->left = y_rot;
                y_rot->right = xpp;
                xpp->parent = y_rot;
            }
        } else {
            Rb_tree_node_base* y = xpp->left;
            if (y && y->color == 0) {
                x->parent->color = 1;
                y->color = 1;
                xpp->color = 0;
                x = xpp;
            } else {
                if (x == x->parent->left) {
                    x = x->parent;
                    Rb_tree_node_base* y_rot = x->left;
                    x->left = y_rot->right;
                    if (y_rot->right != nullptr) y_rot->right->parent = x;
                    y_rot->parent = x->parent;
                    if (x == header->parent) header->parent = y_rot;
                    else if (x == x->parent->right) x->parent->right = y_rot;
                    else x->parent->left = y_rot;
                    y_rot->right = x;
                    x->parent = y_rot;
                }
                x->parent->color = 1;
                xpp->color = 0;
                Rb_tree_node_base* y_rot = xpp->right;
                xpp->right = y_rot->left;
                if (y_rot->left != nullptr) y_rot->left->parent = xpp;
                y_rot->parent = xpp->parent;
                if (xpp == header->parent) header->parent = y_rot;
                else if (xpp == xpp->parent->left) xpp->parent->left = y_rot;
                else xpp->parent->right = y_rot;
                y_rot->left = xpp;
                xpp->parent = y_rot;
            }
        }
    }
    header->parent->color = 1;
}

size_t wstrlen(const int32_t* s) { size_t len = 0; if (s) while (s[len] != 0) len++; return len; }

int32_t* AllocateLibStdWString(const int32_t* cstr) {
    size_t len = wstrlen(cstr);
    LibStdStringRep* rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + (len + 1) * 4);
    rep->length = len; rep->capacity = len; rep->refcount = 1;
    int32_t* data = (int32_t*)(rep + 1);
    if (cstr) memcpy(data, cstr, len * 4);
    data[len] = 0; return data;
}

extern "C" void* wrap_cxx_wstring_default_ctor(void* this_ptr) {
    int32_t** str_obj = (int32_t**)this_ptr; *str_obj = AllocateLibStdWString(nullptr); return this_ptr;
}

extern "C" void* wrap_cxx_wstring_ctor(void* this_ptr, void* str_ptr, void* alloc) {
    int32_t** str_obj = (int32_t**)this_ptr; *str_obj = AllocateLibStdWString((const int32_t*)str_ptr); return this_ptr;
}

extern "C" void* wrap_cxx_wstring_copy_ctor(void* this_ptr, void* other_ptr) {
    int32_t** dest = (int32_t**)this_ptr; int32_t** src = (int32_t**)other_ptr;
    // Защита: проверяем, что *src не является системным мусором (< 0x1000)
    if (src && *src && (uintptr_t)(*src) > 0x1000) {
        LibStdStringRep* rep = (LibStdStringRep*)(*src) - 1;
        if (rep->refcount >= 0) __sync_fetch_and_add(&rep->refcount, 1);
        *dest = *src;
    } else {
        *dest = AllocateLibStdWString(nullptr);
    }
    return this_ptr;
}

extern "C" void* wrap_cxx_wstring_assign_ptr_len(void* this_ptr, const int32_t* str_ptr, size_t len) {
    int32_t** dest = (int32_t**)this_ptr;
    if (dest && *dest) {
        LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
        if (len > dest_rep->capacity || dest_rep->refcount < 0 || dest_rep->refcount > 1) {
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + (len + 1) * 4);
            if (new_rep) {
                new_rep->length = len; new_rep->capacity = len; new_rep->refcount = 1;
                int32_t* new_data = (int32_t*)(new_rep + 1);
                if (str_ptr) memcpy(new_data, str_ptr, len * 4);
                new_data[len] = 0;
                if (dest_rep->refcount >= 0) { if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep); }
                *dest = new_data;
            }
        } else {
            if (str_ptr) memcpy(*dest, str_ptr, len * 4);
            dest_rep->length = len; (*dest)[len] = 0;
        }
    }
    return this_ptr;
}

extern "C" const int32_t* wrap_cxx_wstring_c_str(void* this_ptr) {
    int32_t** str_obj = (int32_t**)this_ptr;
    return (str_obj && *str_obj) ? *str_obj : nullptr;
}

extern "C" void* wrap_cxx_wstring_reserve(void* this_ptr, size_t res_arg) {
    int32_t** str_obj = (int32_t**)this_ptr; // На iOS ARMv7 wchar_t это 4 байта
    if (str_obj && *str_obj) {
        LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        if (res_arg > rep->capacity || rep->refcount < 0 || rep->refcount > 1) {
            size_t target_cap = (res_arg > rep->capacity) ? res_arg : rep->capacity;
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + (target_cap + 1) * 4);
            if (new_rep) {
                new_rep->length = rep->length;
                new_rep->capacity = target_cap;
                new_rep->refcount = 1;
                int32_t* new_data = (int32_t*)(new_rep + 1);
                memcpy(new_data, *str_obj, (rep->length + 1) * 4);
                if (rep->refcount >= 0) {
                    if (__sync_sub_and_fetch(&rep->refcount, 1) <= 0) free(rep);
                }
                *str_obj = new_data;
            }
        }
    }
    return this_ptr;
}

extern "C" void* wrap_cxx_wstring_assign_string(void* this_ptr, void* other_ptr) {
    int32_t** dest = (int32_t**)this_ptr;
    int32_t** src = (int32_t**)other_ptr;
    if (dest && src && *src) {
        LibStdStringRep* src_rep = (LibStdStringRep*)(*src) - 1;
        if (src_rep->refcount >= 0) __sync_fetch_and_add(&src_rep->refcount, 1);
        if (*dest) {
            LibStdStringRep* dest_rep = (LibStdStringRep*)(*dest) - 1;
            if (dest_rep->refcount >= 0) {
                if (__sync_sub_and_fetch(&dest_rep->refcount, 1) <= 0) free(dest_rep);
            }
        }
        *dest = *src;
    }
    return this_ptr;
}

extern "C" void* wrap_cxx_wstring_assign_c_str(void* this_ptr, const int32_t* str) {
    return wrap_cxx_wstring_assign_ptr_len(this_ptr, str, wstrlen(str));
}

extern "C" size_t wrap_cxx_string_length(void* this_ptr) {
    char** str_obj = (char**)this_ptr;
    if (str_obj && *str_obj) return ((LibStdStringRep*)(*str_obj) - 1)->length;
    return 0;
}

extern "C" void* wrap_cxx_string_assign_c_str(void* this_ptr, const char* str) {
    return wrap_cxx_string_assign_ptr_len(this_ptr, str, str ? strlen(str) : 0);
}

extern "C" void* wrap_cxx_string_M_leak_hard(void* this_ptr) {
    char** str_obj = (char**)this_ptr;
    if (str_obj && *str_obj) {
        LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        if (rep->refcount > 1) {
            LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + rep->capacity + 1);
            new_rep->length = rep->length;
            new_rep->capacity = rep->capacity;
            new_rep->refcount = 1;
            char* new_data = (char*)(new_rep + 1);
            memcpy(new_data, *str_obj, rep->length + 1);
            __sync_sub_and_fetch(&rep->refcount, 1);
            *str_obj = new_data;
        } else if (rep->refcount < 0) {
            rep->refcount = 1;
        }
    }
    return this_ptr;
}

extern "C" size_t wrap_cxx_string_find_char(void* this_ptr, char c, size_t pos) {
    char** str_obj = (char**)this_ptr;
    if (str_obj && *str_obj) {
        LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        if (pos >= rep->length) return (size_t)-1;
        for (size_t i = pos; i < rep->length; i++) {
            if ((*str_obj)[i] == c) return i;
        }
    }
    return (size_t)-1;
}

extern "C" void wrap_cxx_string_M_mutate(void* this_ptr, size_t pos, size_t len1, size_t len2) {
    char** dest = (char**)this_ptr;
    if (!dest || !*dest) return;
    LibStdStringRep* rep = (LibStdStringRep*)(*dest) - 1;
    size_t old_len = rep->length;
    if (pos > old_len) return;
    if (pos + len1 > old_len) len1 = old_len - pos;
    size_t new_len = old_len - len1 + len2;
    if (new_len > rep->capacity || rep->refcount > 1 || rep->refcount < 0) {
        size_t new_cap = (rep->capacity * 2 > new_len) ? (rep->capacity * 2) : new_len;
        LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + new_cap + 1);
        new_rep->length = new_len;
        new_rep->capacity = new_cap;
        new_rep->refcount = 1;
        char* new_data = (char*)(new_rep + 1);
        memcpy(new_data, *dest, pos);
        memcpy(new_data + pos + len2, (*dest) + pos + len1, old_len - pos - len1);
        new_data[new_len] = '\0';
        if (rep->refcount >= 0 && __sync_sub_and_fetch(&rep->refcount, 1) <= 0) free(rep);
        *dest = new_data;
    } else {
        memmove((*dest) + pos + len2, (*dest) + pos + len1, old_len - pos - len1);
        rep->length = new_len;
        (*dest)[new_len] = '\0';
    }
}

extern "C" void* wrap_cxx_string_M_replace_aux(void* this_ptr, size_t pos, size_t n1, size_t n2, char c) {
    wrap_cxx_string_M_mutate(this_ptr, pos, n1, n2);
    char** dest = (char**)this_ptr;
    if (dest && *dest && n2 > 0) {
        memset((*dest) + pos, c, n2);
    }
    return this_ptr;
}

extern "C" void wrap_cxx_string_M_destroy(void* rep_ptr, void* alloc_ptr) {
    wrap_cxx_string_dispose(rep_ptr, alloc_ptr);
}

extern "C" void* wrap_allocator_ctor(void* this_ptr) { return this_ptr; }
extern "C" void* wrap_allocator_dtor(void* this_ptr) { return this_ptr; }

extern "C" void wrap_memset_pattern16(void *b, const void *pattern16, size_t len) {
    uint8_t *dest = (uint8_t *)b;
    const uint8_t *pat = (const uint8_t *)pattern16;
    for (size_t i = 0; i < len; i++) {
        dest[i] = pat[i % 16];
    }
}

// --- STD::LIST MISSING METHODS ---
struct List_node_base { List_node_base* next; List_node_base* prev; };
extern "C" void wrap_List_node_base_hook(void* this_node_ptr, void* position_ptr) {
    List_node_base* const this_node = (List_node_base*)this_node_ptr;
    List_node_base* const position = (List_node_base*)position_ptr;
    this_node->next = position; this_node->prev = position->prev;
    position->prev->next = this_node; position->prev = this_node;
}
extern "C" void wrap_List_node_base_unhook(void* this_node_ptr) {
    List_node_base* const this_node = (List_node_base*)this_node_ptr;
    List_node_base* const next_node = this_node->next; List_node_base* const prev_node = this_node->prev;
    prev_node->next = next_node; next_node->prev = prev_node;
}
extern "C" void wrap_List_node_base_transfer(void* position_ptr, void* first_ptr, void* last_ptr) {
    List_node_base* const position = (List_node_base*)position_ptr;
    List_node_base* const first = (List_node_base*)first_ptr;
    List_node_base* const last = (List_node_base*)last_ptr;
    if (position != last) {
        first->prev->next = last; last->prev->next = position; position->prev->next = first;
        List_node_base* const tmp = position->prev; position->prev = last->prev; last->prev = first->prev; first->prev = tmp;
    }
}

// --- STD::RB_TREE ERASE FIXUP ---
extern "C" void wrap_Rb_tree_rebalance_for_erase(void* z_ptr, void* header_ptr) {
    Rb_tree_node_base* z = (Rb_tree_node_base*)z_ptr;
    Rb_tree_node_base* header = (Rb_tree_node_base*)header_ptr;
    Rb_tree_node_base*& root = header->parent; Rb_tree_node_base*& leftmost = header->left; Rb_tree_node_base*& rightmost = header->right;
    Rb_tree_node_base* y = z; Rb_tree_node_base* x = nullptr; Rb_tree_node_base* x_parent = nullptr;
    if (y->left == nullptr) x = y->right; else if (y->right == nullptr) x = y->left;
    else { y = y->right; while (y->left != nullptr) y = y->left; x = y->right; }
    if (y != z) { z->left->parent = y; y->left = z->left;
        if (y != z->right) { x_parent = y->parent; if (x) x->parent = y->parent; y->parent->left = x; y->right = z->right; z->right->parent = y; } else x_parent = y;
        if (root == z) root = y; else if (z->parent->left == z) z->parent->left = y; else z->parent->right = y;
        y->parent = z->parent; int tmp = y->color; y->color = z->color; z->color = tmp; y = z;
    } else { x_parent = y->parent; if (x) x->parent = y->parent;
        if (root == z) root = x; else if (z->parent->left == z) z->parent->left = x; else z->parent->right = x;
        if (leftmost == z) leftmost = (z->right == nullptr) ? z->parent : (Rb_tree_node_base*)wrap_Rb_tree_decrement(z);
        if (rightmost == z) rightmost = (z->left == nullptr) ? z->parent : (Rb_tree_node_base*)wrap_Rb_tree_increment(z);
    }
    if (y->color != 0) {
        while (x != root && (x == nullptr || x->color == 1)) {
            if (x == x_parent->left) { Rb_tree_node_base* w = x_parent->right;
                if (w->color == 0) { w->color = 1; x_parent->color = 0; Rb_tree_node_base* tmp = x_parent->right; x_parent->right = tmp->left; if (tmp->left) tmp->left->parent = x_parent; tmp->parent = x_parent->parent; if (x_parent == root) root = tmp; else if (x_parent == x_parent->parent->left) x_parent->parent->left = tmp; else x_parent->parent->right = tmp; tmp->left = x_parent; x_parent->parent = tmp; w = x_parent->right; }
                if ((w->left == nullptr || w->left->color == 1) && (w->right == nullptr || w->right->color == 1)) { w->color = 0; x = x_parent; x_parent = x_parent->parent; } else {
                    if (w->right == nullptr || w->right->color == 1) { if (w->left) w->left->color = 1; w->color = 0; Rb_tree_node_base* tmp = w->left; w->left = tmp->right; if (tmp->right) tmp->right->parent = w; tmp->parent = w->parent; if (w == root) root = tmp; else if (w == w->parent->right) w->parent->right = tmp; else w->parent->left = tmp; tmp->right = w; w->parent = tmp; w = x_parent->right; }
                    w->color = x_parent->color; x_parent->color = 1; if (w->right) w->right->color = 1; Rb_tree_node_base* tmp = x_parent->right; x_parent->right = tmp->left; if (tmp->left) tmp->left->parent = x_parent; tmp->parent = x_parent->parent; if (x_parent == root) root = tmp; else if (x_parent == x_parent->parent->left) x_parent->parent->left = tmp; else x_parent->parent->right = tmp; tmp->left = x_parent; x_parent->parent = tmp; break;
                }
            } else { Rb_tree_node_base* w = x_parent->left;
                if (w->color == 0) { w->color = 1; x_parent->color = 0; Rb_tree_node_base* tmp = x_parent->left; x_parent->left = tmp->right; if (tmp->right) tmp->right->parent = x_parent; tmp->parent = x_parent->parent; if (x_parent == root) root = tmp; else if (x_parent == x_parent->parent->right) x_parent->parent->right = tmp; else x_parent->parent->left = tmp; tmp->right = x_parent; x_parent->parent = tmp; w = x_parent->left; }
                if ((w->right == nullptr || w->right->color == 1) && (w->left == nullptr || w->left->color == 1)) { w->color = 0; x = x_parent; x_parent = x_parent->parent; } else {
                    if (w->left == nullptr || w->left->color == 1) { if (w->right) w->right->color = 1; w->color = 0; Rb_tree_node_base* tmp = w->right; w->right = tmp->left; if (tmp->left) tmp->left->parent = w; tmp->parent = w->parent; if (w == root) root = tmp; else if (w == w->parent->left) w->parent->left = tmp; else w->parent->right = tmp; tmp->left = w; w->parent = tmp; w = x_parent->left; }
                    w->color = x_parent->color; x_parent->color = 1; if (w->left) w->left->color = 1; Rb_tree_node_base* tmp = x_parent->left; x_parent->left = tmp->right; if (tmp->right) tmp->right->parent = x_parent; tmp->parent = x_parent->parent; if (x_parent == root) root = tmp; else if (x_parent == x_parent->parent->right) x_parent->parent->right = tmp; else x_parent->parent->left = tmp; tmp->right = x_parent; x_parent->parent = tmp; break;
                }
            }
        }
        if (x) x->color = 1;
    }
}

// --- STD::STRING MISSING ---
extern "C" size_t wrap_cxx_string_size(void* this_ptr) { char** dest = (char**)this_ptr; return (dest && *dest) ? (((LibStdStringRep*)(*dest) - 1)->length) : 0; }
extern "C" char* wrap_cxx_string_begin(void* this_ptr) { char** dest = (char**)this_ptr; return (dest && *dest) ? *dest : nullptr; }
extern "C" char* wrap_cxx_string_end(void* this_ptr) { char** dest = (char**)this_ptr; return (dest && *dest) ? *dest + (((LibStdStringRep*)(*dest)-1)->length) : nullptr; }
extern "C" size_t wrap_cxx_string_find_last_of_ptr_len(void* this_ptr, const char* s, size_t pos) {
    char** dest=(char**)this_ptr;
    if(dest&&*dest&&s){
        size_t l=(((LibStdStringRep*)(*dest)-1)->length);
        if(l==0) return -1;
        if(pos>=l) pos = l - 1;
        std::string_view sv(*dest, l);
        size_t f=sv.find_last_of(s, pos);
        return f==std::string_view::npos ? -1 : f;
    }
    return -1;
}
extern "C" void* wrap_cxx_string_substr(void* ret_ptr, void* this_ptr, size_t pos, size_t n) { 
    wrap_cxx_string_default_ctor(ret_ptr); 
    wrap_cxx_string_ctor_sub(ret_ptr, this_ptr, pos, n); 
    return ret_ptr; 
}
extern "C" int wrap_cxx_string_compare_pos_len_str(void* this_ptr, size_t pos, size_t n, void* other_ptr) { char** src=(char**)other_ptr; return wrap_cxx_string_compare_pos_len_ptr(this_ptr, pos, n, (src&&*src)?*src:""); }
extern "C" char* wrap_cxx_string_at(void* this_ptr, size_t pos) {
    char** dest = (char**)this_ptr;
    if (dest && *dest) {
        size_t l = (((LibStdStringRep*)(*dest)-1)->length);
        if (pos < l) return &(*dest)[pos];
    }
    wrap_ZSt20__throw_out_of_rangePKc("basic_string::at");
    return nullptr;
}
extern "C" void* wrap_cxx_string_replace_pos_len_ptr(void* this_ptr, size_t pos, size_t n, const char* s) {
    size_t slen = s ? strlen(s) : 0;
    wrap_cxx_string_M_mutate(this_ptr, pos, n, slen);
    char** dest = (char**)this_ptr;
    if (dest && *dest && slen > 0) memcpy((*dest) + pos, s, slen);
    return this_ptr;
}
extern "C" void* wrap_cxx_string_replace_pos_len_str(void* this_ptr, size_t pos, size_t n, void* other_ptr) {
    char** src = (char**)other_ptr;
    if (src && *src) {
        size_t slen = (((LibStdStringRep*)(*src) - 1)->length);
        wrap_cxx_string_M_mutate(this_ptr, pos, n, slen);
        char** dest = (char**)this_ptr;
        if (dest && *dest && slen > 0) memcpy((*dest) + pos, *src, slen);
    } else {
        wrap_cxx_string_M_mutate(this_ptr, pos, n, 0);
    }
    return this_ptr;
}
extern "C" size_t wrap_cxx_string_find_string(void* this_ptr, void* other_ptr, size_t pos) {
    char** src = (char**)other_ptr;
    if (src && *src) {
        size_t len = (((LibStdStringRep*)(*src) - 1)->length);
        return wrap_cxx_string_find_ptr_len(this_ptr, *src, pos, len);
    }
    return (size_t)-1;
}
extern "C" void* wrap_cxx_string_ctor_len_char(void* this_ptr, size_t n, char c, void* alloc) {
    char** str_obj = (char**)this_ptr;
    LibStdStringRep* rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + n + 1);
    rep->length = n;
    rep->capacity = n;
    rep->refcount = 1;
    char* data = (char*)(rep + 1);
    memset(data, c, n);
    data[n] = '\0';
    *str_obj = data;
    return this_ptr;
}
extern "C" void* wrap_cxx_ostream_insert_string(void* this_ptr, void* str_ptr) {
    char** src = (char**)str_ptr;
    if (src && *src) { wrap_cxx_ostream_insert_char_ptr(this_ptr, *src, (((LibStdStringRep*)(*src)-1)->length)); }
    return this_ptr;
}
extern "C" void* wrap_cxx_ostream_insert_char_ptr_simple(void* this_ptr, const char* s) {
    if (s) wrap_cxx_ostream_insert_char_ptr(this_ptr, s, strlen(s));
    return this_ptr;
}
extern "C" void* wrap_cxx_ostream_insert_int(void* this_ptr, int v) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) LogToJava("GAME-COUT: " + std::to_string(v)); return this_ptr; }
extern "C" void* wrap_cxx_ostream_insert_uint(void* this_ptr, unsigned int v) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) LogToJava("GAME-COUT: " + std::to_string(v)); return this_ptr; }
extern "C" void* wrap_cxx_ostream_insert_ulong(void* this_ptr, unsigned long v) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) LogToJava("GAME-COUT: " + std::to_string(v)); return this_ptr; }
extern "C" void* wrap_cxx_ostream_iomanip(void* this_ptr, int) { return this_ptr; }
extern "C" void wrap_List_node_base_swap(void* x_ptr, void* y_ptr) {
    List_node_base* x = (List_node_base*)x_ptr;
    List_node_base* y = (List_node_base*)y_ptr;
    if (x->next != x) {
        if (y->next != y) {
            List_node_base* tmp1 = x->next->prev; x->next->prev = y->next->prev; y->next->prev = tmp1;
            List_node_base* tmp2 = x->prev->next; x->prev->next = y->prev->next; y->prev->next = tmp2;
            List_node_base* tmp3 = x->next; x->next = y->next; y->next = tmp3;
            List_node_base* tmp4 = x->prev; x->prev = y->prev; y->prev = tmp4;
        }
        else { y->next = x->next; y->prev = x->prev; y->next->prev = y; y->prev->next = y; x->next = x; x->prev = x; }
    } else if (y->next != y) { x->next = y->next; x->prev = y->prev; x->next->prev = x; x->prev->next = x; y->next = y; y->prev = y; }
}
extern "C" void* wrap_new_array_nothrow(size_t size, void*) { return malloc(size); }
extern "C" void* wrap_cxx_basic_ios_operator_void_ptr(void* this_ptr) { return this_ptr; }

extern "C" size_t wrap_cxx_locale_id_M_id(void* this_ptr) {
    size_t* id_ptr = (size_t*)this_ptr;
    if (!*id_ptr) {
        static size_t __id_counter = 100;
        *id_ptr = __sync_add_and_fetch(&__id_counter, 1);
    }
    return *id_ptr;
}

extern "C" uint64_t wrap_cxx_ostream_tellp(void* this_ptr) {
    // В libstdc++ tellp() возвращает std::streampos. Если реализовывать полноценно
    // без подвязок к rdbuf, безопаснее всего вернуть -1, чтобы указать конец/отсутствие.
    return (uint64_t)-1;
}

extern "C" void wrap_cxx_basic_ios_clear(void* this_ptr, int state) {
    if (this_ptr) {
        // basic_ios::clear(iostate)
        // В libstdc++ (Apple GCC 4.2) _M_streambuf_state (iostate) находится по смещению 28 (7-ой DWORD).
        uint32_t* base = (uint32_t*)this_ptr;
        base[7] = state; 
    }
}

extern "C" uint32_t Stub_ReturnZero();
extern "C" uint32_t Stub_ReturnComma() { return ','; }
extern "C" uint32_t Stub_ReturnDot() { return '.'; }

uint32_t g_dummy_facet_vtable[64];
uint32_t g_dummy_facet_obj[4];
void* g_dummy_facets_array[256];

struct DummyLocaleImpl {
    int _M_refcount;
    void** _M_facets;
    size_t _M_facets_size;
    void** _M_caches;
    char** _M_names;
};
DummyLocaleImpl g_dummy_locale_impl;

void InitDummyLocale() {
    static bool init = false;
    if (init) return;
    init = true;
    for (int i = 0; i < 64; i++) g_dummy_facet_vtable[i] = (uint32_t)Stub_ReturnZero;
    
    // Эмуляция std::numpunct для char и wchar_t
    g_dummy_facet_vtable[2] = (uint32_t)Stub_ReturnDot;   // decimal_point
    g_dummy_facet_vtable[3] = (uint32_t)Stub_ReturnComma; // thousands_sep
    g_dummy_facet_vtable[4] = (uint32_t)Stub_ReturnDot;   // fallback
    g_dummy_facet_vtable[5] = (uint32_t)Stub_ReturnComma; // fallback
    
    g_dummy_facet_obj[0] = (uint32_t)g_dummy_facet_vtable + 8; // Смещение до виртуальных функций (пропуск typeinfo)
    for (int i = 0; i < 256; i++) g_dummy_facets_array[i] = g_dummy_facet_obj;
    
    g_dummy_locale_impl._M_refcount = 999999;
    g_dummy_locale_impl._M_facets = g_dummy_facets_array;
    g_dummy_locale_impl._M_facets_size = 256;
    g_dummy_locale_impl._M_caches = nullptr;
    g_dummy_locale_impl._M_names = nullptr;
}

extern "C" void* wrap_cxx_use_facet_ctype_char(void* loc) {
    InitDummyLocale();
    return g_dummy_facet_obj;
}

extern "C" void* wrap_cxx_use_facet_ctype_wchar(void* loc) {
    InitDummyLocale();
    return g_dummy_facet_obj;
}

extern "C" int wrap_AudioComponentInstanceDispose(void* inInstance) {
    LogToJava("HLE: AudioComponentInstanceDispose");
    return 0; // kAudioHardwareNoError
}

extern "C" void wrap__dyld_register_func_for_add_image(void (*func)(const void* mh, intptr_t vmaddr_slide)) {
    LogToJava("HLE: _dyld_register_func_for_add_image (проигнорировано для безопасности хуков игры)");
}

extern "C" int wrap_asprintf(char** ret, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vasprintf(ret, format, args);
    va_end(args);
    return res;
}

extern "C" void* wrap_dlopen(const char* filename, int flags) {
    LogToJava(std::string("HLE: dlopen(") + (filename ? filename : "null") + ") -> Имитация загрузки");
    return (void*)0x87654321; // Фейковый хэндл
}

extern "C" int wrap_dlclose(void* handle) {
    LogToJava("HLE: dlclose");
    return 0;
}

extern "C" void* wrap_hash_create(int nelem, void* info) {
    LogToJava("HLE: hash_create");
    return nullptr; // Если игра хочет BSD hash, мягко отказываем, она перейдет на fallback (массивы)
}

extern "C" void* wrap_hash_search(void* hashp, void* item, int action) {
    return nullptr;
}

extern "C" uint8_t wrap_class_addMethod(void* cls, void* name, void* imp, const char* types) { return 1; }
extern "C" uint8_t wrap_class_addProperty(void* cls, const char* name, const void* attributes, uint32_t attributeCount) { return 1; }
extern "C" uint8_t wrap_class_addProtocol(void* cls, void* protocol) { return 1; }
extern "C" void* wrap_class_getInstanceVariable(void* cls, const char* name) { return nullptr; }
extern "C" const uint8_t* wrap_class_getIvarLayout(void* cls) { return nullptr; }
extern "C" uint8_t wrap_class_isMetaClass(void* cls) { return 0; }
extern "C" uint8_t wrap_class_respondsToSelector(void* cls, void* sel) { return wrap_class_getInstanceMethod(cls, (const char*)sel) ? 1 : 0; }
extern "C" void* wrap_objc_getClass(const char* name) { return wrap_objc_lookUpClass(name); }
extern "C" void* wrap_objc_getMetaClass(const char* name) { return wrap_objc_lookUpClass(name); }
extern "C" void* wrap_objc_getProtocol(const char* name) { return name ? (void*)name : nullptr; }
extern "C" void* wrap_objc_getRequiredClass(const char* name) { void* cls = wrap_objc_lookUpClass(name); if (!cls) wrap_abort(); return cls; }
extern "C" void wrap_objc_initializeClassPair(void* cls) {}
extern "C" void wrap_objc_registerClassPair(void* cls) {}
extern "C" void* wrap_object_getIvar(void* obj, void* ivar) { return nullptr; }
extern "C" void wrap_object_setIvar(void* obj, void* ivar, void* value) {}
extern "C" void* wrap_property_copyAttributeList(void* property, uint32_t* outCount) { if (outCount) *outCount = 0; return nullptr; }
extern "C" void* wrap_sel_getUid(const char* str) { return wrap_sel_registerName(str); }

extern "C" void* wrap_cxx_getline(void* is_ptr, void* str_ptr) { wrap_cxx_string_clear(str_ptr); return is_ptr; }
extern "C" void* wrap_cxx_stringstream_ctor(void* this_ptr, void* str_ptr, int mode) { return this_ptr; }
extern "C" void* wrap_cxx_ios_init(void* this_ptr, void* sb_ptr) { return this_ptr; }
char wrap_ZSt7nothrow = 0;
extern "C" bool wrap_cxx_string_empty(void* this_ptr) { return wrap_cxx_string_size(this_ptr) == 0; }
extern "C" const char* wrap_cxx_string_c_str(void* this_ptr) { char** dest = (char**)this_ptr; return (dest && *dest) ? *dest : ""; }
extern "C" void* wrap_cxx_string_assign_string(void* this_ptr, void* other_ptr); // Fwd
extern "C" void* wrap_cxx_string_operator_assign(void* this_ptr, void* other_ptr) { return wrap_cxx_string_assign_string(this_ptr, other_ptr); }
extern "C" char* wrap_cxx_string_operator_index(void* this_ptr, size_t idx) { static char dummy=0; char** dest=(char**)this_ptr; if(dest&&*dest){ LibStdStringRep* rep=(LibStdStringRep*)(*dest)-1; if(idx<=rep->length) return &(*dest)[idx]; } return &dummy; }
extern "C" void* wrap_cxx_string_append_ptr_len(void* this_ptr, const char* str_ptr, size_t len);
extern "C" void* wrap_cxx_string_operator_plus_assign(void* this_ptr, void* other_ptr) { char** src = (char**)other_ptr; if(src&&*src) wrap_cxx_string_append_ptr_len(this_ptr, *src, (((LibStdStringRep*)(*src) - 1)->length)); return this_ptr; }
extern "C" void* wrap_cxx_string_clear(void* this_ptr) { char** dest = (char**)this_ptr; if(dest&&*dest) { wrap_cxx_string_M_mutate(this_ptr, 0, (((LibStdStringRep*)(*dest) - 1)->length), 0); } return this_ptr; }
extern "C" size_t wrap_cxx_string_find_ptr_len(void* this_ptr, const char* s, size_t pos, size_t n) { char** dest=(char**)this_ptr; if(dest&&*dest&&s){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); if(pos>l || pos+n>l) return -1; std::string_view sv(*dest, l); size_t f = sv.find(s, pos, n); return f==std::string_view::npos ? -1 : f; } return -1; }
extern "C" size_t wrap_cxx_string_find_ptr(void* this_ptr, const char* s, size_t pos) { return wrap_cxx_string_find_ptr_len(this_ptr, s, pos, s?strlen(s):0); }
extern "C" size_t wrap_cxx_string_rfind_ptr_len(void* this_ptr, const char* s, size_t pos, size_t n) { char** dest=(char**)this_ptr; if(dest&&*dest&&s){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); if(n==0) return pos<=l?pos:l; if(pos>l) pos=l; std::string_view sv(*dest, l); size_t f=sv.rfind(std::string_view(s,n), pos); return f==std::string_view::npos ? -1 : f; } return -1; }
extern "C" size_t wrap_cxx_string_rfind_ptr(void* this_ptr, const char* s, size_t pos) { return wrap_cxx_string_rfind_ptr_len(this_ptr, s, pos, s?strlen(s):0); }
extern "C" size_t wrap_cxx_string_find_first_of_ptr_len(void* this_ptr, const char* s, size_t pos, size_t n) { char** dest=(char**)this_ptr; if(dest&&*dest&&s){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); std::string_view sv(*dest, l); size_t f=sv.find_first_of(std::string_view(s,n), pos); return f==std::string_view::npos ? -1 : f; } return -1; }
extern "C" int wrap_cxx_string_compare_pos_len_ptr(void* this_ptr, size_t pos, size_t n, const char* s) { char** dest=(char**)this_ptr; if(dest&&*dest){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); if(pos>l)return 1; size_t rn = std::min(n, l-pos); return strncmp((*dest)+pos, s?s:"", rn); } return -1; }
extern "C" void* wrap_cxx_string_resize_char(void* this_ptr, size_t n, char c) { char** dest=(char**)this_ptr; if(dest&&*dest) { size_t l=(((LibStdStringRep*)(*dest)-1)->length); if(n<l) wrap_cxx_string_M_mutate(this_ptr, n, l-n, 0); else if(n>l) { wrap_cxx_string_M_mutate(this_ptr, l, 0, n-l); memset((*dest)+l, c, n-l); } } return this_ptr; }
extern "C" void* wrap_cxx_string_append_ptr(void* this_ptr, const char* s) { return wrap_cxx_string_append_ptr_len(this_ptr, s, s?strlen(s):0); }
extern "C" void* wrap_cxx_string_append_len_char(void* this_ptr, size_t n, char c) { char** dest=(char**)this_ptr; if(dest&&*dest) { size_t l=(((LibStdStringRep*)(*dest)-1)->length); wrap_cxx_string_M_mutate(this_ptr, l, 0, n); memset((*dest)+l, c, n); } return this_ptr; }

// --- STD::WSTRING MISSING ---
extern "C" void wrap_cxx_wstring_M_mutate(void* this_ptr, size_t pos, size_t len1, size_t len2) {
    int32_t** dest = (int32_t**)this_ptr; if (!dest || !*dest) return; LibStdStringRep* rep = (LibStdStringRep*)(*dest) - 1; size_t old_len = rep->length;
    if (pos > old_len) return; if (pos + len1 > old_len) len1 = old_len - pos; size_t new_len = old_len - len1 + len2;
    if (new_len > rep->capacity || rep->refcount > 1 || rep->refcount < 0) {
        size_t new_cap = (rep->capacity * 2 > new_len) ? (rep->capacity * 2) : new_len; LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + (new_cap + 1) * 4);
        new_rep->length = new_len; new_rep->capacity = new_cap; new_rep->refcount = 1; int32_t* new_data = (int32_t*)(new_rep + 1);
        memcpy(new_data, *dest, pos * 4); memcpy(new_data + pos + len2, (*dest) + pos + len1, (old_len - pos - len1) * 4); new_data[new_len] = 0;
        if (rep->refcount >= 0 && __sync_sub_and_fetch(&rep->refcount, 1) <= 0) free(rep); *dest = new_data;
    } else { memmove((*dest) + pos + len2, (*dest) + pos + len1, (old_len - pos - len1) * 4); rep->length = new_len; (*dest)[new_len] = 0; }
}
extern "C" void* wrap_cxx_wstring_M_leak_hard(void* this_ptr) {
    int32_t** str_obj = (int32_t**)this_ptr; if (str_obj && *str_obj) { LibStdStringRep* rep = (LibStdStringRep*)(*str_obj) - 1;
        if (rep->refcount > 1) { LibStdStringRep* new_rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + (rep->capacity + 1) * 4); new_rep->length = rep->length; new_rep->capacity = rep->capacity; new_rep->refcount = 1;
            int32_t* new_data = (int32_t*)(new_rep + 1); memcpy(new_data, *str_obj, (rep->length + 1) * 4); __sync_sub_and_fetch(&rep->refcount, 1); *str_obj = new_data; } else if (rep->refcount < 0) rep->refcount = 1; } return this_ptr;
}
extern "C" size_t wrap_cxx_wstring_size(void* this_ptr) { int32_t** dest = (int32_t**)this_ptr; return (dest && *dest) ? (((LibStdStringRep*)(*dest) - 1)->length) : 0; }
extern "C" bool wrap_cxx_wstring_empty(void* this_ptr) { return wrap_cxx_wstring_size(this_ptr) == 0; }
extern "C" int32_t* wrap_cxx_wstring_begin(void* this_ptr) { int32_t** dest = (int32_t**)this_ptr; return (dest && *dest) ? *dest : nullptr; }
extern "C" void* wrap_cxx_wstring_clear(void* this_ptr) { int32_t** dest = (int32_t**)this_ptr; if(dest&&*dest) { wrap_cxx_wstring_M_mutate(this_ptr, 0, (((LibStdStringRep*)(*dest) - 1)->length), 0); } return this_ptr; }
extern "C" void* wrap_cxx_wstring_erase_iter(void* this_ptr, int32_t* it) { int32_t** dest = (int32_t**)this_ptr; if(dest&&*dest&&it){ size_t pos = it - *dest; wrap_cxx_wstring_M_mutate(this_ptr, pos, 1, 0); } return this_ptr; }
extern "C" int32_t* wrap_cxx_wstring_operator_index(void* this_ptr, size_t idx) { static int32_t dummy=0; int32_t** dest=(int32_t**)this_ptr; if(dest&&*dest){ LibStdStringRep* rep=(LibStdStringRep*)(*dest)-1; if(idx<=rep->length) return &(*dest)[idx]; } return &dummy; }
extern "C" void* wrap_cxx_wstring_ctor_ptr_len_alloc(void* this_ptr, const int32_t* ptr, size_t len, void* alloc) { return wrap_cxx_wstring_assign_ptr_len(this_ptr, ptr, len); }
extern "C" void* wrap_cxx_wstring_ctor_str_pos_len(void* this_ptr, void* other_ptr, size_t pos, size_t len) { int32_t** src = (int32_t**)other_ptr; if(src&&*src){ size_t srclen = (((LibStdStringRep*)(*src)-1)->length); if(pos>srclen) pos=srclen; if(len>srclen-pos) len=srclen-pos; return wrap_cxx_wstring_assign_ptr_len(this_ptr, (*src)+pos, len); } return wrap_cxx_wstring_assign_ptr_len(this_ptr, nullptr, 0); }
extern "C" void* wrap_cxx_wstring_ctor_len_char_alloc(void* this_ptr, size_t n, int32_t c, void* alloc) { int32_t** str_obj = (int32_t**)this_ptr; *str_obj = AllocateLibStdWString(nullptr); wrap_cxx_wstring_M_mutate(this_ptr, 0, 0, n); for(size_t i=0; i<n; i++) (*str_obj)[i] = c; return this_ptr; }
extern "C" void* wrap_cxx_wstring_append_ptr_len(void* this_ptr, const int32_t* ptr, size_t len) { int32_t** dest=(int32_t**)this_ptr; if(dest&&*dest&&ptr){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); wrap_cxx_wstring_M_mutate(this_ptr, l, 0, len); memcpy((*dest)+l, ptr, len*4); } return this_ptr; }
extern "C" void* wrap_cxx_wstring_append_ptr(void* this_ptr, const int32_t* ptr) { return wrap_cxx_wstring_append_ptr_len(this_ptr, ptr, wstrlen(ptr)); }
extern "C" void* wrap_cxx_wstring_append_str(void* this_ptr, void* other_ptr) { int32_t** src = (int32_t**)other_ptr; if(src&&*src) wrap_cxx_wstring_append_ptr_len(this_ptr, *src, (((LibStdStringRep*)(*src) - 1)->length)); return this_ptr; }
extern "C" void* wrap_cxx_wstring_append_len_char(void* this_ptr, size_t n, int32_t c) { int32_t** dest=(int32_t**)this_ptr; if(dest&&*dest){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); wrap_cxx_wstring_M_mutate(this_ptr, l, 0, n); for(size_t i=0; i<n; i++) (*dest)[l+i] = c; } return this_ptr; }
extern "C" void* wrap_cxx_wstring_operator_plus_assign_ptr(void* this_ptr, const int32_t* ptr) { return wrap_cxx_wstring_append_ptr(this_ptr, ptr); }
extern "C" void* wrap_cxx_wstring_operator_plus_assign_str(void* this_ptr, void* other_ptr) { return wrap_cxx_wstring_append_str(this_ptr, other_ptr); }
extern "C" void* wrap_cxx_wstring_operator_plus_assign_char(void* this_ptr, int32_t c) { return wrap_cxx_wstring_append_len_char(this_ptr, 1, c); }
extern "C" size_t wrap_cxx_wstring_find_ptr_len(void* this_ptr, const int32_t* s, size_t pos) { int32_t** dest=(int32_t**)this_ptr; if(dest&&*dest&&s){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); size_t slen = wstrlen(s); if(pos>l || pos+slen>l) return -1; for(size_t i=pos; i<=l-slen; i++) { bool match=true; for(size_t j=0; j<slen; j++) if((*dest)[i+j]!=s[j]) { match=false; break; } if(match) return i; } } return -1; }
extern "C" size_t wrap_cxx_wstring_find_char(void* this_ptr, int32_t c, size_t pos) { int32_t** dest=(int32_t**)this_ptr; if(dest&&*dest){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); for(size_t i=pos; i<l; i++) if((*dest)[i]==c) return i; } return -1; }
extern "C" size_t wrap_cxx_wstring_rfind_char(void* this_ptr, int32_t c, size_t pos) { int32_t** dest=(int32_t**)this_ptr; if(dest&&*dest){ size_t l=(((LibStdStringRep*)(*dest)-1)->length); if(l==0) return -1; if(pos>=l) pos=l-1; for(size_t i=pos+1; i>0; i--) if((*dest)[i-1]==c) return i-1; } return -1; }
extern "C" void* wrap_cxx_wstring_substr(void* ret_ptr, void* this_ptr, size_t pos, size_t n) { 
    wrap_cxx_wstring_default_ctor(ret_ptr); 
    wrap_cxx_wstring_ctor_str_pos_len(ret_ptr, this_ptr, pos, n); 
    return ret_ptr; 
}
extern "C" int wrap_cxx_wstring_compare_ptr(void* this_ptr, const int32_t* ptr) { int32_t** dest=(int32_t**)this_ptr; const int32_t* p1=(dest&&*dest)?*dest:nullptr; const int32_t* p2=ptr; if(!p1)p1=(const int32_t*)&hle_dummy_struct; if(!p2)p2=(const int32_t*)&hle_dummy_struct; size_t l1=wstrlen(p1), l2=wstrlen(p2); size_t l=std::min(l1,l2); for(size_t i=0;i<l;i++) if(p1[i]!=p2[i]) return p1[i]<p2[i]?-1:1; return l1<l2?-1:(l1>l2?1:0); }
extern "C" int wrap_cxx_wstring_compare_str(void* this_ptr, void* other_ptr) { int32_t** src=(int32_t**)other_ptr; return wrap_cxx_wstring_compare_ptr(this_ptr, (src&&*src)?*src:nullptr); }

// --- IOS / LOCALE / IOSTREAM STUBS ---
extern "C" void* wrap_cxx_locale_ctor_str(void* this_ptr, const char* name) {
    InitDummyLocale();
    uint32_t* loc = (uint32_t*)this_ptr;
    loc[0] = (uint32_t)&g_dummy_locale_impl; // Подсовываем наш фейковый массив фасетов
    return this_ptr;
}
extern "C" void* wrap_cxx_locale_ctor(void* this_ptr) {
    InitDummyLocale();
    uint32_t* loc = (uint32_t*)this_ptr;
    loc[0] = (uint32_t)&g_dummy_locale_impl;
    return this_ptr;
}
extern "C" void* wrap_cxx_locale_dtor(void* this_ptr) { 
    return this_ptr; 
}
extern "C" void* wrap_cxx_ios_base_init_ctor(void* this_ptr) { return this_ptr; }
extern "C" void* wrap_cxx_ios_base_init_dtor(void* this_ptr) { return this_ptr; }
extern "C" void* wrap_cxx_ios_base_ctor(void* this_ptr) { return this_ptr; }
extern "C" void* wrap_cxx_ios_base_dtor(void* this_ptr) { return this_ptr; }
extern "C" char wrap_cxx_basic_ios_widen(void* this_ptr, char c) { return c; }
extern "C" void* wrap_cxx_ostream_put(void* this_ptr, char c) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) { LogToJava(std::string("GAME-COUT: ") + c); } return this_ptr; }
extern "C" void* wrap_cxx_ostream_flush(void* this_ptr) { return this_ptr; }
extern "C" void* wrap_cxx_ostream_insert_double(void* this_ptr, double d) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) { LogToJava("GAME-COUT: " + std::to_string(d)); } return this_ptr; }
extern "C" void* wrap_cxx_ostream_insert_longlong(void* this_ptr, long long v) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) { LogToJava("GAME-COUT: " + std::to_string(v)); } return this_ptr; }
extern "C" void* wrap_cxx_ostream_insert_char_ptr(void* this_ptr, const char* s, int n) { if (this_ptr == &wrap_ZSt4cout || this_ptr == &wrap_ZSt4cerr) { std::string str(s?s:"", n); LogToJava("GAME-COUT: " + str); } return this_ptr; }
extern "C" void* wrap_cxx_basic_stringbuf_str(void* ret_ptr, void* this_ptr) { wrap_cxx_string_default_ctor(ret_ptr); return ret_ptr; }
extern "C" void* wrap_cxx_string_Rep_S_create(size_t cap, size_t old_cap, void* alloc) { LibStdStringRep* rep = (LibStdStringRep*)malloc(sizeof(LibStdStringRep) + cap + 1); rep->length=0; rep->capacity=cap; rep->refcount=1; ((char*)(rep+1))[0]=0; return (char*)(rep+1); }

// --- ТРЕЙСИНГ C-API ФУНКЦИЙ (СЕТЬ И ВРЕМЯ) ---
extern bool g_machOLoaded;

extern "C" void* wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    void* res = mmap(addr, length, prot, flags, fd, offset);
    if (res == MAP_FAILED) {
        LogToJava("C-API-ERROR: mmap(" + std::to_string(length) + " bytes, prot=" + std::to_string(prot) + ") FAILED! Caller: " + GetModuleInfoForAddress(lr));
    } else if (g_machOLoaded) {
        LogToBlackBox("C-API-MEM: [mmap] addr=" + std::to_string((uintptr_t)res) + " len=" + std::to_string(length) + " Caller: " + GetModuleInfoForAddress(lr));
    }
    return res;
}

extern "C" int wrap_munmap(void *addr, size_t length) {
    uint32_t lr = (uint32_t)__builtin_return_address(0);
    if (g_machOLoaded) {
        LogToBlackBox("C-API-MEM: [munmap] addr=" + std::to_string((uintptr_t)addr) + " len=" + std::to_string(length) + " Caller: " + GetModuleInfoForAddress(lr));
    }
    return munmap(addr, length);
}

extern "C" int wrap_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) { 
    LogToJava("C-API-TRACE: connect() ИМИТАЦИЯ УСПЕХА"); 
    return 0; // Имитируем успешное подключение
}
extern "C" void* wrap_gethostbyname(const char *name) { 
    std::string sName = name ? name : "null";
    LogToJava("C-API-TRACE: gethostbyname(" + sName + ") ИМИТАЦИЯ (127.0.0.1)"); 
    
    static struct hostent he;
    static char hostname[] = "localhost";
    static char* aliases[] = { nullptr };
    static uint32_t addr = 0x0100007F; // 127.0.0.1 в network byte order
    static char* addr_list[] = { (char*)&addr, nullptr };
    
    he.h_name = hostname;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addr_list;
    
    return &he; 
}
extern "C" uint64_t wrap_mach_absolute_time() { 
    return Stub_mach_absolute_time();
}

// ---------------------------------------------

void LoadUserDefaults() {
    std::string path = g_sandboxDir + "Library/Preferences/standard.txt";
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        size_t sep = line.find("=");
        if (sep != std::string::npos) {
            std::string key = line.substr(0, sep);
            std::string val = line.substr(sep + 1);
            g_userDefaults[key] = CreateNSString(val);
        }
    }
}

void SaveUserDefaults() {
    std::string path = g_sandboxDir + "Library/Preferences/standard.txt";
    std::ofstream out(path);
    if (!out.is_open()) return;
    for (auto const& pair : g_userDefaults) {
        std::string key = pair.first;
        void* obj = pair.second;
        std::string valStr = "";
        if (obj && (uintptr_t)obj > 0x1000) {
            std::string clsName = GetObjCClassName(obj);
            if (clsName == "NSString" || clsName == "__CFConstantStringClassReference") {
                valStr = GetNSString(obj);
            } else if (clsName == "NSNumber") {
                uint32_t* ptr = (uint32_t*)obj;
                int type = ptr[2];
                if (type == 9 || type == 3) valStr = std::to_string((int)ptr[1]);
                else if (type == 12) { float f; memcpy(&f, &ptr[1], 4); valStr = std::to_string(f); }
                else if (type == 13) { double d; memcpy(&d, &ptr[1], 8); valStr = std::to_string(d); }
            } else {
                valStr = GetNSString(obj);
            }
        }
        if (!valStr.empty() && valStr != "Unknown" && valStr != "InvalidObj") {
            out << key << "=" << valStr << "\n";
        }
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm; 
    // SA_NODEFER позволяет поймать двойной краш (Double Fault), если подробный логгер упадет
    struct sigaction sa; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sa.sa_sigaction = CrashHandler; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGILL, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr); sigaction(SIGTRAP, &sa, nullptr); sigaction(SIGFPE, &sa, nullptr); sigaction(SIGSYS, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN); 
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL Java_com_damnwrapper32armv7_xaview_MainActivity_initWrapper(JNIEnv *env, jobject thiz, jstring workDir, jstring appBundle, jstring bundleId, jboolean logRender, jboolean logSound, jboolean logFs, jboolean logNet, jboolean logTodo, jboolean logRenderDebug, jboolean logFuncList, jboolean logHiddenClasses, jboolean logOther, jint spamFiltersMask, jboolean onScreenDebugOverlay, jboolean showPerfOverlay, jboolean nativeRootMmap, jint resWidth, jint resHeight, jint esMode, jint gpuOffloadMask) {
    g_mainActivity = env->NewGlobalRef(thiz); 
    g_gpuOffloadMask = gpuOffloadMask;
    g_surfaceWidth = resWidth;
    g_surfaceHeight = resHeight;
    g_logRender = logRender;
    g_logSound = logSound;
    g_logFs = logFs;
    g_logNet = logNet;
    g_logTodo = logTodo;
    g_logRenderDebug = logRenderDebug;
    g_logFuncList = logFuncList;
    g_logHiddenClasses = logHiddenClasses;
    g_logOther = logOther;
    g_spamFiltersMask = spamFiltersMask;
    g_disableLogging = !(logRender || logSound || logFs || logNet || logTodo || logRenderDebug || logFuncList || logHiddenClasses || logOther);
    g_esModeOption = esMode;
    g_onScreenDebugOverlay = onScreenDebugOverlay;
    g_showPerfOverlay = showPerfOverlay;
    g_nativeRootMmap = nativeRootMmap;
    // FPS инициализируется при первом кадре, чтобы избежать неверных расчетов
    g_fpsLastTimeMs = 0;
    g_startTimeMs = 0;
    g_fpsFrameCount = 0;
    g_totalFrames = 0;
    const char* wd = env->GetStringUTFChars(workDir, 0); 
    g_workDir = wd; 
    snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%sdamn32_log.txt", wd);
    env->ReleaseStringUTFChars(workDir, wd); 

    const char* bId = env->GetStringUTFChars(bundleId, 0);
    std::string bundleIdStr = bId;
    env->ReleaseStringUTFChars(bundleId, bId);
    
    // Инициализация изолированной песочницы для файловой системы приложения
    g_sandboxDir = g_workDir + "sandbox/" + bundleIdStr + "/";
    system(("mkdir -p '" + g_sandboxDir + "'").c_str());
    system(("mkdir -p '" + g_sandboxDir + "tmp/'").c_str());
    system(("mkdir -p '" + g_sandboxDir + "Documents/'").c_str());
    system(("mkdir -p '" + g_sandboxDir + "Library/Caches/'").c_str());
    system(("mkdir -p '" + g_sandboxDir + "Library/Preferences/'").c_str());

    LoadUserDefaults();

    // ДОБАВЛЕНО: Чтение TTF файла в память и инициализация stb_truetype
    std::string fontPath = g_workDir + "setup/Roboto-VariableFont_wdth,wght.ttf";
    std::ifstream fontFile(fontPath, std::ios::binary | std::ios::ate);
    if (fontFile.is_open()) {
        std::streamsize size = fontFile.tellg();
        fontFile.seekg(0, std::ios::beg);
        g_fontBuffer.resize(size);
        if (fontFile.read((char*)g_fontBuffer.data(), size)) {
            if (stbtt_InitFont(&g_fontInfo, g_fontBuffer.data(), 0)) {
                g_fontLoaded = true;
            }
        }
    }

    const char* bundle = env->GetStringUTFChars(appBundle, 0); 
    std::string bundleStr(bundle);
    g_appBundlePath = bundleStr; // Сохраняем путь к .app для NSBundle
    env->ReleaseStringUTFChars(appBundle, bundle); 
    LoadMachO(bundleStr);
}

bool g_gameThreadStarted = false;

extern "C" JNIEXPORT void JNICALL Java_com_damnwrapper32armv7_xaview_MainActivity_onSurfaceCreated(JNIEnv *env, jobject thiz, jobject surface) {
    if (g_gameThreadStarted) {
        LogToJava("onSurfaceCreated: Поверхность пересоздана (поворот экрана), но игра уже запущена. Блокируем дубликат.");
        return;
    }
    g_gameThreadStarted = true;

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    g_nativeWindow = window;
    
    ANativeWindow_setBuffersGeometry(g_nativeWindow, g_surfaceWidth, g_surfaceHeight, WINDOW_FORMAT_RGBA_8888);
    
    LogToJava("Render: Инициализация ANativeWindow для рендера! EGL переведен в режим PBuffer (Offscreen).");
    g_cpuColorBuffer.resize(g_surfaceWidth * g_surfaceHeight, 0xFF000000);
    
    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(g_eglDisplay, 0, 0);
    // ФИКС КРАША 0x8: Обязательно добавляем EGL_WINDOW_BIT, иначе драйвер упадет при eglSwapBuffers
    // ФИКС КРАША GL_STENCIL_BUFFER_BIT: Добавляем EGL_STENCIL_SIZE и EGL_ALPHA_SIZE для Adreno
    const EGLint attribs[] = { 
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, 
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, 
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, 
        EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8, 
        EGL_NONE 
    };
    EGLConfig config; EGLint numConfigs; eglChooseConfig(g_eglDisplay, attribs, &config, 1, &numConfigs);
    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE }; g_eglContext = eglCreateContext(g_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_gpuOffloadMask & 1) {
        g_eglSurface = eglCreateWindowSurface(g_eglDisplay, config, g_nativeWindow, nullptr);
    } else {
        // ФИКС СПЛЮСНУТОГО ЭКРАНА: Создаем PBuffer в размер экрана игры, а не 64x64
        const EGLint pbufferAttribs[] = { EGL_WIDTH, g_surfaceWidth, EGL_HEIGHT, g_surfaceHeight, EGL_NONE };
        g_eglSurface = eglCreatePbufferSurface(g_eglDisplay, config, pbufferAttribs);
    }
    
    eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    LogToJava("onSurfaceCreated: EGL контекст отвязан от главного потока, создаем execThread...");
    pthread_t execThread; pthread_create(&execThread, nullptr, NativeExecutionThread, nullptr);
}

extern "C" JNIEXPORT void JNICALL Java_com_damnwrapper32armv7_xaview_MainActivity_onSurfaceChanged(JNIEnv *env, jobject thiz, jint width, jint height) { 
    // Не перезаписываем g_surfaceWidth/g_surfaceHeight, чтобы сохранить запрошенное логическое разрешение.
}

extern "C" JNIEXPORT void JNICALL Java_com_damnwrapper32armv7_xaview_MainActivity_onTouchEventNative(JNIEnv *env, jobject thiz, jint actionMasked, jint pointerId, jfloat x, jfloat y) {
    if (g_ignoringInteractionEvents > 0) return;
    void* activeView = g_presentedView ? g_presentedView : g_mainView; if (!activeView) return;

    bool isDown = (actionMasked == 0 || actionMasked == 5);
    bool isUp = (actionMasked == 1 || actionMasked == 6);
    bool isMove = (actionMasked == 2);
    bool isCancel = (actionMasked == 3);

    // =====================================
    // ПРОСЧЕТ НАЖАТИЙ НА НАШ КАСТОМНЫЙ UI
    // =====================================
    void* hitElement = nullptr;
    for (auto const& pair : g_views) {
        void* view = pair.first;
        auto& v = g_views[view];
        if (v.type.empty() || v.hidden || !v.userInteraction) continue;
        
        float absX = 0, absY = 0;
        void* curr = view;
        bool connected = false;
        bool anyHidden = false;
        while (curr) {
            if (g_views.count(curr)) {
                if (g_views[curr].hidden) { anyHidden = true; break; }
                absX += g_views[curr].frame[0]; absY += g_views[curr].frame[1];
            }
            if (curr == activeView) { connected = true; break; }
            curr = g_views.count(curr) ? g_views[curr].parent : nullptr;
        }
        
        if (connected && !anyHidden) {
            if (x >= absX && x <= absX + v.frame[2] && y >= absY && y <= absY + v.frame[3]) {
                if (v.type == "UIButton" || v.type == "UISwitch") hitElement = view;
            }
        }
    }

    if (isDown) {
        if (hitElement && (g_views[hitElement].type == "UIButton" || g_views[hitElement].type == "UISwitch")) {
            g_pointerToUI[pointerId] = hitElement;
            if (g_views[hitElement].type == "UIButton") g_views[hitElement].buttonPressed = true;
            return; // Поглощаем тач
        }
    } else if (isMove) {
        if (g_pointerToUI.count(pointerId)) {
            void* activeUI = g_pointerToUI[pointerId];
            if (activeUI != hitElement) {
                if (g_views[activeUI].type == "UIButton") g_views[activeUI].buttonPressed = false;
            } else {
                if (g_views[activeUI].type == "UIButton") g_views[activeUI].buttonPressed = true;
            }
            return; // Поглощаем тач
        }
    } else if (isUp || isCancel) {
        if (g_pointerToUI.count(pointerId)) {
            void* activeUI = g_pointerToUI[pointerId];
            if (isUp && activeUI == hitElement) {
                if (g_views[activeUI].type == "UIButton") {
                    for (auto& btn : g_buttons) {
                        if (btn.button == activeUI && btn.target && !btn.action.empty()) {
                            pthread_mutex_lock(&g_mainQueueMutex);
                            g_mainQueue.push_back({btn.target, btn.action.c_str(), btn.button});
                            pthread_mutex_unlock(&g_mainQueueMutex);
                            break;
                        }
                    }
                } else if (g_views[activeUI].type == "UISwitch") {
                    g_views[activeUI].switchState = !g_views[activeUI].switchState;
                }
            }
            if (g_views[activeUI].type == "UIButton") g_views[activeUI].buttonPressed = false;
            g_pointerToUI.erase(pointerId);
            return; // Поглощаем тач
        }
    }

    // =====================================
    // ПРОБРОС ТАЧЕЙ В САМУ ИГРУ
    // =====================================
    if (!g_hleClasses.count("FakeUITouch")) g_hleClasses["FakeUITouch"] = new HLEClass{0xDEADBEEF, "FakeUITouch"};
    if (!g_hleClasses.count("FakeNSSet")) g_hleClasses["FakeNSSet"] = new HLEClass{0xDEADBEEF, "FakeNSSet"};

    // Координаты уже отмасштабированы в Java, используем их напрямую
    float scaledX = x;
    float scaledY = y;

    // Принудительный поворот осей тачскрина убран. 
    // Визуал мы уже выровняли в CPUExtractAndDraw, а UIWindow игры (в SMB2)
    // и так работает в горизонтальных координатах.

    FakeUITouch* t = nullptr;
    if (g_activeTouches.count(pointerId)) t = g_activeTouches[pointerId];
    else { t = new FakeUITouch{(uint32_t)g_hleClasses["FakeUITouch"], "FakeUITouch", scaledX, scaledY, activeView, (uint32_t)pointerId}; g_activeTouches[pointerId] = t; }
    t->x = scaledX; t->y = scaledY;
    
    FakeNSSet* set = new FakeNSSet{(uint32_t)g_hleClasses["FakeNSSet"], "FakeNSSet", {(void*)t}};
    const char* method = nullptr; 
    if (isDown) method = "touchesBegan:withEvent:";
    else if (isUp) method = "touchesEnded:withEvent:";
    else if (isMove) method = "touchesMoved:withEvent:";
    else if (isCancel) method = "touchesCancelled:withEvent:";
    
    if (method) { 
        pthread_mutex_lock(&g_mainQueueMutex);
        // Отправляем тач во View
        g_mainQueue.push_back({activeView, method, (void*)set});
        
        // ВАЖНО: Дублируем тач в ViewController (Responder Chain), так как логика часто там
        for (auto const& pair : g_viewControllersViews) {
            if (pair.second == activeView) {
                g_mainQueue.push_back({pair.first, method, (void*)set});
                break;
            }
        }
        pthread_mutex_unlock(&g_mainQueueMutex);
    }
    if (isUp || isCancel) { g_activeTouches.erase(pointerId); }
}

extern "C" JNIEXPORT void JNICALL Java_com_damnwrapper32armv7_xaview_MainActivity_onVideoFinishedNative(JNIEnv *env, jobject thiz, jint ptrId) {
    LogToJava("HLE: onVideoFinishedNative успешно вызван из Java!");
    pthread_mutex_lock(&g_videoMutex);
    g_pendingVideoFinishes.push_back((void*)(uintptr_t)ptrId);
    pthread_mutex_unlock(&g_videoMutex);
}

extern "C" JNIEXPORT void JNICALL Java_com_damnwrapper32armv7_xaview_MainActivity_onSensorChangedNative(JNIEnv *env, jobject thiz, jint sensorType, jfloat x, jfloat y, jfloat z) {
    if (sensorType == 1) { // Accelerometer
        g_latestAccelX = (double)x;
        g_latestAccelY = (double)y;
        g_latestAccelZ = (double)z;
        if (g_accelerometerDelegate) {
            if (!g_hleClasses.count("FakeUIAcceleration")) {
                g_hleClasses["FakeUIAcceleration"] = new HLEClass{0xDEADBEEF, "FakeUIAcceleration"};
            }
            
            FakeUIAcceleration* acc = new FakeUIAcceleration{
                (uint32_t)g_hleClasses["FakeUIAcceleration"], 
                "FakeUIAcceleration", 
                (double)Stub_mach_absolute_time() / 1000000000.0, 
                (double)x, 
                (double)y, 
                (double)z
            };
            
            static uint32_t* accelInst = nullptr;
            if (!accelInst) {
                accelInst = (uint32_t*)calloc(1, 32);
                accelInst[0] = (uint32_t)g_hleClasses["UIAccelerometer"];
            }

            pthread_mutex_lock(&g_mainQueueMutex);
            g_mainQueue.push_back({g_accelerometerDelegate, "accelerometer:didAccelerate:", (void*)accelInst, (void*)acc});
            pthread_mutex_unlock(&g_mainQueueMutex);
        }
    } else if (sensorType == 4) { // Gyroscope
        // Для iOS 4+ гироскоп работает через CMMotionManager (CoreMotion).
        // Оставляю эту ветку-заглушку. Если какая-то игра запросит гироскоп, 
        // данные уже прилетают сюда, и останется только прикрутить HLE CoreMotion.
    }
}
