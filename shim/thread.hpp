#pragma once

#include <thread>

namespace wtc {

[[clang::annotate("wtc_thread_spawn")]]
inline std::thread spawn(void (*start_routine)(void *), void* arg) {
    return std::thread(start_routine, arg); 
}

[[clang::annotate("wtc_thread_spawn")]]
inline std::thread spawn(void (*start_routine)()) {
    return std::thread(start_routine); 
}

}