cmake --build cmake-build-debug --target %1 -- -j 16
adb push outputs\x86_64\%1 /data/local/tmp
adb shell su -c chmod +x /data/local/tmp/%1
adb shell su -c /data/local/tmp/%1