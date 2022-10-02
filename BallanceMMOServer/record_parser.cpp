#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif


#include "../BallanceMMOCommon/common.hpp"
#include "../BallanceMMOCommon/entity/record_entry.hpp"
#include <fstream>

template<typename T>
void read_variable(std::istream& stream, T& t) {
    stream.read(reinterpret_cast<char*>(&t), sizeof(T));
}

bool print_states = true;

void parse_message(bmmo::record_entry& entry, SteamNetworkingMicroseconds time) {
    auto* raw_msg = reinterpret_cast<bmmo::general_message*>(entry.data);
    switch (raw_msg->code) {
        case bmmo::LoginAcceptedV2: {
            // printf("Code: LoginAcceptedV2\n");
            // bmmo::login_accepted_v2_msg msg{};
            // msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
            // msg.deserialize();
            // printf("\t%d player(s) online: \n", msg.online_players.size());
            // for (const auto& i: msg.online_players) {
            //     printf("%s, %u, %s\n", i.second.name.c_str(), i.first, (i.second.cheated ? " [CHEAT]" : ""));
            // }
            break;
        }
        case bmmo::OwnedBallStateV2: {
                bmmo::owned_ball_state_v2_msg msg;
                msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    if (print_states)
                        printf("%llu, %u, %d, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf\n",
                            time,
                            ball.player_id,
                            ball.state.type,
                            ball.state.position.x,
                            ball.state.position.y,
                            ball.state.position.z,
                            ball.state.rotation.x,
                            ball.state.rotation.y,
                            ball.state.rotation.z,
                            ball.state.rotation.w);
                }

                break;
            }
            case bmmo::OwnedTimedBallState: {
                bmmo::owned_timed_ball_state_msg msg;
            msg.raw.write(reinterpret_cast<char*>(entry.data), entry.size);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    if (print_states)
                        printf("%llu, %u, %d, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf\n",
                            time,
                            ball.player_id,
                            ball.state.type,
                            ball.state.position.x,
                            ball.state.position.y,
                            ball.state.position.z,
                            ball.state.rotation.x,
                            ball.state.rotation.y,
                            ball.state.rotation.z,
                            ball.state.rotation.w);
                }

                break;
            }
        default: {
            // printf("Code: %d\n", raw_msg->code);
            break;
        }
    }
    // std::cout.write(entry.data, entry.size);
}

int main(int argc, char** argv) {
    std::string filename;
    if (argc <= 1) {
        std::cout << "Record name: ";
        std::cin >> filename;
    }
    else
        filename = argv[1];
    std::ifstream record_stream(filename, std::ios::binary);
    if (!record_stream.is_open()) {
        std::cerr << "Error: cannot open record file." << std::endl;
        return 1;
    }
    std::string read_data;
    std::getline(record_stream, read_data, '\0');
    if (read_data != "BallanceMMO FlightRecorder") {
        std::cerr << "Error: invalid record file." << std::endl;
        return 1;
    }
    puts("BallanceMMO FlightRecorder Data");
    bmmo::version_t version;
    read_variable(record_stream, version);
    // record_stream.read(reinterpret_cast<char*>(&version), sizeof(version));
    printf("Version: \t\t%s\n", version.to_string().c_str());
    time_t start_time;
    read_variable(record_stream, start_time);
    // record_stream.read(reinterpret_cast<char *>(&start_time), sizeof(start_time));
    char time_str[32];
    strftime(time_str, 32, "%F %T", localtime(&start_time));
    printf("Record start time: \t%s\n", time_str);
    SteamNetworkingMicroseconds init_timestamp;
    read_variable(record_stream, init_timestamp);

    while (record_stream.good() && record_stream.peek() != std::ifstream::traits_type::eof()) {
        SteamNetworkingMicroseconds timestamp;
        read_variable(record_stream, timestamp);
        int32_t size;
        read_variable(record_stream, size);
        bmmo::record_entry entry(size);
        record_stream.read(reinterpret_cast<char*>(entry.data), size);
        // record_stream.seekg(size, std::ios::cur);
        // printf("Record | Time: %7.2lf | Size: %5d\n", 
        //     (timestamp - init_timestamp) / 1e6, size);
        
        parse_message(entry, timestamp);
    }
}
