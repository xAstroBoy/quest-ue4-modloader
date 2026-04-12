// modloader/src/main.cpp
// Entry point — JNI_OnLoad spawns TWO threads:
//   1. Early PAK hook thread — zero delay, polls for libUE4.so, installs Dobby
//      hooks on MountAllPakFiles BEFORE the engine's init runs
//   2. Main boot thread — 5s delay, initializes everything else (logger, lua, mods)

#include "modloader/init.h"
#include "modloader/logger.h"
#include "modloader/pak_mounter.h"
#include "modloader/game_profile.h"

#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <android/log.h>
#include <atomic>
#include <cstring>

#define LOG_TAG "UEModLoader"

// ═══ Global JavaVM pointer — stored from JNI_OnLoad, used by paths + notifications ═══
static JavaVM *g_jvm = nullptr;

extern "C" JavaVM *get_stored_jvm()
{
    return g_jvm;
}

// ═══ Find libUE4.so base address via dl_iterate_phdr ════════════════════
struct PhdrCallbackData
{
    uintptr_t base;
    bool found;
};

static int phdr_callback(struct dl_phdr_info *info, size_t /*size*/, void *data)
{
    auto *d = static_cast<PhdrCallbackData *>(data);
    if (info->dlpi_name)
    {
        // Match any known UE engine library name
        if (strstr(info->dlpi_name, "libUE4.so") ||
            strstr(info->dlpi_name, "libUnreal4.so") ||
            strstr(info->dlpi_name, "libUnreal.so"))
        {
            d->base = static_cast<uintptr_t>(info->dlpi_addr);
            d->found = true;
            return 1; // stop iteration
        }
    }
    return 0;
}

static uintptr_t find_libue4_base()
{
    PhdrCallbackData data{0, false};
    dl_iterate_phdr(phdr_callback, &data);
    return data.found ? data.base : 0;
}

// ═══ Early PAK hook thread — installs Dobby hooks BEFORE engine init ════
// This thread runs with ZERO delay. It polls for libUE4.so every 10ms.
// As soon as the library is mapped, it installs Dobby hooks on:
//   - FPakPlatformFile::Mount (captures the this pointer)
//   - MountAllPakFiles #1 and #2 (mounts custom PAKs inside engine's pass)
// By the time the engine calls MountAllPakFiles during its init, our hooks
// are already in place and custom PAKs get mounted in the same batch.
static void *early_pak_thread(void * /*arg*/)
{
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "[PAK-EARLY] Thread started — polling for libUE4.so...");

    uintptr_t base = 0;
    void *lib_handle = nullptr;
    int attempts = 0;
    const int max_attempts = 500; // 500 * 10ms = 5 seconds max wait

    while (attempts < max_attempts)
    {
        // Try dl_iterate_phdr first — works even if dlopen hasn't been called by us
        base = find_libue4_base();
        if (base != 0)
        {
            // Try profile-specific lib name first, then fallbacks
            const std::string &plib = game_profile::engine_lib_name();
            lib_handle = dlopen(plib.c_str(), RTLD_NOLOAD | RTLD_NOW);
            if (!lib_handle)
                lib_handle = dlopen("libUE4.so", RTLD_NOLOAD | RTLD_NOW);
            if (!lib_handle)
                lib_handle = dlopen("libUnreal4.so", RTLD_NOLOAD | RTLD_NOW);
            if (!lib_handle)
                lib_handle = dlopen("libUnreal.so", RTLD_NOLOAD | RTLD_NOW);
            break;
        }

        usleep(10000); // 10ms
        attempts++;
    }

    if (base == 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "[PAK-EARLY] Engine lib NOT found after %d attempts (%.1fs) — early hooks FAILED",
                            attempts, attempts * 0.01f);
        return nullptr;
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "[PAK-EARLY] Engine lib found after %d attempts (%.0fms) — base=0x%lX, handle=%p",
                        attempts, attempts * 10.0f, (unsigned long)base, lib_handle);

    // Install the hooks immediately — before engine calls MountAllPakFiles
    pak_mounter::install_early_hooks(base, lib_handle);

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "[PAK-EARLY] Thread done — hooks installed, waiting for engine to init");

    return nullptr;
}

// ═══ Background boot thread ═════════════════════════════════════════════
static void *boot_thread(void *arg)
{
    (void)arg;

    // 5-second delay — give the Activity time to fully initialize
    // We need ActivityThread.currentApplication() to return non-null
    // for JNI path resolution and notifications
    usleep(5000000); // 5 seconds

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Boot thread started — initializing modloader");

    bool ok = init::boot();

    if (ok)
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Modloader initialization complete");
    }
    else
    {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Modloader initialization FAILED");
    }

    return nullptr;
}

// ═══ JNI_OnLoad — called when the .so is loaded by System.loadLibrary ══
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;

    // Store the JavaVM* globally — this is the ONLY reliable way to get it
    g_jvm = vm;

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "JNI_OnLoad — UE ModLoader library loaded, JavaVM stored @ %p", (void *)vm);

    // Detect game profile early — needed for early PAK thread to know which lib to wait for
    game_profile::init();

    // ── Thread 1: Early PAK hooks — zero delay ──────────────────────────
    {
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1 * 1024 * 1024); // 1 MB stack (lightweight)

        int ret = pthread_create(&tid, &attr, early_pak_thread, nullptr);
        pthread_attr_destroy(&attr);

        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "Failed to create early PAK hook thread: %d", ret);
        }
        else
        {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "Early PAK hook thread spawned — will install hooks ASAP");
        }
    }

    // ── Thread 2: Main boot — 5s delay for full init ────────────────────
    {
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 4 * 1024 * 1024); // 4 MB stack

        int ret = pthread_create(&tid, &attr, boot_thread, nullptr);
        pthread_attr_destroy(&attr);

        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "Failed to create boot thread: %d", ret);
        }
        else
        {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "Boot thread spawned — modloader will initialize in 5s");
        }
    }

    return JNI_VERSION_1_6;
}

// ═══ Constructor attribute — fallback if JNI_OnLoad is not called ═══════
__attribute__((constructor)) static void modloader_constructor()
{
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "Constructor called — library loaded into process");
}
