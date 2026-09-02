// Test automation command pipe (docs/collision-overhaul-design.md, 3.6).
// Every command runs on the game thread from OnProcess and answers one line.

#include "BallanceMMOClient.h"
#include <cfloat>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace {
    std::optional<CKDWORD> automation_key_code(std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::unordered_map<std::string, CKDWORD> names{
            {"up", CKKEY_UP}, {"down", CKKEY_DOWN}, {"left", CKKEY_LEFT}, {"right", CKKEY_RIGHT},
            {"space", CKKEY_SPACE}, {"lshift", CKKEY_LSHIFT}, {"rshift", CKKEY_RSHIFT},
            {"esc", CKKEY_ESCAPE}, {"escape", CKKEY_ESCAPE}, {"return", CKKEY_RETURN},
            {"enter", CKKEY_RETURN}, {"tab", CKKEY_TAB}, {"lctrl", CKKEY_LCONTROL},
            {"w", CKKEY_W}, {"a", CKKEY_A}, {"s", CKKEY_S}, {"d", CKKEY_D},
        };
        if (const auto it = names.find(name); it != names.end()) return it->second;
        char* end = nullptr;
        const auto value = std::strtoul(name.c_str(), &end, 0);
        if (end && *end == '\0' && value < 256) return static_cast<CKDWORD>(value);
        return std::nullopt;
    }

    void automation_dump_behavior(CKBehavior* behavior, int depth, int max_depth, std::string& out) {
        if (!behavior) return;
        const char* name = behavior->GetName();
        out += name ? name : "?";
        if (behavior->IsUsingFunction()) {
            out += '{';
            out += behavior->GetPrototypeName() ? behavior->GetPrototypeName() : "?";
            const int inputs = behavior->GetInputParameterCount();
            for (int i = 0; i < inputs && i < 8; ++i) {
                auto* input = behavior->GetInputParameter(i);
                auto* source = input ? input->GetRealSource() : nullptr;
                char value[128] = {};
                if (source) source->GetStringValue(value, FALSE);
                out += i ? "," : ":";
                out += '[';
                out += value;
                out += ']';
            }
            out += '}';
        }
        if (behavior->IsActive()) out += '*';
        const int count = behavior->GetSubBehaviorCount();
        if (count > 0 && depth < max_depth) {
            out += '(';
            for (int i = 0; i < count; ++i) {
                if (i) out += ' ';
                automation_dump_behavior(behavior->GetSubBehavior(i), depth + 1, max_depth, out);
            }
            out += ')';
        } else if (count > 0) {
            out += "(...)";
        }
    }
}

void BallanceMMOClient::start_command_pipe_from_environment() {
    const char* name = std::getenv("BMMO_COMMAND_PIPE");
    if (!name || !*name) return;
    std::string error;
    if (!command_pipe_.start(name, error))
        logger_->Warn("Automation command pipe unavailable: %s", error.c_str());
    else
        logger_->Info("Automation command pipe listening at \\\\.\\pipe\\%s", name);
}

void BallanceMMOClient::process_command_pipe() {
    if (command_pipe_.running()) {
        for (const auto& command: command_pipe_.drain()) {
            std::string response;
            try {
                response = dispatch_automation_command(command.line);
            } catch (const std::exception& e) {
                response = std::string("error ") + e.what();
            }
            logger_->Info("Automation: %s -> %s", command.line.c_str(),
                          response.substr(0, 200).c_str());
            command_pipe_.respond(command.id, response);
        }
    }
}

void BallanceMMOClient::install_input_hook() {
    std::string error;
    auto& injector = bmmo::session::input_injector::instance();
    if (!injector.install(m_bml->GetCKContext(), error)) {
        logger_->Warn("Input hook unavailable: %s", error.c_str());
        return;
    }
    injector.set_callback([this](unsigned char* state) { on_input_polled(state); });
    input_hook_installed_ = true;
}

