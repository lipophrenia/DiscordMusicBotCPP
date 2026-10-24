#include "bot.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace musicbot {
namespace {

std::uint64_t snowflake_value(dpp::snowflake value) {
    return static_cast<std::uint64_t>(value);
}

dpp::message safe_message(std::string text) {
    dpp::message message(std::move(text));
    message.set_allowed_mentions(false, false, false, false);
    return message;
}

dpp::message ephemeral_message(std::string text) {
    auto message = safe_message(std::move(text));
    message.set_flags(dpp::m_ephemeral);
    return message;
}

std::string safe_title(std::string title, std::size_t maximum_bytes = 100) {
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');
    if (title.size() > maximum_bytes) {
        std::size_t end = maximum_bytes - 3;
        while (end > 0 && (static_cast<unsigned char>(title[end]) & 0xC0) == 0x80) --end;
        title.resize(end);
        title += "…";
    }
    return title;
}

std::string escape_markdown(const std::string& value) {
    std::string result;
    for (const auto c : value) {
        if (std::string_view("\\`*_{}[]()<>#+-.!|~").find(c) != std::string_view::npos) {
            result.push_back('\\');
        }
        result.push_back(c);
    }
    return result;
}

std::string track_label(const Track& track) {
    const auto title = escape_markdown(safe_title(track.title));
    if (track.local_filename) return "📁 " + title;
    return "[" + title + "](" + track.webpage_url + ")";
}

std::string track_line(std::size_t index, const Track& track) {
    std::ostringstream output;
    output << '`' << index << ".` " << track_label(track)
           << " · `" << format_duration(track) << '`';
    return output.str();
}

std::uint64_t local_button_number(std::string_view text) {
    std::uint64_t number{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw UserError("Invalid button. Run /list-local again.");
    }
    return number;
}

dpp::message local_audio_menu(const LocalAudioPage& listing, std::size_t page,
                              std::uint64_t interaction_id) {
    auto response = safe_message("");
    for (std::size_t index = 0; index < listing.filenames.size(); ++index) {
        const auto stem = std::filesystem::u8path(listing.filenames[index]).stem().u8string();
        const std::string title(stem.begin(), stem.end());
        response.add_component(dpp::component().add_component(dpp::component()
            .set_label(safe_title(title, 80))
            .set_style(dpp::cos_primary)
            .set_id("local:play:" + std::to_string(interaction_id) + ":" + std::to_string(index))));
    }
    response.add_embed(dpp::embed().set_title("Local audio")
        .set_footer(dpp::embed_footer().set_text("Page " + std::to_string(page) +
            " / " + std::to_string(listing.total_pages) + " · tracks: " +
            std::to_string(listing.total_files))));
    if (listing.total_pages > 1) {
        response.add_component(dpp::component()
            .add_component(dpp::component().set_label("Previous").set_style(dpp::cos_secondary)
                .set_id("local:page:" + std::to_string(page - 1)).set_disabled(page == 1))
            .add_component(dpp::component().set_label("Next").set_style(dpp::cos_secondary)
                .set_id("local:page:" + std::to_string(page + 1)).set_disabled(page == listing.total_pages)));
    }
    return response;
}

}  // namespace

