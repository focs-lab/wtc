// The body of supercollider's testsuite/server/supernova/sndfile_backend_test.cpp
// (at b2f6e4422088) without Boost.Test: the backend under the engine
// functor that test declares, opened, activated and deactivated. Built by
// supercollider.sh.
#include <chrono>
#include <thread>

#include "audio_backend/sndfile_backend.hpp"

using namespace nova;

namespace {
aligned_storage_ptr<float> data(nova::calloc_aligned<float>(64));

struct engine_functor;
struct engine_functor {
    void init_tick() {}
    void run_tick();
};

sndfile_backend<engine_functor> be;

void engine_functor::run_tick() {
    float *data_ptr = data.get();
    be.output_mapping(&data_ptr, &data_ptr + 1);
}
} // namespace

int main() {
    be.open_client("", "./output.wav", 44100, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, 64);
    be.activate_audio();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    be.deactivate_audio();
    be.close_client();
}
