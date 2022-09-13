//
// Created by Ssage on 2022/3/18.
//
#include "main.h"

int main(int argc, char *argv[]) {
    screen_config();
    cout << "height:" << displayInfo.height << " width:" << displayInfo.width << " orientation:"
         << displayInfo.orientation << endl;

    if (!init_egl((int) displayInfo.width, (int) displayInfo.height, true)) {
        exit(0);
    }
    ImGui_init();
    Init_touch_config();
    printf("Pid is %d\n", getpid());
    for (;;) {
        tick();
    }
    shutdown();
    return 0;
}