// Game thread, immediately after the retail input manager polled DirectInput
// and before any behaviour of this frame runs.
void BallanceMMOClient::on_input_polled(unsigned char* state) {
#if defined(_MSC_VER) && defined(_M_IX86)
    if (fpu53_) _controlfp(_PC_53, _MCW_PC);
#endif
    // The manager's buffer holds KS_IDLE / KS_PRESSED / KS_RELEASED (not
    // DirectInput's 0x80): IsKeyDown tests KS_PRESSED, and a released key
    // reads KS_RELEASED for exactly one frame.
    for (const auto key: automation_released_keys_)
        if (key < 256 && !automation_held_keys_.count(key)) state[key] = KS_RELEASED;
    automation_released_keys_.clear();
    for (const auto key: automation_held_keys_)
        if (key < 256) state[key] = KS_PRESSED;
    if (replay_active_ && replay_anchored_ && replay_index_ < replay_source_.frames.size()) {
        const auto& source = replay_source_.frames[replay_index_];
        std::memcpy(state, source.keys.data(), source.keys.size());
    }
    std::memcpy(frame_keys_.data(), state, frame_keys_.size());
    frame_keys_tick_ = m_bml->GetTimeManager()->GetMainTickCount();
}

std::string BallanceMMOClient::automation_status_line() {
    std::string ball_name = "-";
    VxVector position{};
    if (auto* ball = get_current_ball()) {
        ball_name = ball->GetName() ? ball->GetName() : "?";
        ball->GetPosition(&position);
    }
    return std::format(
        "ok ingame={} paused={} playing={} map={} sector={} connected={} id={} name={} ball={} "
        "pos=({:.4f},{:.4f},{:.4f}) held_keys={}",
        m_bml->IsIngame() ? 1 : 0, m_bml->IsPaused() ? 1 : 0, m_bml->IsPlaying() ? 1 : 0,
        current_map_.name.empty() ? std::string("-") : current_map_.name,
        current_sector_.load(), connected() ? 1 : 0, db_.get_client_id(),
        get_display_nickname(), ball_name, position.x, position.y, position.z,
        automation_held_keys_.size())
        + std::format(" ballnav={} key_up={}/{} level_time={:.1f}", ball_nav_active_ ? 1 : 0,
            input_manager_ && input_manager_->IsKeyDown(CKKEY_UP) ? 1 : 0,
            input_manager_ && input_manager_->oIsKeyDown(CKKEY_UP) ? 1 : 0,
            m_bml->GetTimeManager()->GetTime() / 1000.0f);
}

std::string BallanceMMOClient::automation_dump_script(const std::string& name) {
    auto* context = m_bml->GetCKContext();
    const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
    CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
    std::string out;
    if (name.empty()) {
        int listed = 0;
        for (int i = 0; i < count; ++i) {
            auto* behavior = static_cast<CKBehavior*>(context->GetObject(ids[i]));
            if (!behavior || behavior->GetType() != CKBEHAVIORTYPE_SCRIPT || behavior->GetParent())
                continue;
            if (listed++) out += "; ";
            out += behavior->GetName() ? behavior->GetName() : "?";
            if (auto* owner = behavior->GetOwner()) {
                out += '@';
                out += owner->GetName() ? owner->GetName() : "?";
            }
            if (behavior->IsActive()) out += '*';
        }
        return "ok " + std::to_string(listed) + " scripts: " + out;
    }
    for (int i = 0; i < count; ++i) {
        auto* behavior = static_cast<CKBehavior*>(context->GetObject(ids[i]));
        if (!behavior || !behavior->GetName() || name != behavior->GetName()) continue;
        if (!out.empty()) out += " | ";
        automation_dump_behavior(behavior, 0, 6, out);
    }
    return out.empty() ? "error no behavior named " + name : "ok " + out;
}