MusicBot::MusicBot(Config config)
    : config_(std::move(config)),
      bot_(config_.token, dpp::i_default_intents | dpp::i_guild_voice_states),
      extractor_(config_),
      players_(bot_, config_, extractor_),
      local_audio_(config_.local_audio_dir) {
    bot_.on_log([](const dpp::log_t& event) {
        if (event.severity >= dpp::ll_info) {
            std::clog << event.message << '\n';
        }
    });
    bot_.on_voice_ready([this](const dpp::voice_ready_t& event) {
        if (!shutting_down_.load(std::memory_order_acquire)) {
            players_.mark_voice_ready(event.voice_client);
        }
    });
    bot_.on_ready([this](const dpp::ready_t&) {
        if (dpp::run_once<struct register_music_commands>()) {
            register_commands();
            bot_.set_presence(dpp::presence(dpp::ps_online, dpp::at_game, "/play"));
            std::clog << "Discord Music Bot started as " << bot_.me.username << '\n';
        }
    });
    bot_.on_button_click([this](const dpp::button_click_t& event) {
        if (!event.custom_id.starts_with("local:")) return;
        try {
            handle_local_button(event);
        } catch (const UserError& error) {
            reply_error(event, error.what());
        } catch (const LocalAudioError& error) {
            reply_error(event, error.what());
        } catch (const std::exception& error) {
            std::cerr << "Local audio button error: " << error.what() << '\n';
            reply_error(event, "Could not process the selected track.");
        }
    });
    bot_.on_slashcommand([this](const dpp::slashcommand_t& event) {
        try {
            const auto user = event.command.get_issuing_user();
            std::clog << "Command received: /" << event.command.get_command_name()
                      << " user=" << user.username
                      << " guild=" << snowflake_value(event.command.guild_id) << '\n';
            handle_command(event);
        } catch (const UserError& error) {
            reply_error(event, error.what());
        } catch (const std::exception& error) {
            std::cerr << "Slash command error: " << error.what() << '\n';
            reply_error(event, "An internal error occurred.");
        }
    });
}

MusicBot::~MusicBot() {
    shutting_down_.store(true, std::memory_order_release);
    std::vector<AsyncTask> tasks;
    {
        std::scoped_lock lock(tasks_mutex_);
        tasks.swap(tasks_);
    }
    for (auto& task : tasks) {
        task.thread.request_stop();
    }
    for (auto& task : tasks) {
        if (task.thread.joinable()) {
            task.thread.join();
        }
    }
    players_.shutdown();
}

void MusicBot::run() {
    bot_.start(dpp::st_wait);
}

void MusicBot::register_commands() {
    std::vector<dpp::slashcommand> commands;
    commands.emplace_back("join", "Join your voice channel", bot_.me.id);
    commands.emplace_back("play", "Add a YouTube video to the queue", bot_.me.id)
        .add_option(dpp::command_option(
            dpp::co_string, "query", "YouTube URL or search query", true));
    commands.emplace_back("list-local", "Browse local audio tracks", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_integer, "page", "Page number", false)
                        .set_min_value(1));
    commands.emplace_back("play-local", "Add a local audio track to the queue", bot_.me.id)
        .add_option(dpp::command_option(dpp::co_string, "file",
            "Full local filename, including .webm", true));
    commands.emplace_back("pause", "Pause playback", bot_.me.id);
    commands.emplace_back("resume", "Resume playback", bot_.me.id);
    commands.emplace_back("skip", "Skip the current track", bot_.me.id);
    commands.emplace_back("stop", "Stop playback and clear the queue", bot_.me.id);
    commands.emplace_back("leave", "Stop playback and leave the voice channel", bot_.me.id);
    commands.emplace_back("queue", "Show the queue", bot_.me.id);
    commands.emplace_back("now", "Show the current track", bot_.me.id);
    commands.emplace_back("volume", "Set the volume", bot_.me.id)
        .add_option(dpp::command_option(
            dpp::co_integer, "percent", "Volume from 0 to 200 percent", true)
                        .set_min_value(0)
                        .set_max_value(200));
    if (config_.guild_id) {
        bot_.guild_bulk_command_create(commands, dpp::snowflake(*config_.guild_id));
        std::clog << "Registering commands for guild " << *config_.guild_id << '\n';
    } else {
        bot_.global_bulk_command_create(commands);
        std::clog << "Registering global commands\n";
    }
}

