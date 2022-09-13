//
// Created by user on 2022/9/9.
//

#include "extern_function.h"
#include "aosp_8/native_surface_8.h"
#include "aosp_8_1/native_surface_8_1.h"
#include "aosp_9/native_surface_9.h"
#include "aosp_10/native_surface_10.h"
#include "aosp_11/native_surface_11.h"
#include "aosp_12/native_surface_12.h"
#include "aosp_13/native_surface_13.h"

static void *handle;// 动态库方案

ExternFunction::ExternFunction() {
    printf("android api level:%d\n", get_android_api_level());
    if (!handle) {
        if (get_android_api_level() == 33) { // 安卓13支持
            exec_native_surface("settings put global block_untrusted_touches 0");
#ifdef __aarch64__
            handle = dlblob(&native_surface_13_64, sizeof(native_surface_13_64)); // 64位支持
#else
            handle = dlblob(&native_surface_13_32, sizeof(native_surface_13_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else if (get_android_api_level() == /*__ANDROID_API_S__*/ 31) { // 安卓12支持
            exec_native_surface("settings put global block_untrusted_touches 0");
#ifdef __aarch64__
            handle = dlblob(&native_surface_12_64, sizeof(native_surface_12_64)); // 64位支持
#else
            handle = dlblob(&native_surface_12_32, sizeof(native_surface_12_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else if (get_android_api_level() == /*__ANDROID_API_R__*/ 30) { // 安卓11支持
#ifdef __aarch64__
            handle = dlblob(&native_surface_11_64, sizeof(native_surface_11_64)); // 64位支持
#else
            handle = dlblob(&native_surface_11_32, sizeof(native_surface_11_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else if (get_android_api_level() == __ANDROID_API_Q__) { // 安卓10支持
#ifdef __aarch64__
            handle = dlblob(&native_surface_10_64, sizeof(native_surface_10_64)); // 64位支持
#else
            handle = dlblob(&native_surface_10_32, sizeof(native_surface_10_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else if (get_android_api_level() == __ANDROID_API_P__) { // 安卓9支持
#ifdef __aarch64__
            handle = dlblob(&native_surface_9_64, sizeof(native_surface_9_64)); // 64位支持
#else
            handle = dlblob(&native_surface_9_32, sizeof(native_surface_9_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else if (get_android_api_level() == __ANDROID_API_O_MR1__) { // 安卓8.1支持
#ifdef __aarch64__
            handle = dlblob(&native_surface_8_1_64, sizeof(native_surface_8_1_64)); // 64位支持
#else
            handle = dlblob(&native_surface_8_1_32, sizeof(native_surface_8_1_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else if (get_android_api_level() == __ANDROID_API_O__) { // 安卓8.0支持
#ifdef __aarch64__
            handle = dlblob(&native_surface_8_64, sizeof(native_surface_8_64)); // 64位支持
#else
            handle = dlblob(&native_surface_8_32, sizeof(native_surface_8_32)); // 32位支持 <<-- 其实很没必要 未测试
#endif
        } else {
            printf("Sorry, level:%d Don't Support~", get_android_api_level());
            exit(0);
        }
    }
    funcPointer.func_createNativeWindow = dlsym(handle, "_Z18createNativeWindowPKcjjb");
    // 获取屏幕信息
    funcPointer.func_getDisplayInfo = dlsym(handle, "_Z14getDisplayInfov");
}

/**
 * 创建 native surface
 * @param surface_name 创建名称
 * @param screen_width 创建宽度
 * @param screen_height 创建高度
 * @param author 是否打印作者信息
 * @return
 */
ANativeWindow *
ExternFunction::createNativeWindow(const char *surface_name, uint32_t screen_width, uint32_t screen_height,
                                   bool author) const {
    return ((ANativeWindow *(*)(
            const char *, uint32_t, uint32_t, bool))
            (funcPointer.func_createNativeWindow))(surface_name, screen_width, screen_height, author);
}

/**
 * 获取屏幕宽高以及旋转状态
 */
MDisplayInfo ExternFunction::getDisplayInfo() const {
    return ((MDisplayInfo (*)()) (funcPointer.func_getDisplayInfo))();
}