std::string BallanceMMOClient::automation_load_level(int level) {
    // Mirror the retail Menu_Start graph: write the selected level into
    // CurrentLevel[0,0] and send "Load Level" to the Level object that owns
    // the Event_handler script.
    auto* context = m_bml->GetCKContext();
    auto* array = m_bml->GetArrayByName("CurrentLevel");
    if (!array) return "error CurrentLevel array is unavailable";
    if (!array->SetElementValue(0, 0, &level, sizeof(level)))
        return "error failed to write CurrentLevel[0,0]";
    CKBeObject* level_object = nullptr;
    const int count = context->GetObjectsCountByClassID(CKCID_BEHAVIOR);
    CK_ID* ids = context->GetObjectsListByClassID(CKCID_BEHAVIOR);
    for (int i = 0; i < count && !level_object; ++i) {
        auto* behavior = static_cast<CKBehavior*>(context->GetObject(ids[i]));
        if (behavior && behavior->GetName() && std::string_view(behavior->GetName()) == "Event_handler"
                && !behavior->GetParent())
            level_object = behavior->GetOwner();
    }
    if (!level_object) return "error Event_handler owner (Level) not found";
    auto* messages = m_bml->GetMessageManager();
    const CKMessageType type = messages->AddMessageType(const_cast<CKSTRING>("Load Level"));
    messages->SendMessageSingle(type, level_object, nullptr);
    return "ok loading level " + std::to_string(level);
}

