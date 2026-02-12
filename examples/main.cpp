#include <iostream>

#include "sinnet/eventloop/EventLoop.hpp"

int main() {
    sinnet::EventLoop eventLoop;
    std::cout << "Event loop started" << '\n';
    return 0;
}
