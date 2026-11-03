#include "BuildSystemFactory.h"

namespace lightcpp::build {
namespace {

const SystemSpec kSpecs[] = {
    {"cmake", "CMakeLists.txt", "cmake", "cmake", true},
    {"meson", "meson.build", "meson", "meson", true},
    {"make", "Makefile", "", "make", true},
    {"ninja", "build.ninja", "", "ninja", false},
};

}  // namespace

const SystemSpec* specFor(const std::string& id) {
    for (const SystemSpec& spec : kSpecs) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

}  // namespace lightcpp::build
