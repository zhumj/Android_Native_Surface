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

    bool log = true;
    // dynamic contents
    vector<string> frame_names;
    vector<cv::Mat *> frames;
    float gain;


    void showMainContents();

public:
    ImageViewer();

    void imshow(string, cv::Mat *);

    void imshow(cv::Mat *);

    void show();

    float getGain();
};

ImageViewer::ImageViewer() {
    if (!initDraw(true)) {
        return;
    }
    Init_touch_config();
    gain = 1.0f;
}

float ImageViewer::getGain() {
    return gain;
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
    ImGui::Text("height: %.0f width: %.0f", ImGui::GetWindowHeight(), ImGui::GetWindowWidth());
    ImGui::End();
}

void ImageViewer::show() {
    drawBegin();
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

    drawEnd();

    // clear resources
    for (int i = 0; i < frames.size(); i++) {
        delete my_textures[i];
    }

    frame_names.clear();
    frames.clear();
    my_textures.clear();

}

//---------------------------------------------------------------------


int main(int, char **) {
    ImageViewer gui;
    cv::VideoCapture video("/sdcard/a.mp4");
//    cv::VideoCapture video(2,CAP_ANDROID);
    if (!video.isOpened()) {
        cout << "打开失败" << endl;
        return -1;
    }

    double count = video.get(CAP_PROP_FRAME_COUNT);
    double rate = video.get(CAP_PROP_FPS);

    int width = (int) video.get(CAP_PROP_FRAME_WIDTH);
    int height = (int) video.get(CAP_PROP_FRAME_HEIGHT);

    cout << "帧率为:" << " " << std::to_string(rate) << endl;
    cout << "总帧数为:" << " " << std::to_string(count) << endl;//输出帧总数
    cout << "width:" << " " << std::to_string(width) << endl;//输出帧总数
    cout << "height:" << " " << std::to_string(height) << endl;//输出帧总数

    cv::Mat frame, frame2, img;

//    img = cv::imread("/sdcard/b.jpg");
//    cv::resize(img, img, cv::Size(0, 0), 0.5, 0.5, cv::INTER_LINEAR);

    while (video.read(frame)) {

//        cv::resize(frame, frame, cv::Size(0, 0), 1.5, 1.5, cv::INTER_LINEAR);
        float g = gui.getGain();
        cv::resize(frame, frame2, cv::Size(0, 0), 0.5, 0.5, cv::INTER_LINEAR);

        frame2.convertTo(frame2, CV_8U, g, 0);
        // show halfsize image
//        cv::resize(frame, frame2, cv::Size(0, 0), 0.5, 0.5, cv::INTER_LINEAR);

        gui.imshow("video", &frame2);
//        gui.imshow("img", &img);
        // make quartersize image and show
//            gui.imshow("quater", &frame2);
        gui.show();
        std::this_thread::sleep_for(1ms);
//            if (cv::waitKey(20) >= 0) {
//            }
    }
    video.release();
    shutdown();

    return 0;
}
