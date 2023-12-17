#include <fstream>
#include "server_list.h"

namespace {
    inline static const picojson::object DEFAULT_CONFIG {
        {"servers", picojson::value{picojson::array{}}},
        {"selected_server", picojson::value{0ll}},
    };
}

void server_list::select_server(size_t index, bool save_to_config) {
    std::lock_guard lk(mtx_);
    server_index_ = std::clamp(index, 0u, (std::min)(servers_.size(), MAX_SERVERS_COUNT - 1));
    selected_server_background_->SetPosition({ 0.3f,
            SERVER_LIST_Y_BEGIN + 0.001f + server_index_ * SERVER_ENTRY_HEIGHT });
    if (save_to_config) config_modified_ = true;
}

void server_list::delete_selected_server() {
    std::unique_lock lk(mtx_);
    if (server_index_ >= servers_.size()) return;
    servers_.erase(servers_.begin() + server_index_);
    config_modified_ = true;
    lk.unlock();
    enter_server_list();
}

void server_list::save_server_data() {
    std::unique_lock lk(mtx_);
    std::string address = server_address_->GetText(),
            name = server_name_->GetText();
    if (address.empty()) {
        if (name.empty())
            enter_server_list();
        return;
    };
    if (server_index_ >= servers_.size()) servers_.push_back({});
    auto& entry = servers_[(std::min)(server_index_, servers_.size())];
    entry = picojson::value{picojson::object{ {"address", picojson::value{address}},
              {"name", picojson::value{name}} }};
    config_modified_ = true;
    lk.unlock();
    enter_server_list();
}

void server_list::hide_server_list() {
    new_server_->SetVisible(false);
    selected_server_background_->SetVisible(false);
    for (size_t i = 0; i < MAX_SERVERS_COUNT; ++i)
        server_labels_[i]->SetVisible(false);
}

void server_list::hide_server_edit() {
    server_address_->SetVisible(false);
    server_address_label_->SetVisible(false);
    server_address_background_->SetVisible(false);
    server_name_->SetVisible(false);
    server_name_label_->SetVisible(false);
    server_name_background_->SetVisible(false);
    edit_cancel_->SetVisible(false);
    edit_save_->SetVisible(false);
}

void server_list::hide_connection_status() {
    connection_status_->SetVisible(false);
}

void server_list::enter_server_list() {
    hide_server_edit();
    hide_connection_status();
    header_->SetText("BMMO Server List");
    header_->SetVisible(true);
    for (size_t i = 0; i < MAX_SERVERS_COUNT; ++i) {
        if (i < servers_.size()) {
            auto& entry = servers_[i].get<picojson::object>();
            auto& name = entry["name"].get<std::string>(),
                & address = entry["address"].get<std::string>();
            server_labels_[i]->SetText((name.empty() ? address : name).c_str());
        }
        else
            server_labels_[i]->SetText("");
        server_labels_[i]->SetVisible(true);
    }
    new_server_->SetPosition({ 0.31f, 0.33f + SERVER_ENTRY_HEIGHT * servers_.size() });
    new_server_->SetVisible(servers_.size() < MAX_SERVERS_COUNT);
    selected_server_background_->SetPosition({ 0.3f,
            SERVER_LIST_Y_BEGIN + SERVER_ENTRY_HEIGHT * server_index_ + 0.001f });
    selected_server_background_->SetVisible(true);
    hints_->SetVisible(true);
    hints_->SetText("<Enter> Join selected server\n<E> Edit entry \xB7 <Delete> \xB7 <Esc>");
    select_server(server_index_, false);
    screen_ = ServerList;
}

