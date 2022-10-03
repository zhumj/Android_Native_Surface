cmake --build cmake-build-debug --target %1 -- -j 16
adb -s 192.168.31.108:39097 push outputs\arm64-v8a\%1 /data/local/tmp
adb -s 192.168.31.108:39097 shell su -c chmod +x /data/local/tmp/%1
@REM adb shell su -c /data/local/tmp/%1