std::string BallanceMMOClient::dispatch_automation_command(const std::string& line) {
    std::istringstream stream(line);
    std::string verb;
    stream >> verb;
    std::string rest;
    std::getline(stream, rest);
    if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);

    if (verb == "ping") return "pong";
    if (verb == "status") return automation_status_line();
    if (verb == "mmo") {
        OnFullCommand(rest);
        return "ok";
    }
    if (verb == "bml") {
        m_bml->ExecuteCommand(rest.c_str());
        return "ok";
    }
    if (verb == "quit") {
        m_bml->ExitGame();
        return "ok";
    }
    if (verb == "key") {
        std::istringstream args(rest);
        std::string key_name, action;
        args >> key_name >> action;
        const auto key = automation_key_code(key_name);
        if (!key) return "error unknown key " + key_name;
        if (action == "down") automation_held_keys_.insert(*key);
        else if (action == "up") {
            if (automation_held_keys_.erase(*key)) automation_released_keys_.insert(*key);
        }
        else return "error action must be down or up";
        return "ok";
    }
    if (verb == "keys" && rest == "clear") {
        automation_released_keys_.insert(automation_held_keys_.begin(), automation_held_keys_.end());
        automation_held_keys_.clear();
        return "ok";
    }
    if (verb == "screenshot") {
        auto* render_context = m_bml->GetRenderContext();
        if (!render_context || rest.empty()) return "error no render context or path";
        const CKERROR result = render_context->DumpToFile(
            const_cast<CKSTRING>(rest.c_str()), nullptr, VXBUFFER_BACKBUFFER);
        return result == CK_OK ? "ok " + rest : "error DumpToFile " + std::to_string(result);
    }
    if (verb == "scripts") return automation_dump_script({});
    if (verb == "script") return automation_dump_script(rest);
    if (verb == "objects") {
        auto* context = m_bml->GetCKContext();
        const int count = context->GetObjectsCountByClassID(CKCID_3DENTITY);
        CK_ID* ids = context->GetObjectsListByClassID(CKCID_3DENTITY);
        std::string out;
        int listed = 0;
        for (int i = 0; i < count && listed < 500; ++i) {
            auto* object = context->GetObject(ids[i]);
            if (!object || !object->GetName()) continue;
            if (!rest.empty()
                    && std::string_view(object->GetName()).find(rest) == std::string_view::npos)
                continue;
            if (listed++) out += "; ";
            out += object->GetName();
        }
        return "ok " + std::to_string(listed) + ": " + out;
    }
    if (verb == "message") {
        auto* messages = m_bml->GetMessageManager();
        if (!messages || rest.empty()) return "error no message manager or name";
        const CKMessageType type = messages->AddMessageType(const_cast<CKSTRING>(rest.c_str()));
        messages->SendMessageBroadcast(type, CKCID_BEOBJECT, nullptr);
        return "ok";
    }
    if (verb == "fixedtick") {
        if (rest == "on") { fixed_tick_.enable(m_bml); return "ok fixed tick enabled"; }
        if (rest == "off") { fixed_tick_.disable(m_bml); return "ok fixed tick disabled"; }
        return std::format("ok enabled={} ticks={} last_delta_ms={:.4f} skipped_renders={} waited={} "
            "input_hook={} input_polled_before_onprocess={}",
            fixed_tick_.enabled() ? 1 : 0, fixed_tick_.ticks(), fixed_tick_.last_delta_ms(),
            fixed_tick_.skipped_renders(), fixed_tick_.waited_frames(), input_hook_installed_ ? 1 : 0,
            frame_keys_tick_ == m_bml->GetTimeManager()->GetMainTickCount() ? 1 : 0);
    }
    if (verb == "physview") {
        std::string error;
        if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error))
            return "error " + error;
        bmmo::physics::world_hash hash;
        if (!physics_view_.capture(hash, error)) return "error " + error;
        return std::format(
            "ok hash={:016x} pose={:016x} cores={} ivp_time={:.6f} ivp_seed={} delta_ms={:.4f} "
            "physics_delta={:.6f} time_factor={:.6f} dll_sha256={} build_id={} main_tick={}",
            hash.hash, hash.pose, hash.cores, hash.ivp_time, hash.ivp_seed, hash.delta_time_ms,
            hash.physics_delta_time, hash.time_factor, physics_view_.dll_sha256(), physics_view_.build_id(),
            static_cast<unsigned>(m_bml->GetTimeManager()->GetMainTickCount()));
    }
    if (verb == "physobjs") {
        std::string error;
        if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error))
            return "error " + error;
        if (rest.find("all") != std::string::npos) return "ok " + physics_view_.describe_physics_objects();
        return "ok " + physics_view_.describe_movable_objects();
    }
    if (verb == "array") {
        // array <name>: every cell of a CKDataArray (diagnostics).
        std::string name = rest;
        if (!name.empty() && name.front() == ' ') name.erase(0, 1);
        CKDataArray* array = m_bml->GetArrayByName(name.c_str());
        if (!array) return "error no data array named " + name;
        const int rows = array->GetRowCount(), columns = array->GetColumnCount();
        std::string out = std::format("ok {} rows={} columns={} |", name, rows, columns);
        for (int c = 0; c < columns; ++c)
            out += std::string(" ") + (array->GetColumnName(c) ? array->GetColumnName(c) : "?")
                 + "(" + std::to_string(static_cast<int>(array->GetColumnType(c))) + ")";
        char cell[1024];
        for (int r = 0; r < rows; ++r) {
            out += " || " + std::to_string(r) + ":";
            for (int c = 0; c < columns; ++c) {
                cell[0] = 0;
                array->GetElementStringValue(r, c, cell);
                cell[sizeof(cell) - 1] = 0;
                out += std::string(c ? " | " : " ") + cell;
            }
        }
        return out;
    }
    if (verb == "exactframes") {
        std::istringstream args(rest);
        args >> record_exact_from_ >> record_exact_to_;
        return "ok exact core dumps for frames " + std::to_string(record_exact_from_) + ".."
             + std::to_string(record_exact_to_);
    }
    if (verb == "fpu53") {
        // Experiment: force 53-bit x87 precision at every frame start (Direct3D
        // leaves the game thread at 24 bits).
        fpu53_ = rest.find("on") != std::string::npos;
        return fpu53_ ? "ok x87 precision forced to 53 bits each frame" : "ok x87 precision left alone";
    }
    if (verb == "physlog") {
        std::string error;
        if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error))
            return "error " + error;
        return "ok " + physics_view_.drain_event_log();
    }
    if (verb == "physdump") {
        auto* manager = m_bml->GetCKContext()->GetManagerByGuid(CKGUID(0x6BED328B, 0x141F5148));
        if (!manager) return "error no physics manager";
        std::string out = "ok";
        const auto* base = reinterpret_cast<const unsigned char*>(manager);
        for (size_t offset = 0x20; offset < 0x100; offset += 4) {
            uint32_t raw; float f;
            std::memcpy(&raw, base + offset, 4); std::memcpy(&f, base + offset, 4);
            out += std::format(" {:03x}={:08x}/{:.5g}", offset, raw, f);
        }
        return out;
    }
    if (verb == "rng") {
        std::istringstream args(rest);
        std::string action; int seed = 1;
        args >> action >> seed;
        std::string error;
        if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error))
            return "error " + error;
        if (action != "reset") return "error usage: rng reset <seed>";
        if (!physics_view_.reset_random(seed, error)) return "error " + error;
        return "ok rng reset to " + std::to_string(seed);
    }
    if (verb == "record") {
        // record start <path-without-spaces> [level]   (level: what the
        // record is for when it starts before the level is loaded)
        std::istringstream args(rest);
        std::string action, path;
        int level = 0;
        args >> action >> path >> level;
        if (action == "start") {
            std::string error;
            if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error))
                return "error " + error;
            if (record_writer_.is_open()) record_writer_.close();
            bmmo::physics::tick_record_header header;
            header.level = level > 0 ? level : current_map_.level;
            std::snprintf(header.physics_sha256, sizeof(header.physics_sha256), "%s",
                          physics_view_.dll_sha256().c_str());
            if (!record_writer_.open(path, header)) return "error cannot open " + path;
            record_diag_.close();
            record_diag_.open(path + ".txt", std::ios::trunc);
            physics_view_.drain_event_log();  // install the listener, discard history
            record_pending_frame_ = false;
            record_anchor_pending_ = true;
            replay_anchored_ = false;
            record_frames_ = 0;
            return "ok recording to " + path;
        }
        if (action == "stop") {
            record_diag_.close();
            record_writer_.close();
            record_pending_frame_ = false;
            return "ok recorded " + std::to_string(record_frames_) + " frames";
        }
        return std::format("ok recording={} frames={}", record_writer_.is_open() ? 1 : 0, record_frames_);
    }
    if (verb == "replay") {
        // replay start <in.bmrc> <out.bmrc>
        std::istringstream args(rest);
        std::string action, in_path, out_path;
        args >> action >> in_path >> out_path;
        if (action == "stop") {
            replay_active_ = false;
            record_writer_.close();
            record_pending_frame_ = false;
            return "ok replay stopped after " + std::to_string(replay_index_) + " frames";
        }
        if (action != "start" || in_path.empty() || out_path.empty())
            return "error usage: replay start <in.bmrc> <out.bmrc> | replay stop";
        std::string error;
        if (!replay_source_.load(in_path, error)) return "error " + error;
        if (!physics_view_.available() && !physics_view_.initialize(m_bml->GetCKContext(), error))
            return "error " + error;
        if (record_writer_.is_open()) record_writer_.close();
        bmmo::physics::tick_record_header header = replay_source_.header;
        std::snprintf(header.physics_sha256, sizeof(header.physics_sha256), "%s",
                      physics_view_.dll_sha256().c_str());
        if (!record_writer_.open(out_path, header)) return "error cannot open " + out_path;
        record_pending_frame_ = false;
        record_anchor_pending_ = true;
            replay_anchored_ = false;
        record_frames_ = 0;
        replay_index_ = 0;
        replay_active_ = true;
        return "ok replaying " + std::to_string(replay_source_.frames.size()) + " frames into " + out_path;
    }
    if (verb == "level") {
        std::istringstream args(rest);
        std::string mode;
        int level = 0;
        args >> level >> mode;
        if (level < 1 || level > 13) return "error usage: level <1..13> [direct]";
        if (mode == "direct") return automation_load_level(level);
        level_request_.begin(level);
        return "ok level " + std::to_string(level) + " requested through the menus";
    }
    return "error unknown command " + verb;
}

