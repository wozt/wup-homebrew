// C++ on the pad: std::string, std::vector, std::sort, a class with virtual functions, a static local, new and delete
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "wup.h"
WUP_APP("C++ Test");
struct Shape { virtual ~Shape() {} virtual int area() const = 0; };
struct Rect : Shape { int w, h; Rect(int a, int b) : w(a), h(b) {} int area() const override { return w * h; } };
static int counted() { static int n = std::printf("c++: a static local, made once\n"); return n; }
int main() {
    std::string s = "Wii U";
    s += " GamePad";
    std::vector<int> v = { 5, 3, 9, 1, 7 };
    std::sort(v.begin(), v.end());
    std::vector<std::unique_ptr<Shape>> shapes;
    for (int i = 1; i <= 3; i++) shapes.emplace_back(new Rect(i, i + 1));
    int total = 0; for (auto &x : shapes) total += x->area();
    counted(); counted();
    std::string sorted; for (int x : v) sorted += std::to_string(x) + " ";
    bool ok = s == "Wii U GamePad" && sorted == "1 3 5 7 9 " && total == 2 + 6 + 12 && s.find("Pad") == 10;
    std::printf("c++: \"%s\", sorted %s, areas %d: %s\n", s.c_str(), sorted.c_str(), total, ok ? "OK" : "WRONG");
    return 0;
}
