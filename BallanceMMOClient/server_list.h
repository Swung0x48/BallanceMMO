#include <memory>
#include <map>
#include <fstream>

#ifndef PICOJSON_USE_INT64
# define PICOJSON_USE_INT64
#endif // !PICOJSON_USE_INT64
#include <picojson/picojson.h>

#include "bml_includes.h"

class server_list {
private:
    IBML* bml_;
    ILogger* logger_;
    InputHook* input_manager_{};
    std::unique_ptr<BGui::Gui> gui_;
    inline static const picojson::object DEFAULT_CONFIG {
            {"servers", picojson::value{picojson::array{}}},
            {"selected_server", picojson::value{0ll}},
    };
    static constexpr float SERVER_ENTRY_HEIGHT = 0.034f, SERVER_LIST_Y_BEGIN = 0.33f,
        SERVER_NAME_Y_BEGIN = SERVER_LIST_Y_BEGIN + (SERVER_ENTRY_HEIGHT + 0.01f) * 3;
    static constexpr size_t MAX_SERVERS_COUNT = 10;
    static constexpr const char* EXTRA_CONFIG_PATH = "..\\ModLoader\\Config\\BallanceMMOClient_extra.json";
    bool gui_visible_ = false, is_editing_ = false;
    BGui::Panel* selected_server_background_{},
        * server_address_background_{}, * server_name_background_{};
    BGui::Label* header_{}, * new_server_{}, * hints_{},
        * server_address_label_{}, * server_name_label_{},
        * connection_status_{};
    BGui::Label* server_labels_[MAX_SERVERS_COUNT]{};
    picojson::array servers_{};
    BGui::Input* server_address_{}, * server_name_{};
    BGui::Button* edit_cancel_{}, * edit_ok_{};
    size_t server_index_{};
    std::function<void()> process_ = [this] {};
    std::function<void(std::string, std::string)> connect_callback_{};

    void select_server(size_t index) {
        server_index_ = std::clamp(index, 0u, (std::min)(servers_.size(), MAX_SERVERS_COUNT - 1));
        selected_server_background_->SetPosition({ 0.3f,
                SERVER_LIST_Y_BEGIN + 0.001f + server_index_ * SERVER_ENTRY_HEIGHT });
    }

    void delete_selected_server() {
        if (server_index_ >= servers_.size()) return;
        servers_.erase(servers_.begin() + server_index_);
        enter_server_list();
        select_server(server_index_);
    }

    void save_server_data() {
        std::string address = server_address_->GetText();
        if (address.empty()) {
            enter_server_list();
            return;
        };
        if (server_index_ >= servers_.size()) servers_.push_back({});
        auto& entry = servers_[(std::min)(server_index_, servers_.size())];
        entry = picojson::value{picojson::object{ {"address", picojson::value{address}},
                  {"name", picojson::value{server_name_->GetText()}} }};
        enter_server_list();
    }

    void hide_server_list() {
        new_server_->SetVisible(false);
        selected_server_background_->SetVisible(false);
        for (size_t i = 0; i < MAX_SERVERS_COUNT; ++i)
            server_labels_[i]->SetVisible(false);
    }

    void hide_server_edit() {
        server_address_->SetVisible(false);
        server_address_label_->SetVisible(false);
        server_address_background_->SetVisible(false);
        server_name_->SetVisible(false);
        server_name_label_->SetVisible(false);
        server_name_background_->SetVisible(false);
        edit_cancel_->SetVisible(false);
        edit_ok_->SetVisible(false);
    }

    void hide_connection_status() {
        connection_status_->SetVisible(false);
    }

    void enter_server_list() {
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
        select_server(server_index_);
        is_editing_ = false;
    }

    void enter_server_edit() {
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
        gui_->SetFocus(nullptr);
        bool create_new = (server_index_ >= servers_.size());
        hints_->SetText("Default port: 26676 \xB7 <Enter> Save data\n<Up|Down> Move input focus \xB7 <Esc>");
        hints_->SetVisible(true);
        picojson::object* entry{};
        if (!create_new) entry = &(servers_[server_index_].get<picojson::object>());
        server_address_->SetText(create_new ? "" : (*entry)["address"].get<std::string>().c_str());
        server_name_->SetText(create_new ? "" : (*entry)["name"].get<std::string>().c_str());
        edit_cancel_->SetVisible(true);
        edit_ok_->SetVisible(true);
        bml_->AddTimerLoop(CKDWORD(1), [this] {
            if (input_manager_->oIsKeyDown(CKKEY_E))
                return true;
            gui_->SetFocus(server_address_);
            return false;
        });
        is_editing_ = true;
    }

