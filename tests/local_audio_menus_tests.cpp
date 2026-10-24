#include "local_audio_menus.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class Action> void rejected(Action action) {
    try { action(); } catch (const musicbot::LocalAudioError&) { return; }
    throw std::runtime_error("invalid selection was accepted");
}
void test_selection() {
    musicbot::LocalAudioMenus menus;
    const auto now = musicbot::LocalAudioMenus::Clock::now();
    const std::string long_name = std::string(200, 'x') + " музыка.WEBM";
    menus.remember(100, 7, 9, {"B.webm", long_name}, now);
    // A refreshed/sorted catalog and another page must not retarget old buttons.
    menus.remember(101, 7, 9, {"A.webm", "B.webm"}, now);
    menus.remember(100, 7, 9, {"replacement.webm"}, now);
    require(menus.select(100, 0, 7, 9, now) == "B.webm", "old button changed target");
    require(menus.select(100, 1, 7, 9, now) == long_name, "long UTF-8 filename changed");
    rejected([&] { return menus.select(100, 0, 8, 9, now); });
    rejected([&] { return menus.select(100, 0, 7, 10, now); });
    rejected([&] { return menus.select(100, 2, 7, 9, now); });
    rejected([&] { return menus.select(999, 0, 7, 9, now); });
    require(menus.select(100, 0, 7, 9, now) == "B.webm", "selection consumed the menu");
    musicbot::LocalAudioMenus restarted;
    rejected([&] { return restarted.select(100, 0, 7, 9, now); });
}
void test_expiry_and_capacity() {
    musicbot::LocalAudioMenus menus;
    const auto now = musicbot::LocalAudioMenus::Clock::now();
    menus.remember(1, 7, 9, {"B.webm"}, now);
    require(menus.select(1, 0, 7, 9, now + menus.lifetime - std::chrono::seconds(1)) == "B.webm", "expired too soon");
    rejected([&] { return menus.select(1, 0, 7, 9, now + menus.lifetime); });
    for (std::size_t i = 0; i <= menus.capacity; ++i) {
        menus.remember(1000 + i, 7, 9, {"B.webm"}, now + std::chrono::milliseconds(i));
    }
    const auto later = now + std::chrono::seconds(1);
    rejected([&] { return menus.select(1000, 0, 7, 9, later); });
    require(menus.select(1001, 0, 7, 9, later) == "B.webm", "evicted more than oldest menu");
    require(menus.select(1000 + menus.capacity, 0, 7, 9, later) == "B.webm", "newest menu unavailable");
}
}
int main() {
    try {
        test_selection();
        test_expiry_and_capacity();
        std::cout << "Local audio menu tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
