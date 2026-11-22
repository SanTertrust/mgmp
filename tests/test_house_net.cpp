// Exercise the TCP receive queue without starting the game or opening a save.
#include "mgmp_net.h"

#include <winsock2.h>
#include <windows.h>

#include <cstdio>

namespace mgmp {
void log_line(const char*, const char*, ...) {}
}

namespace {
using namespace mgmp;

uint16_t reserve_port() {
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    const bool ok = bind(listener, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
                    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    closesocket(listener);
    return ok ? ntohs(address.sin_port) : 0;
}

bool send_frame(SOCKET client, const uint8_t* payload, uint32_t length) {
    uint8_t frame[128];
    if (length == 0 || length > sizeof(frame) - 5) return false;
    const uint32_t framed_length = length + 1;
    memcpy(frame, &framed_length, sizeof(framed_length));
    frame[4] = kNoPeer; // The host stamps the assigned peer id.
    memcpy(frame + 5, payload, length);
    const int total = static_cast<int>(length + 5);
    int sent = 0;
    while (sent < total) {
        const int count = send(client, reinterpret_cast<const char*>(frame + sent), total - sent, 0);
        if (count <= 0) return false;
        sent += count;
    }
    return true;
}

bool send_drag(SOCKET client, uint32_t generation, uint32_t capture, uint32_t seq) {
    uint8_t payload[64];
    const HouseDragMoveMsg drag{generation, 42, capture, seq, 3.0f, 4.0f, 1};
    return send_frame(client, payload, enc_house_drag_move(payload, sizeof(payload), drag));
}

bool verify_stream(SOCKET client) {
    const uint64_t roster_deadline = GetTickCount64() + 3000;
    while (net_stats().received == 0 && GetTickCount64() < roster_deadline) Sleep(1);
    NetMsg roster{};
    if (!net_poll(roster) || roster.type != MSG_PEERS) return false;
    net_msg_release(roster);

    uint8_t payload[64];
    const HouseDropReqMsg release{3, 42, 7, 4, 3.0f, 4.0f};
    if (!send_drag(client, 3, 7, 1) || !send_drag(client, 3, 7, 3) ||
        !send_drag(client, 3, 7, 2) ||
        !send_frame(client, payload, enc_house_drop_req(payload, sizeof(payload), release)) ||
        !send_drag(client, 3, 7, 5) || !send_drag(client, 3, 8, 1) ||
        !send_drag(client, 4, 8, 1))
        return false;

    const uint64_t deadline = GetTickCount64() + 3000;
    // Seven samples/control frames plus the host's locally enqueued roster.
    while (net_stats().received < 8 && GetTickCount64() < deadline) Sleep(1);
    if (net_stats().house_samples_coalesced != 2) return false;

    const uint8_t types[] = {MSG_HOUSE_DRAG_MOVE, MSG_HOUSE_DROP_REQ, MSG_HOUSE_DRAG_MOVE,
                             MSG_HOUSE_DRAG_MOVE, MSG_HOUSE_DRAG_MOVE};
    const uint32_t generations[] = {3, 3, 3, 3, 4};
    const uint32_t captures[] = {7, 7, 7, 8, 8};
    const uint32_t sequences[] = {3, 4, 5, 1, 1};
    size_t found = 0;
    NetMsg msg{};
    while (net_poll(msg)) {
        const uint8_t type = msg.type;
        if (type == MSG_PEERS) {
            net_msg_release(msg);
            continue;
        }
        const bool drop = type == MSG_HOUSE_DROP_REQ;
        const uint32_t generation = drop ? msg.house_drop_req.generation : msg.house_drag_move.generation;
        const uint32_t capture = drop ? msg.house_drop_req.capture_id : msg.house_drag_move.capture_id;
        const uint32_t seq = drop ? msg.house_drop_req.seq : msg.house_drag_move.seq;
        const bool valid = found < sizeof(types) && types[found] == type &&
                           generations[found] == generation && captures[found] == capture &&
                           sequences[found] == seq && msg.from == 1;
        net_msg_release(msg);
        if (!valid) return false;
        ++found;
    }
    return found == sizeof(types);
}

bool verify_disconnect() {
    const uint64_t deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline) {
        NetMsg msg{};
        if (net_poll(msg)) {
            const bool expected = msg.type == kNetPeerDisconnected && msg.from == 1;
            net_msg_release(msg);
            if (expected) return true;
        } else {
            Sleep(1);
        }
    }
    return false;
}
} // namespace

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    const uint16_t port = reserve_port();
    bool ok = false;
    if (port && net_host(port, /*loopback_only=*/true)) {
        const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                ok = verify_stream(client);
            closesocket(client);
            if (ok) ok = verify_disconnect();
        }
        net_shutdown();
    }
    WSACleanup();
    puts(ok ? "House motion coalesced across independent streams only"
            : "House motion coalescing or release order failed");
    return ok ? 0 : 1;
}
