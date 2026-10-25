// Verifies that the new media properties are part of the config schema and
// survive parsing. Usage: config_test <config.conf>

#include "config/ConfigManager.hpp"
#include "core/AnimationManager.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (!(cond)) {                                                                                                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                                                            \
            g_failures++;                                                                                                                                                          \
        }                                                                                                                                                                          \
    } while (0)

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: %s <config.conf>\n", argv[0]);
        return 2;
    }

    g_pAnimationManager = makeUnique<CHyprlockAnimationManager>();
    g_pConfigManager    = makeUnique<CConfigManager>(argv[1]);
    g_pConfigManager->init();

    const auto WIDGETS = g_pConfigManager->getWidgetConfigs();

    bool       sawBackground = false;
    bool       sawImage      = false;

    for (const auto& widget : WIDGETS) {
        if (widget.type == "background") {
            sawBackground = true;
            CHECK(std::string(std::any_cast<Hyprlang::STRING>(widget.values.at("path"))) == "/tmp/animated.gif");
            CHECK(std::any_cast<Hyprlang::INT>(widget.values.at("media_loop")) == 0);
            CHECK(std::any_cast<Hyprlang::INT>(widget.values.at("media_fps")) == 24);
            CHECK(std::any_cast<Hyprlang::FLOAT>(widget.values.at("media_speed")) == 0.5);
            CHECK(std::any_cast<Hyprlang::FLOAT>(widget.values.at("media_scale")) == 0.5);
        } else if (widget.type == "image") {
            sawImage = true;
            CHECK(std::string(std::any_cast<Hyprlang::STRING>(widget.values.at("path"))) == "/tmp/animated.mp4");
            CHECK(std::any_cast<Hyprlang::INT>(widget.values.at("media_loop")) == 1);
            CHECK(std::any_cast<Hyprlang::INT>(widget.values.at("media_fps")) == 30);
            CHECK(std::any_cast<Hyprlang::FLOAT>(widget.values.at("media_speed")) == 1.0);
        }
    }

    CHECK(sawBackground);
    CHECK(sawImage);

    if (g_failures == 0)
        std::printf("CONFIG TEST PASSED\n");

    return g_failures == 0 ? 0 : 1;
}
