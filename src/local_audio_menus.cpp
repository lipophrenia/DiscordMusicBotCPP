#include "local_audio_menus.hpp"

#include <algorithm>
#include <utility>

namespace musicbot {

void LocalAudioMenus::remember(std::uint64_t interaction_id, std::uint64_t user_id,
                              std::uint64_t guild_id, std::vector<std::string> filenames,
                              Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    std::erase_if(menus_, [now](const auto& entry) { return now - entry.second.created >= lifetime; });
    // The same interaction must never be rebound to different files.
    if (menus_.contains(interaction_id)) return;
    if (menus_.size() >= capacity) {
        const auto oldest = std::min_element(menus_.begin(), menus_.end(), [](const auto& a, const auto& b) {
            return a.second.created < b.second.created;
        });
        menus_.erase(oldest);
    }
    menus_.emplace(interaction_id, Menu{user_id, guild_id, std::move(filenames), now});
}

std::string LocalAudioMenus::select(std::uint64_t interaction_id, std::size_t index,
                                   std::uint64_t user_id, std::uint64_t guild_id,
                                   Clock::time_point now) {
    std::scoped_lock lock(mutex_);
    const auto found = menus_.find(interaction_id);
    if (found == menus_.end()) {
        throw LocalAudioError("Этот список устарел. Откройте /list-local заново.");
    }
    if (now - found->second.created >= lifetime) {
        menus_.erase(found);
        throw LocalAudioError("Этот список устарел. Откройте /list-local заново.");
    }
    const auto& menu = found->second;
    if (menu.user_id != user_id || menu.guild_id != guild_id) {
        throw LocalAudioError("Откройте свой список через /list-local.");
    }
    if (index >= menu.filenames.size()) {
        throw LocalAudioError("Запись отсутствует в этом списке. Откройте /list-local заново.");
    }
    return menu.filenames[index];
}

} // namespace musicbot