void server_list::enter_server_edit() {
    hide_server_list();
    hide_connection_status();
    header_->SetText("Edit Server Info");
    header_->SetVisible(true);
    server_address_->SetVisible(true);
    server_address_label_->SetVisible(true);
    server_address_background_->SetVisible(true);
    server_name_->SetVisible(true);
    server_name_label_->SetVisible(true);
    server_name_background_->SetVisible(true);
    SetFocus(nullptr);
    bool create_new = (server_index_ >= servers_.size());
    hints_->SetText("Default port: 26676 \xB7 <Enter> Save data\n<Up|Down|Tab> Move input focus \xB7 <Esc>");
    hints_->SetVisible(true);
    picojson::object* entry{};
    if (!create_new) entry = &(servers_[server_index_].get<picojson::object>());
    server_address_->SetText(create_new ? "" : (*entry)["address"].get<std::string>().c_str());
    server_name_->SetText(create_new ? "" : (*entry)["name"].get<std::string>().c_str());
    edit_cancel_->SetVisible(true);
    edit_save_->SetVisible(true);
    bml_->AddTimerLoop(CKDWORD(1), [this] {
        if (bml_->GetInputManager()->oIsKeyDown(CKKEY_E))
            return true;
        SetFocus(server_address_);
        return false;
    });
    screen_ = ServerEditor;
}

void server_list::save_config() {
    std::ofstream extra_config(EXTRA_CONFIG_PATH);
    if (!extra_config.is_open()) return;
    // we can use (picojson::) value::set<array> but using value::set(array)
    // gives value::set<array&> and linker error somehow
    // also there doesn't seem to be value::set<int64_t> for some reason
    picojson::object o(DEFAULT_CONFIG);
    o["servers"].set<decltype(servers_)>(servers_);
    o["selected_server"] = picojson::value{int64_t(server_index_)};
    extra_config << picojson::value{o}.serialize(true);
    extra_config.close();
    config_modified_ = false;
}

void server_list::exit_gui(CKDWORD key) {
    gui_visible_ = false;
    bml_->GetInputManager()->ShowCursor(previous_mouse_visibility_);
    if (config_modified_) save_config();
    screen_ = None;
    set_input_block(false, key, [this] { return gui_visible_; }, [this] {
        SetVisible(false);
        process_ = [] {};
    });
}

void server_list::connect_to_server() {
    auto index = server_index_ % (servers_.size() + 1);
    if (index >= servers_.size()) {
        enter_server_edit();
        return;
    }
    auto& entry = servers_[index].get<picojson::object>();
    const auto& address = entry["address"].get<std::string>(),
            & name = entry["name"].get<std::string>();
    hide_server_edit();
    hide_server_list();
    header_->SetVisible(false);
    hints_->SetVisible(false);
    connection_status_->SetText(("Connecting to\n[" + (name.empty() ? address : name)
                                  + "]\n...").c_str());
    connection_status_->SetVisible(true);
    bml_->AddTimer(500.0f, [this] { exit_gui(CKKEY_RETURN); });
    connect_callback_(address.c_str(), name.c_str());
    screen_ = ConnectionScreen;
}

void server_list::set_input_block(bool block, CKDWORD defer_key, std::function<bool()> cancel_condition, std::function<void()> callback) {
    auto input_manager = bml_->GetInputManager();
    bml_->AddTimerLoop(CKDWORD(1), [=, this] {
        if (cancel_condition())
            return false;
        if (input_manager->oIsKeyDown(defer_key))
            return true;
        input_manager->SetBlock(block);
        callback();
        return false;
    });
}

server_list::server_list(IBML* bml, log_manager* log_manager, decltype(connect_callback_) connect_callback):
        bml_(bml), log_manager_(log_manager), connect_callback_(connect_callback) {
    picojson::value v;
    std::ifstream extra_config(EXTRA_CONFIG_PATH);
    if (extra_config.is_open()) extra_config >> v;
    else v = picojson::value{ DEFAULT_CONFIG };
    extra_config.close();
    try {
        servers_ = v.get<picojson::object>()["servers"].get<picojson::array>();
        server_index_ = (size_t)v.get<picojson::object>()["selected_server"].get<int64_t>();
    }
    catch (const std::exception& e) {
        log_manager_->get_logger()->Info("Error parsing %s: %s", EXTRA_CONFIG_PATH, e.what());
    }
}