void MusicBot::handle_command(const dpp::slashcommand_t& event) {
    const auto command = event.command.get_command_interaction();
    const auto& name = command.name;
    const auto guild_id = snowflake_value(event.command.guild_id);
    if (guild_id == 0) {
        throw UserError("This command can only be used in a server.");
    }

    if (name == "join") {
        ensure_voice(event);
        event.reply(ephemeral_message("✅ Joining your voice channel."));
        return;
    }
    if (name == "play") {
        if (command.options.empty() ||
            !std::holds_alternative<std::string>(command.options.front().value)) {
            throw UserError("Enter a YouTube URL or search query.");
        }
        handle_play(event, std::get<std::string>(command.options.front().value));
        return;
    }
    if (name == "list-local") {
        std::int64_t page = 1;
        if (!command.options.empty()) {
            if (!std::holds_alternative<std::int64_t>(command.options.front().value)) {
                throw UserError("The page number must be an integer.");
            }
            page = std::get<std::int64_t>(command.options.front().value);
        }
        if (page < 1 || page > 1000000) throw UserError("Invalid page number.");
        handle_list_local(event, static_cast<std::size_t>(page));
        return;
    }
    if (name == "play-local") {
        if (command.options.empty() ||
            !std::holds_alternative<std::string>(command.options.front().value)) {
            throw UserError("Enter the local filename, including .webm.");
        }
        handle_play_local(event, std::get<std::string>(command.options.front().value));
        return;
    }
    if (name == "queue") {
        const auto state = player(event).snapshot(15);
        if (!state.current && state.queued.empty()) {
            event.reply(ephemeral_message("The queue is empty."));
            return;
        }
        std::ostringstream text;
        if (state.current) {
            text << "▶️ Now playing: " << track_label(*state.current)
                 << " · `" << format_duration(*state.current)
                 << "`\n";
        }
        std::size_t shown{};
        for (const auto& track : state.queued) {
            const auto line = track_line(shown + 1, track);
            if (text.tellp() + static_cast<std::streamoff>(line.size()) > 3500) break;
            text << line << '\n';
            ++shown;
        }
        if (state.total_queued > shown) {
            text << "…and more: " << state.total_queued - shown;
        }
        auto response = safe_message("");
        response.add_embed(dpp::embed().set_title("Queue").set_description(text.str()));
        event.reply(response);
        return;
    }
    if (name == "now") {
        const auto current = player(event).current_track();
        if (!current) {
            event.reply(ephemeral_message("Nothing is playing."));
            return;
        }
        event.reply(safe_message("▶️ Now playing: " + track_label(*current) + " · `" +
                                 format_duration(*current) + "`, requested by " +
                                 current->requester_name));
        return;
    }

    require_same_voice(event);
    auto& guild_player = player(event);
    if (name == "pause") {
        if (guild_player.paused()) {
            throw UserError("Playback is already paused.");
        }
        guild_player.set_paused(true);
        event.reply(safe_message("⏸️ Playback paused."));
    } else if (name == "resume") {
        if (!guild_player.paused()) {
            throw UserError("Playback is not paused.");
        }
        guild_player.set_paused(false);
        event.reply(safe_message("▶️ Playback resumed."));
    } else if (name == "skip") {
        if (!guild_player.skip()) {
            throw UserError("Nothing is playing.");
        }
        event.reply(safe_message("⏭️ Track skipped."));
    } else if (name == "stop") {
        const auto removed = guild_player.stop();
        event.reply(safe_message("⏹️ Playback stopped. Tracks removed from the queue: " +
                                 std::to_string(removed) + "."));
    } else if (name == "leave") {
        players_.remove(guild_id);
        event.from()->disconnect_voice(event.command.guild_id);
        event.reply(ephemeral_message("Left the voice channel."));
    } else if (name == "volume") {
        if (command.options.empty() ||
            !std::holds_alternative<std::int64_t>(command.options.front().value)) {
            throw UserError("Enter a volume from 0 to 200.");
        }
        const auto percent = std::get<std::int64_t>(command.options.front().value);
        if (percent < 0 || percent > 200) {
            throw UserError("Volume must be between 0 and 200.");
        }
        guild_player.set_volume(static_cast<int>(percent));
        event.reply(safe_message("🔊 Volume: **" + std::to_string(percent) + "%**."));
    } else {
        throw UserError("Unknown command.");
    }
}

