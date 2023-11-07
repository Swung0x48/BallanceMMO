#include <memory>
#include <mutex>

#ifndef PICOJSON_USE_INT64
# define PICOJSON_USE_INT64
#endif // !PICOJSON_USE_INT64
#include <picojson/picojson.h>

#include "bml_includes.h"
#include "log_manager.h"

class server_list {
private:
    IBML* bml_;
    log_manager* log_manager_;
    InputHook* input_manager_{};
    std::mutex mtx_{};
    std::unique_ptr<BGui::Gui> gui_;
    static constexpr float SERVER_ENTRY_HEIGHT = 0.034f, SERVER_LIST_Y_BEGIN = 0.33f,
        SERVER_NAME_Y_BEGIN = SERVER_LIST_Y_BEGIN + (SERVER_ENTRY_HEIGHT + 0.01f) * 3;
    static constexpr size_t MAX_SERVERS_COUNT = 10;
    static constexpr const char* EXTRA_CONFIG_PATH = "..\\ModLoader\\Config\\BallanceMMOClient_extra.json";
    bool gui_visible_ = false, is_editing_ = false, config_modified_ = false,
        previous_mouse_visibility_ = true;
    BGui::Panel* selected_server_background_{},
        * server_address_background_{}, * server_name_background_{};
    BGui::Label* header_{}, * new_server_{}, * hints_{},
        * server_address_label_{}, * server_name_label_{},
        * connection_status_{};
    BGui::Label* server_labels_[MAX_SERVERS_COUNT]{};
    picojson::array servers_{};
    BGui::Input* server_address_{}, * server_name_{};
    BGui::Button* edit_cancel_{}, * edit_save_{};
    size_t server_index_{};
    std::function<void()> process_ = [this] {};
    std::function<void(std::string, std::string)> connect_callback_{};

    void select_server(size_t index, bool save_to_config = true);
    void delete_selected_server();

    // save to memory but not config.
    // we're not saving every single time we make a change;
    // instead it is deferred to (with `save_config()`)
    // when we're exiting the server list gui itself.
    void save_server_data();
    void save_config();

    void hide_server_list();
    void hide_server_edit();
    void hide_connection_status();

    void enter_server_list();
    void enter_server_edit();

    void exit_gui(CKDWORD key);

    void connect_to_server();

    void set_input_block(bool block, CKDWORD defer_key, std::function<bool()> cancel_condition,
                         std::function<void()> callback = [] {});

    void poll_local_input();

public:
    server_list(IBML* bml, log_manager* log_manager, decltype(connect_callback_) connect_callback);

    void init_gui();
    void enter_gui();

    inline void process() {
        process_();
    }
};