void server_list::init_gui() {
    AddPanel("MMO_Server_List_Background", VxColor(0, 0, 0, 140), 0.25f, 0.25f, 0.5f, 0.5f)->SetZOrder(1024);
    header_ = AddTextLabel("MMO_Server_List_Title", "", ExecuteBB::GAMEFONT_01, 0.27f, 0.27f, 0.46f, 0.06f);
    header_->SetZOrder(1032);
    header_->SetAlignment(ALIGN_CENTER);
    float server_list_bottom_ = 0.33f;
    selected_server_background_ = AddPanel("MMO_Selected_Server", VxColor(255, 175, 28, 96), 0, 0, 0.4f, 0.032f);
    selected_server_background_->SetZOrder(1028);
    for (size_t i = 0; i < MAX_SERVERS_COUNT; ++i) {
        auto* label = AddTextLabel(("MMO_Server_Entry_" + std::to_string(i)).c_str(),
                                    "", ExecuteBB::GAMEFONT_03,
                                    0.31f, SERVER_LIST_Y_BEGIN + SERVER_ENTRY_HEIGHT * i,
                                    0.38f, SERVER_ENTRY_HEIGHT);
        label->SetAlignment(ALIGN_CENTER);
        label->SetZOrder(1032);
        server_labels_[i] = label;
    }
    new_server_ = AddTextLabel("MMO_New_Server", "[ Add New ]", ExecuteBB::GAMEFONT_03A,
                               0, 0, 0.38f, SERVER_ENTRY_HEIGHT);
    new_server_->SetAlignment(ALIGN_CENTER);
    new_server_->SetZOrder(1032);
    hints_ = AddTextLabel("MMO_Server_List_Hints", "", ExecuteBB::GAMEFONT_03A,
                          0.27f, 0.67f, 0.46f, 0.08f);
    hints_->SetAlignment(ALIGN_CENTER);
    hints_->SetZOrder(1032);

    server_address_label_ = AddTextLabel("MMO_Server_Address_Label", "Server Address [*Required]",
                                         ExecuteBB::GAMEFONT_03, 0.3f, SERVER_LIST_Y_BEGIN + 0.03f,
                                         0.4f, SERVER_ENTRY_HEIGHT);
    server_address_label_->SetAlignment(ALIGN_LEFT);
    server_address_label_->SetZOrder(1032);
    server_address_background_ = AddPanel("MMO_Server_Address_Background", VxColor(0, 0, 0, 140),
                                          0.31f, SERVER_LIST_Y_BEGIN + 0.035f + SERVER_ENTRY_HEIGHT,
                                          0.38f, SERVER_ENTRY_HEIGHT + 0.001f);
    server_address_background_->SetZOrder(1036);
    server_address_ = AddTextInput("MMO_Server_Address_Input", ExecuteBB::GAMEFONT_03,
                                   0.315f, SERVER_LIST_Y_BEGIN + 0.04f + SERVER_ENTRY_HEIGHT,
                                   0.37f, SERVER_ENTRY_HEIGHT - 0.002f, [this](CKDWORD key) {});
    server_address_->SetAlignment(ALIGN_LEFT);
    server_address_->SetTextFlags(TEXT_SCREEN | TEXT_RESIZE_VERT);
    server_address_->SetZOrder(1040);
    server_name_label_ = AddTextLabel("MMO_Server_Name_Label", "Server Name (alias)",
                                      ExecuteBB::GAMEFONT_03, 0.3f, SERVER_NAME_Y_BEGIN,
                                      0.4f, SERVER_ENTRY_HEIGHT);
    server_name_label_->SetAlignment(ALIGN_LEFT);
    server_name_label_->SetZOrder(1032);
    server_name_background_ = AddPanel("MMO_Server_Name_Background", VxColor(0, 0, 0, 140),
                                       0.31f, SERVER_NAME_Y_BEGIN + 0.005f + SERVER_ENTRY_HEIGHT,
                                       0.38f, SERVER_ENTRY_HEIGHT + 0.001f);
    server_name_background_->SetZOrder(1036);
    server_name_ = AddTextInput("MMO_Server_Name_Input", ExecuteBB::GAMEFONT_03,
                                0.315f, SERVER_NAME_Y_BEGIN + 0.01f + SERVER_ENTRY_HEIGHT,
                                0.37f, SERVER_ENTRY_HEIGHT - 0.002f, [this](CKDWORD key) {});
    server_name_->SetAlignment(ALIGN_LEFT);
    server_name_->SetTextFlags(TEXT_SCREEN | TEXT_RESIZE_VERT);
    server_name_->SetZOrder(1040);
    edit_cancel_ = AddSmallButton("MMO_Server_Edit_Cancel", "Cancel", 0.59f, 0.3912f,
                                  [this] { enter_server_list(); });
    edit_cancel_->SetZOrder(1032);
    edit_cancel_->SetActive(false);
    edit_save_ = AddSmallButton("MMO_Server_Edit_Save", "Save", 0.59f, 0.54f,
                                [this] { save_server_data(); });
    edit_save_->SetZOrder(1032);

    connection_status_ = AddTextLabel("MMO_Server_List_Connection_Status", "",
                                      ExecuteBB::GAMEFONT_01, 0.3f, 0.39f, 0.4f, 0.22f);
    connection_status_->SetAlignment(ALIGN_CENTER);
    connection_status_->SetZOrder(1032);

    SetCanBeBlocked(false);
    SetVisible(false);
}

