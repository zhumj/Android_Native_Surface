
#include "opencv2/imgcodecs.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/opencv.hpp"

using namespace cv;

/**
 * opencv 库测试
 */
int main() {
    // https://blog.csdn.net/weicao1990/article/details/53379881

    //打开摄像头,VideoCapture的解析
    VideoCapture cap(3);
    if (!cap.isOpened()) {
        std::cout << "打开失败" << std::endl;
        // 检测一下摄像头是否打开
        return -1;
    } else {
        std::cout << "打开成功" << std::endl;
    }

    Mat frame; //创建一个空图像(0*0)

    while (1) {
        cap >> frame; //读取当前帧
        // 显示一下图片，控制台窗口会在main函数结束时关闭，所以增加额外highgui函数，待用户键入数值再结束程序。
        imshow(" OpenCV CAM", frame);
        if (waitKey(20) >= 0) {
            std::cout << "打开成功" << std::endl;
        }
        //  break;  等待20ms按键，无按键输入继续循环，任意按键输入则waitKey(20)=-1 ，跳出循环
    }
    cap.release();
    return 0;
}