// Advances a menu-driven level request (game/menu_driver.hpp) one frame.
void BallanceMMOClient::process_level_request() {
    if (!level_request_.pending()) return;
    using status = bmmo::game::level_request::status;
    const bool was_pressed = level_request_.start_pressed;
    switch (level_request_.step(m_bml->GetCKContext())) {
    case status::start_pressed:
        if (!was_pressed) GetLogger()->Info("Automation: pressed Start in Menu_Main");
        break;
    case status::level_selected:
        GetLogger()->Info("Automation: selected level %d in Menu_Start", level_request_.level);
        break;
    case status::failed:
        GetLogger()->Warn("Automation: level request failed: %s", level_request_.error.c_str());
        break;
    default:
        break;
    }
}

bool BallanceMMOClient::gameplay_ingame_script_active() {
    auto* script = m_bml->GetScriptByName("Gameplay_Ingame");
    return script && script->IsActive();
}

// Frame k of a record pairs the keyboard state the game observed during
// tick A+1+k (polled at that tick's PreProcess by the input manager) with
// the physics hash after that tick's step.  BMLPlus runs the mods' OnProcess
// from its PostProcess, after the physics step, so both are at hand here.
// Tick A is the anchor: the first tick with Gameplay_Ingame active, at whose
// OnProcess the session clock is reset (design 3.1); the headless replay
// resets after the same tick and feeds frame k to tick A+1+k.
void BallanceMMOClient::process_tick_record() {
    if (!record_writer_.is_open()) return;
    if (!gameplay_ingame_script_active() || !input_manager_) return;
    std::string error;
    const auto diagnose = [&](const char* label) {
        if (!record_diag_.is_open()) return;
        record_diag_ << label << " bodies: " << physics_view_.describe_physics_objects() << "\n"
                     << label << " events: " << physics_view_.drain_event_log() << "\n";
        record_diag_.flush();
    };
    if (record_anchor_pending_) {
        record_anchor_pending_ = false;
        if (!physics_view_.reset_session_clock(1, error))
            logger_->Warn("Record anchor reset failed: %s", error.c_str());
        replay_anchored_ = true;
        diagnose("anchor");
        return;
    }
    if (replay_active_ && replay_index_ >= replay_source_.frames.size()) {
        replay_active_ = false;
        record_writer_.close();
        record_diag_.close();
        logger_->Info("Replay finished after %zu frames", replay_index_);
        return;
    }
    record_frame_ = {};
    if (input_hook_installed_)
        record_frame_.keys = frame_keys_;
    else if (const auto* keys = input_manager_->GetKeyboardState())
        std::memcpy(record_frame_.keys.data(), keys, record_frame_.keys.size());
    record_frame_.flags = m_bml->IsPaused() ? 1u : 0u;
    bmmo::physics::world_hash hash;
    if (physics_view_.capture(hash, error)) {
        record_frame_.hash = hash.hash;
        record_frame_.ivp_time = hash.ivp_time;
        record_frame_.cores = hash.cores;
        record_frame_.surfaces = hash.surfaces;
        record_frame_.pose = hash.pose;
        std::snprintf(record_frame_.probe_name, sizeof(record_frame_.probe_name), "%s", hash.probe_name);
        for (int k = 0; k < 3; ++k) {
            record_frame_.probe_position[k] = hash.probe_position[k];
            record_frame_.probe_speed[k] = hash.probe_speed[k];
            record_frame_.probe_rot_speed[k] = hash.probe_rot_speed[k];
        }
    }
    record_writer_.write(record_frame_);
    if (record_diag_.is_open() && record_frames_ >= record_exact_from_ && record_frames_ <= record_exact_to_
            && record_exact_to_ > 0) {
        record_diag_ << "exact frame" << record_frames_ << " ivp_time=" << hash.ivp_time << " seed=" << hash.ivp_seed
                     << "\n" << physics_view_.describe_cores_exact();
        record_diag_.flush();
    }
    if (record_frames_ == 6 || record_frames_ == 66 || record_frames_ == 660 || record_frames_ == 1716) {
        char label[32];
        std::snprintf(label, sizeof(label), "frame%llu", static_cast<unsigned long long>(record_frames_));
        diagnose(label);
    }
    ++record_frames_;
    if (replay_active_) ++replay_index_;
}
