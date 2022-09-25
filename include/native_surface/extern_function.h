//
// Created by user on 2022/9/9.
//

#ifndef NATIVESURFACE_EXTERN_FUNCTION_H
#define NATIVESURFACE_EXTERN_FUNCTION_H
// System libs
#include <iostream>
#include <cstdlib>
#include <android/api-level.h>
// User libs
#include "utils.h"
#include <android/native_window.h>

struct MDisplayInfo {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t orientation{0};
};

// 方法指针
struct FuncPointer {
    void *func_createNativeWindow;
    //    更多可选参数
    void *func_more_createNativeWindow;
    void *func_getDisplayInfo;
};


class ExternFunction {
public:

    FuncPointer funcPointer{};

    ExternFunction();

    /**
     * 创建 native surface
     * @param surface_name 创建名称
     * @param screen_width 创建宽度
     * @param screen_height 创建高度
     * @param author 是否打印作者信息
     */
    ANativeWindow *
    createNativeWindow(const char *surface_name, uint32_t screen_width, uint32_t screen_height, bool author) const;

    /**
     * (更多可选参数_暂时只支持安卓12)创建 native surface
     * @param surface_name 创建名称
     * @param screen_width 创建宽度
     * @param screen_height 创建高度
     * @param format format
     * @param flags flags
     * @param author 是否打印作者信息
     * @return
     */
    ANativeWindow *
    createNativeWindow(const char *surface_name, uint32_t screen_width, uint32_t screen_height, uint32_t format,
                       uint32_t flags, bool author) const;

    /**
     * 获取屏幕宽高以及旋转状态
     */
    MDisplayInfo getDisplayInfo() const;

};


#endif //NATIVESURFACE_EXTERN_FUNCTION_H