    void save_config() {
        std::ofstream extra_config(EXTRA_CONFIG_PATH);
        if (!extra_config.is_open()) return;
        picojson::object o(DEFAULT_CONFIG);
        o["servers"].set<decltype(servers_)>(servers_);
        o["selected_server"] = picojson::value{int64_t(server_index_)};
        extra_config << picojson::value{o}.serialize(true);
        extra_config.close();
    }

    void exit_gui(CKDWORD key) {
        gui_visible_ = false;
        save_config();
        bml_->AddTimerLoop(CKDWORD(1), [this, key] {
            if (gui_visible_) return false;
            if (input_manager_->oIsKeyDown(key))
                return true;
            gui_->SetVisible(false);
            input_manager_->SetBlock(false);
            process_ = [] {};
            return false;
        });
    }

    void poll_local_input() {
        if (is_editing_) {
            if (input_manager_->oIsKeyPressed(CKKEY_UP))
                gui_->SetFocus(server_address_);
            else if (input_manager_->oIsKeyPressed(CKKEY_DOWN))
                gui_->SetFocus(server_name_);
            else if (input_manager_->oIsKeyPressed(CKKEY_RETURN))
                save_server_data();
            else if (input_manager_->oIsKeyPressed(CKKEY_ESCAPE))
                enter_server_list();
        }
        else {
            if (input_manager_->oIsKeyPressed(CKKEY_DOWN))
                select_server(size_t(int(server_index_ + 1) % (servers_.size() + 1)));
            else if (input_manager_->oIsKeyPressed(CKKEY_UP))
                select_server(size_t(int(server_index_ - 1) % (servers_.size() + 1)));
            else if (input_manager_->oIsKeyPressed(CKKEY_E))
                enter_server_edit();
            else if (input_manager_->oIsKeyPressed(CKKEY_DELETE))
                delete_selected_server();
            else if (input_manager_->oIsKeyPressed(CKKEY_ESCAPE))
                exit_gui(CKKEY_ESCAPE);
            else if (input_manager_->oIsKeyPressed(CKKEY_RETURN)) {
              if (server_index_ == servers_.size())
                  enter_server_edit();
              else {
                  auto& entry = servers_[server_index_ % servers_.size()].get<picojson::object>();
                  hide_server_edit();
                  hide_server_list();
                  header_->SetVisible(false);
                  hints_->SetVisible(false);
                  connection_status_->SetText(("Connecting to\n["
                                               + entry["name"].get<std::string>() + "]\n...").c_str());
                  connection_status_->SetVisible(true);
                  bml_->AddTimer(1000.0f, [this] { exit_gui(CKKEY_RETURN); });
                  connect_callback_(entry["address"].get<std::string>(), entry["name"].get<std::string>());
              }
            }
        }
    }

public:
    server_list(IBML* bml, ILogger* logger, std::function<void(std::string, std::string)> connect_callback):
            bml_(bml), logger_(logger), connect_callback_(connect_callback) {
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
            logger_->Info("Error parsing %s: %s", EXTRA_CONFIG_PATH, e.what());
        }
    }

    void init_gui() {
        input_manager_ = bml_->GetInputManager();
        gui_ = std::make_unique<decltype(gui_)::element_type>();
        gui_->AddPanel("MMO_Server_List_Background", VxColor(0, 0, 0, 140), 0.25f, 0.25f, 0.5f, 0.5f)->SetZOrder(1024);
        header_ = gui_->AddTextLabel("MMO_Server_List_Title", "", ExecuteBB::GAMEFONT_01, 0.27f, 0.27f, 0.46f, 0.06f);
        header_->SetZOrder(1032);
        header_->SetAlignment(ALIGN_CENTER);
        float server_list_bottom_ = 0.33f;
        selected_server_background_ = gui_->AddPanel("MMO_Selected_Server", VxColor(255, 175, 28, 96), 0, 0, 0.4f, 0.032f);
        selected_server_background_->SetZOrder(1028);
        for (size_t i = 0; i < MAX_SERVERS_COUNT; ++i) {
            auto* label = gui_->AddTextLabel(("MMO_Server_Entry_" + std::to_string(i)).c_str(),
                                             "", ExecuteBB::GAMEFONT_03,
                                             0.31f, SERVER_LIST_Y_BEGIN + SERVER_ENTRY_HEIGHT * i,
                                             0.38f, SERVER_ENTRY_HEIGHT);
            label->SetAlignment(ALIGN_CENTER);
            label->SetZOrder(1032);
            server_labels_[i] = label;
        }
        new_server_ = gui_->AddTextLabel("MMO_New_Server", "[ Add New ]", ExecuteBB::GAMEFONT_03A,
                                         0, 0, 0.38f, SERVER_ENTRY_HEIGHT);
        new_server_->SetAlignment(ALIGN_CENTER);
        new_server_->SetZOrder(1032);
        hints_ = gui_->AddTextLabel("MMO_Server_List_Hints", "", ExecuteBB::GAMEFONT_03A,
                                    0.27f, 0.67f, 0.46f, 0.08f);
        hints_->SetAlignment(ALIGN_CENTER);
        hints_->SetZOrder(1032);

        server_address_label_ = gui_->AddTextLabel("MMO_Server_Address_Label", "Server Address [*Required]",
                                                   ExecuteBB::GAMEFONT_03, 0.3f, SERVER_LIST_Y_BEGIN + 0.03f,
                                                   0.4f, SERVER_ENTRY_HEIGHT);
        server_address_label_->SetAlignment(ALIGN_LEFT);
        server_address_label_->SetZOrder(1032);
        server_address_background_ = gui_->AddPanel("MMO_Server_Address_Background", VxColor(0, 0, 0, 140),
                                                    0.31f, SERVER_LIST_Y_BEGIN + 0.035f + SERVER_ENTRY_HEIGHT,
                                                    0.38f, SERVER_ENTRY_HEIGHT + 0.001f);
        server_address_background_->SetZOrder(1036);
        server_address_ = gui_->AddTextInput("MMO_Server_Address_Input", ExecuteBB::GAMEFONT_03,
                                             0.315f, SERVER_LIST_Y_BEGIN + 0.04f + SERVER_ENTRY_HEIGHT,
                                             0.37f, SERVER_ENTRY_HEIGHT - 0.002f, [this](CKDWORD key) {});
        server_address_->SetAlignment(ALIGN_LEFT);
        server_address_->SetTextFlags(TEXT_SCREEN | TEXT_RESIZE_VERT);
        server_address_->SetZOrder(1040);
        server_name_label_ = gui_->AddTextLabel("MMO_Server_Name_Label", "Server Name (alias)",
                                                ExecuteBB::GAMEFONT_03, 0.3f, SERVER_NAME_Y_BEGIN,
                                                0.4f, SERVER_ENTRY_HEIGHT);
        server_name_label_->SetAlignment(ALIGN_LEFT);
        server_name_label_->SetZOrder(1032);
        server_name_background_ = gui_->AddPanel("MMO_Server_Name_Background", VxColor(0, 0, 0, 140),
                                                 0.31f, SERVER_NAME_Y_BEGIN + 0.005f + SERVER_ENTRY_HEIGHT,
                                                 0.38f, SERVER_ENTRY_HEIGHT + 0.001f);
        server_name_background_->SetZOrder(1036);
        server_name_ = gui_->AddTextInput("MMO_Server_Name_Input", ExecuteBB::GAMEFONT_03,
                                          0.315f, SERVER_NAME_Y_BEGIN + 0.01f + SERVER_ENTRY_HEIGHT,
                                          0.37f, SERVER_ENTRY_HEIGHT - 0.002f, [this](CKDWORD key) {});
        server_name_->SetAlignment(ALIGN_LEFT);
        server_name_->SetTextFlags(TEXT_SCREEN | TEXT_RESIZE_VERT);
        server_name_->SetZOrder(1040);
        edit_cancel_ = gui_->AddSmallButton("MMO_Server_Edit_Cancel", "Cancel", 0.59f, 0.3912f,
                                            [this] { enter_server_list(); });
        edit_cancel_->SetZOrder(1032);
        edit_ok_ = gui_->AddSmallButton("MMO_Server_Edit_OK", "OK", 0.59f, 0.54f,
                                        [this] { save_server_data(); });
        edit_ok_->SetZOrder(1032);

        connection_status_ = gui_->AddTextLabel("MMO_Server_List_Connection_Status", "",
                                                ExecuteBB::GAMEFONT_01, 0.3f, 0.39f, 0.4f, 0.22f);
        connection_status_->SetAlignment(ALIGN_CENTER);
        connection_status_->SetZOrder(1032);

        gui_->SetCanBeBlocked(false);
        gui_->SetVisible(false);
    }

    inline void process() {
        process_();
    }

    inline void enter_gui() {
        if (gui_visible_) return;
        gui_visible_ = true;
        gui_->SetVisible(true);
        input_manager_->SetBlock(true);
        process_ = [this] {
            poll_local_input();
            gui_->Process();
        };
        enter_server_list();
    }
};
