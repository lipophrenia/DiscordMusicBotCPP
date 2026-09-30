#include "musicbot/bot.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
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

std::string safe_title(std::string title) {
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');
    if (title.size() > 100) {
        title.resize(97);
        title += "…";
    }
    return title;
}

std::string track_line(std::size_t index, const Track& track) {
    std::ostringstream output;
    output << '`' << index << ".` [" << safe_title(track.title) << "](" << track.webpage_url
           << ") · `" << format_duration(track) << '`';
    return output.str();
}

}  // namespace

MusicBot::MusicBot(Config config)
    : config_(std::move(config)),
      bot_(config_.token, dpp::i_default_intents | dpp::i_guild_voice_states),
      extractor_(config_),
      players_(bot_, config_, extractor_) {
    bot_.on_log([](const dpp::log_t& event) {
        if (event.severity >= dpp::ll_info) {
            std::clog << event.message << '\n';
        }
    });
    bot_.on_ready([this](const dpp::ready_t&) {
        if (dpp::run_once<struct register_music_commands>()) {
            register_commands();
            bot_.set_presence(dpp::presence(dpp::ps_online, dpp::at_game, "/play"));
            std::clog << "Discord Music Bot started as " << bot_.me.username << '\n';
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
            reply_error(event, "Произошла внутренняя ошибка.");
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
    commands.emplace_back("join", "Подключить бота к вашему голосовому каналу", bot_.me.id);
    commands.emplace_back("play", "Добавить YouTube-видео в очередь", bot_.me.id)
        .add_option(dpp::command_option(
            dpp::co_string, "query", "Ссылка YouTube или поисковый запрос", true));
    commands.emplace_back("pause", "Поставить воспроизведение на паузу", bot_.me.id);
    commands.emplace_back("resume", "Продолжить воспроизведение", bot_.me.id);
    commands.emplace_back("skip", "Пропустить текущий трек", bot_.me.id);
    commands.emplace_back("stop", "Остановить музыку и очистить очередь", bot_.me.id);
    commands.emplace_back("leave", "Остановить музыку и отключить бота", bot_.me.id);
    commands.emplace_back("queue", "Показать очередь", bot_.me.id);
    commands.emplace_back("now", "Показать текущий трек", bot_.me.id);
    commands.emplace_back("volume", "Изменить громкость", bot_.me.id)
        .add_option(dpp::command_option(
            dpp::co_integer, "percent", "Громкость от 0 до 200 процентов", true)
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
        throw UserError("Эта команда работает только на сервере.");
    }

    if (name == "join") {
        ensure_voice(event);
        event.reply(ephemeral_message("✅ Подключаюсь к вашему голосовому каналу."));
        return;
    }
    if (name == "play") {
        if (command.options.empty() ||
            !std::holds_alternative<std::string>(command.options.front().value)) {
            throw UserError("Укажите ссылку YouTube или поисковый запрос.");
        }
        handle_play(event, std::get<std::string>(command.options.front().value));
        return;
    }
    if (name == "queue") {
        const auto state = player(event).snapshot(15);
        if (!state.current && state.queued.empty()) {
            event.reply(ephemeral_message("Очередь пуста."));
            return;
        }
        std::ostringstream text;
        if (state.current) {
            text << "▶️ Сейчас: [" << safe_title(state.current->title) << "](" <<
                state.current->webpage_url << ") · `" << format_duration(*state.current)
                 << "`\n";
        }
        const auto shown = state.queued.size();
        for (std::size_t index = 0; index < shown; ++index) {
            text << track_line(index + 1, state.queued[index]) << '\n';
        }
        if (state.total_queued > shown) {
            text << "…и ещё " << state.total_queued - shown;
        }
        event.reply(safe_message(text.str()));
        return;
    }
    if (name == "now") {
        const auto current = player(event).current_track();
        if (!current) {
            event.reply(ephemeral_message("Сейчас ничего не играет."));
            return;
        }
        event.reply(safe_message("▶️ Сейчас играет: [" + safe_title(current->title) + "](" +
                                 current->webpage_url + ") · `" +
                                 format_duration(*current) + "`, добавил " +
                                 current->requester_name));
        return;
    }

    require_same_voice(event);
    auto& guild_player = player(event);
    if (name == "pause") {
        if (guild_player.paused()) {
            throw UserError("Воспроизведение уже приостановлено.");
        }
        guild_player.set_paused(true);
        event.reply(safe_message("⏸️ Воспроизведение приостановлено."));
    } else if (name == "resume") {
        if (!guild_player.paused()) {
            throw UserError("Воспроизведение не стоит на паузе.");
        }
        guild_player.set_paused(false);
        event.reply(safe_message("▶️ Воспроизведение продолжено."));
    } else if (name == "skip") {
        if (!guild_player.skip()) {
            throw UserError("Сейчас ничего не воспроизводится.");
        }
        event.reply(safe_message("⏭️ Трек пропущен."));
    } else if (name == "stop") {
        const auto removed = guild_player.stop();
        event.reply(safe_message("⏹️ Воспроизведение остановлено. Удалено из очереди: " +
                                 std::to_string(removed) + "."));
    } else if (name == "leave") {
        players_.remove(guild_id);
        event.from()->disconnect_voice(event.command.guild_id);
        event.reply(ephemeral_message("Отключился от голосового канала."));
    } else if (name == "volume") {
        if (command.options.empty() ||
            !std::holds_alternative<std::int64_t>(command.options.front().value)) {
            throw UserError("Укажите громкость от 0 до 200.");
        }
        const auto percent = std::get<std::int64_t>(command.options.front().value);
        if (percent < 0 || percent > 200) {
            throw UserError("Громкость должна быть от 0 до 200.");
        }
        guild_player.set_volume(static_cast<int>(percent));
        event.reply(safe_message("🔊 Громкость: **" + std::to_string(percent) + "%**."));
    } else {
        throw UserError("Неизвестная команда.");
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
                auto track = extractor_.create_track(
                    query, snowflake_value(user.id), user.username,
                    snowflake_value(event.command.channel_id), stop_token);
                if (stop_token.stop_requested()) {
                    return;
                }
                ensure_voice(event);
                auto response = "✅ Добавлено в очередь: [" + safe_title(track.title) + "](" +
                                track.webpage_url + ") · `" + format_duration(track) +
                                "` · позиция **";
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

void MusicBot::ensure_voice(const dpp::slashcommand_t& event) {
    const auto wanted_channel = member_voice_channel(event);
    auto* shard = event.from();
    auto* current = shard->get_voice(event.command.guild_id);
    if (current != nullptr && snowflake_value(current->channel_id) == wanted_channel) {
        return;
    }
    auto* guild = dpp::find_guild(event.command.guild_id);
    if (guild == nullptr) {
        throw UserError("Сервер отсутствует в кэше Discord.");
    }
    if (current != nullptr) {
        if (player(event).busy()) {
            throw UserError("Бот уже занят в другом голосовом канале.");
        }
        shard->disconnect_voice(event.command.guild_id);
    }
    if (!guild->connect_member_voice(bot_, event.command.get_issuing_user().id, false, true)) {
        throw UserError("Не удалось подключиться к голосовому каналу.");
    }
}

dpp::discord_voice_client* MusicBot::require_same_voice(const dpp::slashcommand_t& event) {
    const auto wanted_channel = member_voice_channel(event);
    auto* connection = event.from()->get_voice(event.command.guild_id);
    if (connection == nullptr || connection->voiceclient == nullptr) {
        throw UserError("Бот сейчас не подключён к голосовому каналу.");
    }
    if (snowflake_value(connection->channel_id) != wanted_channel) {
        throw UserError("Войдите в тот же голосовой канал, что и бот.");
    }
    return connection->voiceclient.get();
}

std::uint64_t MusicBot::member_voice_channel(const dpp::slashcommand_t& event) const {
    auto* guild = dpp::find_guild(event.command.guild_id);
    if (guild == nullptr) {
        throw UserError("Сервер отсутствует в кэше Discord.");
    }
    const auto user_id = event.command.get_issuing_user().id;
    const auto member = guild->voice_members.find(user_id);
    if (member == guild->voice_members.end() || member->second.channel_id.empty()) {
        throw UserError("Сначала войдите в голосовой канал.");
    }
    return snowflake_value(member->second.channel_id);
}

GuildPlayer& MusicBot::player(const dpp::slashcommand_t& event) {
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

void MusicBot::reply_error(const dpp::slashcommand_t& event, const std::string& message) {
    event.reply(ephemeral_message("❌ " + message));
}

}  // namespace musicbot
