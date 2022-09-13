#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include <opencv2/opencv.hpp>


#include <EGL/egl.h>
#include <dlfcn.h>
#include <android/native_window.h>
#include <GLES3/gl32.h>

// User libs
#include "native_surface/extern_function.h"
#include <imgui.h>

#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_android.h>

#include <touch.h>


using namespace std;
using namespace cv;

//---------------------------------------------------------------------

class ImageTexture {
private:
    int width, height;
    GLuint my_opengl_texture;

public:
    ~ImageTexture();

    void setImage(cv::Mat *frame);    // from cv::Mat (BGR)
    void setImage(string filename);  // from file
    void *getOpenglTexture();

    ImVec2 getSize();
};

ImageTexture::~ImageTexture() {
    glBindTexture(GL_TEXTURE_2D, 0);  // unbind texture
    glDeleteTextures(1, &my_opengl_texture);
}

void ImageTexture::setImage(cv::Mat *pframe) {
    width = pframe->cols;
    height = pframe->rows;

    glGenTextures(1, &my_opengl_texture);
    glBindTexture(GL_TEXTURE_2D, my_opengl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // Some enviromnent doesn't support GP_BGR
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, /*GL_BGR*/GL_RGB,
                 GL_UNSIGNED_BYTE, (pframe->data));

}

void ImageTexture::setImage(string filename) {
    cv::Mat frame = cv::imread(filename);
    setImage(&frame);
}

void *ImageTexture::getOpenglTexture() {
    return (void *) (intptr_t) my_opengl_texture;
}

ImVec2 ImageTexture::getSize() { return ImVec2(width, height); };

//---------------------------------------------------------------------

class ImageViewer {
private:
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext gl_context = EGL_NO_CONTEXT;
    ANativeWindow *native_window;
    ExternFunction externFunction;
    EGLConfig config;
    EGLSurface surface = EGL_NO_SURFACE;

    bool log = true;
    // dynamic contents
    vector<string> frame_names;
    vector<cv::Mat *> frames;
    float gain;

    int init();

    void render();

    void showMainContents();

public:
    ImageViewer();

    bool handleEvent();

    void imshow(string, cv::Mat *);

    void imshow(cv::Mat *);

    void show();

    void exit();

    float getGain();
};

ImageViewer::ImageViewer() {
    init();
    screen_config();
    cout << "height:" << displayInfo.width << " width:" << displayInfo.height << endl;
    Init_touch_config();
    // dynamic contents
    gain = 1.0f;
}

float ImageViewer::getGain() {
    return gain;
}