void MusicBot::handle_play(
    const dpp::slashcommand_t& event,
    const std::string& query) {
    member_voice_channel(event);
    event.thinking(false, [this, event, query](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
            return;
        }
        launch_task([this, event, query](std::stop_token stop_token) {
            try {
                const auto user = event.command.get_issuing_user();
                auto track = extractor_.create_track(query, snowflake_value(user.id), user.username,
                                                      snowflake_value(event.command.channel_id), stop_token);
                if (stop_token.stop_requested()) {
                    return;
                }
                ensure_voice(event);
                auto response = "✅ Added to the queue: " + track_label(track) +
                                " · `" + format_duration(track) +
                                "` · position **";
                const auto position = player(event).enqueue(std::move(track), event.from());
                event.edit_original_response(
                    safe_message(response + std::to_string(position) + "**"));
            } catch (const std::exception& error) {
                if (!stop_token.stop_requested()) {
                    event.edit_original_response(
                        safe_message("❌ " + std::string(error.what())));
                }
            }
        });
    });
}

void MusicBot::handle_play_local(const dpp::interaction_create_t& event, const std::string& filename) {
    member_voice_channel(event);
    const auto user = event.command.get_issuing_user();
    Track track;
    try {
        // Only validate the named file; no catalog scan, media scan or extractor.
        track = local_audio_.create_track(filename, snowflake_value(user.id), user.username,
                                          snowflake_value(event.command.channel_id));
    } catch (const LocalAudioError& error) {
        throw UserError(error.what());
    }
    ensure_voice(event);
    auto response = "✅ Added to the queue: " + track_label(track) + " · position **";
    const auto position = player(event).enqueue(std::move(track), event.from());
    // Playback must not wait for a Discord REST callback. One final response is
    // sufficient for this fast local operation; no deferred 'thinking' round-trip.
    event.reply(safe_message(response + std::to_string(position) + "**"));
}

void MusicBot::handle_local_button(const dpp::button_click_t& event) {
    const auto guild_id = snowflake_value(event.command.guild_id);
    if (guild_id == 0) throw UserError("This command can only be used in a server.");
    const std::string_view id(event.custom_id);
    if (id.starts_with("local:page:")) {
        const auto page = local_button_number(id.substr(11));
        if (page == 0 || page > 1000000) throw UserError("Invalid page number.");
        handle_list_local(event, static_cast<std::size_t>(page), true);
        return;
    }
    if (!id.starts_with("local:play:")) throw UserError("Unknown button.");
    const auto selection = id.substr(11);
    const auto separator = selection.find(':');
    if (separator == std::string_view::npos) throw UserError("Invalid button.");
    const auto menu_id = local_button_number(selection.substr(0, separator));
    const auto index = local_button_number(selection.substr(separator + 1));
    if (index >= LocalAudioLibrary::page_size) throw UserError("Invalid track number.");
    const auto filename = local_menus_.select(menu_id, static_cast<std::size_t>(index),
        snowflake_value(event.command.get_issuing_user().id), guild_id);
    handle_play_local(event, filename);
}

