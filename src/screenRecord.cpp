//
// Created by user on 2022/9/29.
//
#include "extern_function.h"

/**
 * 录屏回调
 * @param buff 缓冲区
 * @param size 缓冲区大小
 */
void callback(uint8_t *buff, size_t size) {
    printf("buffer Size: %zu\n", size);
    // TODO
}

bool flag = true;


/**
 * h264录屏测试
 */
int main(int argc, char *argv[]) {
    ExternFunction externFunction;
    externFunction.initRecord();
    externFunction.runRecord(&flag, callback);
    externFunction.stopRecord();
}