int ImageViewer::init() {
    MDisplayInfo displayInfo = externFunction.getDisplayInfo();
    native_window = externFunction.createNativeWindow("Ssage",
                                                      displayInfo.width, displayInfo.height, false);
    ANativeWindow_acquire(native_window);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        printf("eglGetDisplay error=%u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("eglGetDisplay ok\n");
    }
    if (eglInitialize(display, 0, 0) != EGL_TRUE) {
        printf("eglInitialize error=%u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("eglInitialize ok\n");
    }
    EGLint num_config = 0;
    const EGLint attribList[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_BLUE_SIZE, 5,   //-->delete
            EGL_GREEN_SIZE, 6,  //-->delete
            EGL_RED_SIZE, 5,    //-->delete
            EGL_BUFFER_SIZE, 32,  //-->new field
            EGL_DEPTH_SIZE, 16,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE
    };
    if (eglChooseConfig(display, attribList, nullptr, 0, &num_config) != EGL_TRUE) {
        printf("eglChooseConfig  error=%u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("num_config=%d\n", num_config);
    }
    if (!eglChooseConfig(display, attribList, &config, 1, &num_config)) {
        printf("eglChooseConfig  error=%u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("eglChooseConfig ok\n");
    }
    EGLint egl_format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &egl_format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, egl_format);
    const EGLint attrib_list[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    gl_context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrib_list);
    if (gl_context == EGL_NO_CONTEXT) {
        printf("eglCreateContext  error = %u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("eglCreateContext ok\n");
    }
    surface = eglCreateWindowSurface(display, config, native_window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        printf("eglCreateWindowSurface  error = %u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("eglCreateWindowSurface ok\n");
    }
    if (!eglMakeCurrent(display, surface, surface, gl_context)) {
        printf("eglMakeCurrent  error = %u\n", glGetError());
        return -1;
    }
    if (log) {
        printf("eglMakeCurrent ok\n");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImGui::StyleColorsDark();
    ImGui_ImplAndroid_Init(native_window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 22.0f;
    io.Fonts->AddFontDefault(&font_cfg);
    ImGui::GetStyle().ScaleAllSizes(3.0f);
    g_Initialized = true;
    return 0;
}

void ImageViewer::render() {
    // Rendering

    ImGuiIO &imGuiIo = ImGui::GetIO();

    glViewport(0.0f, 0.0f, (int) imGuiIo.DisplaySize.x, (int) imGuiIo.DisplaySize.y);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT); // GL_DEPTH_BUFFER_BIT
    glFlush();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(display, surface);
}

void ImageViewer::exit() {
    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, gl_context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(display, surface);
        }
        eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    gl_context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
    ANativeWindow_release(native_window);
}


void ImageViewer::imshow(string frame_name, cv::Mat *frame) {
    frame_names.push_back(frame_name);
    frames.push_back(frame);
}

void ImageViewer::imshow(cv::Mat *frame) {
    imshow("image:" + to_string(frames.size()), frame);
}

void ImageViewer::showMainContents() {
    ImGui::Begin("Main");

    ImGui::SliderFloat("gain", &gain, 0.0f, 2.0f, "%.3f");
    ImGui::Text("IsWindowFocused = %d", ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow));
    ImGui::Text("%.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                ImGui::GetIO().Framerate);
    ImGui::End();
}

void ImageViewer::show() {
    // Start the Dear ImGui frame
//    ImGui_ImplOpenGL3_NewFrame();
//    ImGui_ImplSDL2_NewFrame(window);

    MDisplayInfo displayInfo = externFunction.getDisplayInfo();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(displayInfo.width, displayInfo.height);

    ImGui::NewFrame();

    showMainContents();

    // initialize textures
    vector<ImageTexture *> my_textures;
    for (int i = 0; i < frames.size(); i++) {
        my_textures.push_back(new ImageTexture());
    }

    // imshow windows
    for (int i = 0; i < frames.size(); i++) {
        cv::Mat *frame = frames[i];

        string window_name;
        if (frame_names.size() <= i) {
            window_name = "image:" + to_string(i);
        } else {
            window_name = frame_names[i];
        }
        ImGui::Begin(window_name.c_str());

        my_textures[i]->setImage(frame);
        ImGui::Image((ImTextureID) my_textures[i]->getOpenglTexture(), my_textures[i]->getSize());
        ImGui::End();
    }

    render();

    // clear resources
    for (int i = 0; i < frames.size(); i++) {
        delete my_textures[i];
    }

    frame_names.clear();
    frames.clear();
    my_textures.clear();

}

bool ImageViewer::handleEvent() {

    return true;
}

//---------------------------------------------------------------------


int main(int, char **) {
    ImageViewer gui;
//    cv::VideoCapture video("/sdcard/a.mp4");
    cv::VideoCapture video(2,CAP_ANDROID);
    if (!video.isOpened()) {
        cout << "打开失败" << endl;
        return -1;
    }

    double count = video.get(CAP_PROP_FRAME_COUNT);
    double rate = video.get(CAP_PROP_FPS);

    int width = (int) video.get(CAP_PROP_FRAME_WIDTH);
    int height = (int) video.get(CAP_PROP_FRAME_HEIGHT);

    cout << "帧率为:" << " " << std::to_string(rate) << endl;
    cout << "总帧数为:" << " " << std::to_string(count)  << endl;//输出帧总数
    cout << "width:" << " " << std::to_string(width)  << endl;//输出帧总数
    cout << "height:" << " " << std::to_string(height)  << endl;//输出帧总数

    cv::Mat frame, frame2, img;

//    img = cv::imread("/sdcard/b.jpg");
//    cv::resize(img, img, cv::Size(0, 0), 0.5, 0.5, cv::INTER_LINEAR);

    while (video.read(frame)) {

        cv::resize(frame, frame, cv::Size(0, 0), 1.5, 1.5, cv::INTER_LINEAR);
            float g = gui.getGain();
            frame.convertTo(frame, CV_8U, g, 0);
        // show halfsize image
        gui.imshow("video", &frame);
//        gui.imshow("img", &img);
        // make quartersize image and show
//            cv::resize(frame, frame2, cv::Size(0, 0), 0.5, 0.5, cv::INTER_LINEAR);
//            gui.imshow("quater", &frame2);
        gui.show();
        std::this_thread::sleep_for(1ms);
//            if (cv::waitKey(20) >= 0) {
//            }
    }
    video.release();
    gui.exit();

    return 0;
}
