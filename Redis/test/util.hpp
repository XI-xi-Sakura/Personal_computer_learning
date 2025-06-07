#pragma once

#include <vector>
#include <string>
#include <iostream>

template<typename T>
inline void printContainer(const T& container) {
    for (const auto& elem : container) {
        std::cout << elem << std::endl;
    }
}

template<typename T>
inline void printContainerPair(const T& container) {
    for (auto& elem : container) {
        // 此处预期 elem 是一个 std::pair
        std::cout << elem.first << ": " << elem.second << std::endl;
    }
}

template<typename T>
inline void printContainerOptional(const T& container) {
    for (const auto& elem : container) {
        // 此处预期 elem 是一个 optional 类型的元素, 打印之前, 先判定一下, 看是否有效
        if (elem) {
            std::cout << elem.value() << std::endl;
        } else {
            std::cout << "元素无效" << std::endl;
        }
    }
}