void server_list::enter_gui() {
    if (gui_visible_) return;
    gui_visible_ = true;
    SetVisible(true);
    auto input_manager = bml_->GetInputManager();
    previous_mouse_visibility_ = input_manager->GetCursorVisibility() || !bml_->IsIngame();
    input_manager->ShowCursor(true);
    set_input_block(true, CKKEY_RETURN, [this] { return !gui_visible_; });
    process_ = [this] { Process(); };
    enter_server_list();
}

void server_list::OnCharTyped(CKDWORD key) {
    BGui::Gui::OnCharTyped(key);
    switch (screen_) {
    case ServerEditor:
        switch (key) {
            case CKKEY_UP: SetFocus(server_address_); break;
            case CKKEY_DOWN: SetFocus(server_name_); break;
            case CKKEY_RETURN: save_server_data(); break;
            case CKKEY_ESCAPE: enter_server_list(); break;
            case CKKEY_TAB: {
                SetFocus((server_address_->GetTextFlags() & TEXT_SHOWCARET)
                         ? server_name_ : server_address_);
                break;
            }
        }
        break;
    case ServerList:
        switch (key) {
            case CKKEY_DOWN: select_server((server_index_ + 1) % (servers_.size() + 1)); break;
            case CKKEY_UP: select_server((server_index_ + servers_.size()) % (servers_.size() + 1)); break;
            case CKKEY_E: enter_server_edit(); break;
            case CKKEY_DELETE: delete_selected_server(); break;
            case CKKEY_ESCAPE: exit_gui(key); break;
            case CKKEY_RETURN: connect_to_server(); break;
        }
        break;
    default:
        break;
    }
}

void server_list::OnMouseDown(float x, float y, CK_MOUSEBUTTON key) {
    BGui::Gui::OnMouseDown(x, y, key);
    if (key != CK_MOUSEBUTTON_LEFT && key != CK_MOUSEBUTTON_RIGHT)
        return;
    /*Vx2DVector mouse_pos; VxRect screen_size;
    input_manager_->GetMousePosition(mouse_pos, false);
    bml_->GetRenderContext()->GetViewRect(screen_size);
    mouse_pos.x /= screen_size.GetWidth(); mouse_pos.y /= screen_size.GetHeight();*/
    if (Intersect(x, y, selected_server_background_)) {
        if (key == CK_MOUSEBUTTON_LEFT)
            connect_to_server();
        else
            enter_server_edit();
        return;
    }
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (!Intersect(x, y, server_labels_[i])) continue;
        select_server(i);
        break;
    }
    if (Intersect(x, y, new_server_))
        select_server(servers_.size());
}