void MusicBot::handle_list_local(const dpp::interaction_create_t& event, std::size_t page, bool update) {
    const auto acknowledged = [this, event, page](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) return;
        launch_task([this, event, page](std::stop_token stop_token) {
            try {
                if (stop_token.stop_requested()) return;
                const auto listing = local_audio_.list(page);
                if (stop_token.stop_requested()) return;
                if (listing.total_files == 0) {
                    event.edit_original_response(safe_message("No local audio tracks are available yet."));
                    return;
                }
                const auto menu_id = snowflake_value(event.command.id);
                local_menus_.remember(menu_id, snowflake_value(event.command.get_issuing_user().id),
                                      snowflake_value(event.command.guild_id), listing.filenames);
                event.edit_original_response(local_audio_menu(listing, page, menu_id));
            } catch (const std::exception& error) {
                if (!stop_token.stop_requested()) {
                    event.edit_original_response(safe_message("❌ " + std::string(error.what())));
                }
            }
        });
    };
    if (update) {
        event.reply(dpp::ir_deferred_update_message, safe_message(""), acknowledged);
    } else {
        event.thinking(true, acknowledged);
    }
}

void MusicBot::ensure_voice(const dpp::interaction_create_t& event) {
    const auto wanted_channel = member_voice_channel(event);
    auto* shard = event.from();
    auto* current = shard->get_voice(event.command.guild_id);
    if (current != nullptr && snowflake_value(current->channel_id) == wanted_channel) {
        return;
    }
    auto* guild = dpp::find_guild(event.command.guild_id);
    if (guild == nullptr) {
        throw UserError("The server is not in the Discord cache.");
    }
    if (current != nullptr) {
        if (player(event).busy()) {
            throw UserError("The bot is already playing in another voice channel.");
        }
        shard->disconnect_voice(event.command.guild_id);
    }
    // Create the player before connecting, including for /join, so the ready
    // event cannot be lost before the first /play-local or /play command.
    player(event).reset_voice_ready();
    if (!guild->connect_member_voice(bot_, event.command.get_issuing_user().id, false, true)) {
        throw UserError("Could not join the voice channel.");
    }
}

dpp::discord_voice_client* MusicBot::require_same_voice(const dpp::slashcommand_t& event) {
    const auto wanted_channel = member_voice_channel(event);
    auto* connection = event.from()->get_voice(event.command.guild_id);
    if (connection == nullptr || connection->voiceclient == nullptr) {
        throw UserError("The bot is not connected to a voice channel.");
    }
    if (snowflake_value(connection->channel_id) != wanted_channel) {
        throw UserError("Join the same voice channel as the bot.");
    }
    return connection->voiceclient.get();
}

std::uint64_t MusicBot::member_voice_channel(const dpp::interaction_create_t& event) const {
    auto* guild = dpp::find_guild(event.command.guild_id);
    if (guild == nullptr) {
        throw UserError("The server is not in the Discord cache.");
    }
    const auto user_id = event.command.get_issuing_user().id;
    const auto member = guild->voice_members.find(user_id);
    if (member == guild->voice_members.end() || member->second.channel_id.empty()) {
        throw UserError("Join a voice channel first.");
    }
    return snowflake_value(member->second.channel_id);
}

GuildPlayer& MusicBot::player(const dpp::interaction_create_t& event) {
    return players_.get(snowflake_value(event.command.guild_id), event.from());
}

void MusicBot::launch_task(std::function<void(std::stop_token)> task) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock(tasks_mutex_);
    for (auto iterator = tasks_.begin(); iterator != tasks_.end();) {
        if (iterator->done->load(std::memory_order_acquire)) {
            if (iterator->thread.joinable()) {
                iterator->thread.join();
            }
            iterator = tasks_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (shutting_down_.load(std::memory_order_relaxed)) {
        return;
    }
    auto done = std::make_shared<std::atomic_bool>(false);
    std::jthread thread([task = std::move(task), done](std::stop_token stop_token) {
        try {
            task(stop_token);
        } catch (const std::exception& error) {
            std::cerr << "Background task error: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Unknown background task error\n";
        }
        done->store(true, std::memory_order_release);
    });
    tasks_.push_back(AsyncTask{std::move(done), std::move(thread)});
}

void MusicBot::reply_error(const dpp::interaction_create_t& event, const std::string& message) {
    event.reply(ephemeral_message("❌ " + message));
}

}  // namespace musicbot
