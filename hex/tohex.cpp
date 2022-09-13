#include <iostream>
#include <fstream>
#include <ostream>
#include <iomanip>
#include <sstream>

using namespace std;


int main(int argv, char **argc) {
    ifstream in_file("D:\\Project\\C++\\Android_Native_Surface\\libs\\arm64-v8a\\aosp_8\\libSsage.so",
                     ios::in | ios::binary);
    filebuf buf;
    buf.open("out.h", ios::out);
    ostream os_file(&buf);
    in_file.seekg(0, std::ios::end);
    int len = in_file.tellg();
    in_file.seekg(0, ios::beg);

    char byte = 0;
    int total = 0;
    stringstream ss;
    int flag = 0;
    while (in_file.read(&byte, sizeof(byte))) {
        ss << "0x" << setw(2) << setfill('0') << hex << (((unsigned int) byte) & 0xFF);
        total++;
        if (len > total) {
            ss << ", ";
        }
        flag++;
        if (flag % 10 == 0) {
            ss << endl;
        }
    }
    os_file << "inline unsigned char arr[" << total << "] = { \n" << ss.str() << "\n};";
//    cout << ss.str() << endl;
    cout << "total: " << dec << total << endl;
//    cout << "len:" << dec << len << endl;
    return 0;
}