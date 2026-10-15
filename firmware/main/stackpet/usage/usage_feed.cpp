/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "usage_feed.h"
#include <hal/hal.h>
#include <hal/usb_serial_jtag_ll.h>
#include <mooncake_log.h>
#include <settings.h>
#include <ssid_manager.h>
#include <stackpet/remote/remote_link.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/time.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>

namespace stackpet::usage {

namespace {

constexpr const char* kTag          = "Usage";
constexpr const char* kNvsNamespace = "stackpet_usage";
constexpr const char* kNvsHost      = "host";
constexpr const char* kNvsPort      = "port";
constexpr const char* kNvsToken     = "token";
constexpr const char* kNvsStyle     = "style";
constexpr const char* kDefaultTz    = "GMT0";  // Hal::getTimezone() when SETUP never set one
// The fetch task: an internal stack (the WiFi calls want one), made for each fetch and gone after (nothing of it is
// left for the app opened next: the launcher hands its RAM on, no reboot in between)
constexpr uint32_t kTaskStack       = 6144;
constexpr UBaseType_t kTaskPriority = 2;
// One connection, waited for this long. Just after the join this side holds its frames back, up to seconds (the air
// capture, 2026-10-13: six SYN-ACKs from the Mac came in together 4 s after the first SYN was sent). A late SYN still
// gets its answer to the socket that sent it, and lwIP sends it again (1.5 s, then 3 s later); a fresh socket each
// second had closed the one the answer was for
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr uint32_t kRecvSliceMs      = 300;  // the abort looked at in between

const char* status_name(FeedStatus s)
{
    switch (s) {
        case FeedStatus::NotSetUp:
            return "not set up";
        case FeedStatus::NoWifi:
            return "no WiFi saved";
        case FeedStatus::Waiting:
            return "waiting";
        case FeedStatus::Ok:
            return "ok";
        case FeedStatus::NoRouter:
            return "the WiFi did not connect";
        case FeedStatus::Unreachable:
            return "the Mac did not answer";
        case FeedStatus::Unauthorized:
            return "the Mac refused the token";
        case FeedStatus::BadReply:
            return "not a snapshot";
    }
    return "?";
}

// How long the parts took (the log): connected, the first byte of the answer, the last
struct HttpTimes {
    uint32_t connect_ms = 0;
    uint32_t first_ms   = 0;
    uint32_t last_ms    = 0;
};

// One connection within ms, the abort looked at in between: the socket, connected, or -1 (refused, reset, too late)
int connect_within(const addrinfo* res, uint32_t ms, const std::atomic<bool>& abort)
{
    const int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        return -1;
    }
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    bool connected       = connect(sock, res->ai_addr, res->ai_addrlen) == 0;
    const bool waiting   = !connected && errno == EINPROGRESS;
    const uint32_t start = GetHAL().millis();
    while (waiting && !abort.load()) {
        const uint32_t spent = GetHAL().millis() - start;
        if (spent >= ms) {
            break;
        }
        const uint32_t slice = std::min(kRecvSliceMs, ms - spent);
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(sock, &writable);
        timeval tv{};
        tv.tv_sec       = slice / 1000;
        tv.tv_usec      = slice % 1000 * 1000;
        const int ready = select(sock + 1, nullptr, &writable, nullptr, &tv);
        if (ready < 0) {
            break;
        }
        if (ready > 0) {
            int err        = 0;
            socklen_t size = sizeof(err);
            connected      = getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &size) == 0 && err == 0;
            break;
        }
    }
    if (!connected) {
        close(sock);
        return -1;
    }
    fcntl(sock, F_SETFL, flags);
    return sock;
}

// GET /api/v1/snapshot: what came back, false when the Mac could not be reached
bool http_get(const Config& c, const std::atomic<bool>& abort, std::string& raw, HttpTimes& times)
{
    const uint32_t begin = GetHAL().millis();
    char port[8];
    std::snprintf(port, sizeof(port), "%d", c.port);
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;
    if (getaddrinfo(c.host.c_str(), port, &hints, &res) != 0 || res == nullptr) {
        return false;
    }
    const int sock = connect_within(res, kConnectTimeoutMs, abort);
    freeaddrinfo(res);
    times.connect_ms = GetHAL().millis() - begin;
    if (sock < 0) {
        return false;
    }
    timeval slice{};
    slice.tv_usec = kRecvSliceMs * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &slice, sizeof(slice));
    timeval send_timeout{};
    send_timeout.tv_sec = 2;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    std::string request = "GET /api/v1/snapshot HTTP/1.1\r\nHost: " + c.host + ":" + port +
                          "\r\nAuthorization: Bearer " + c.token + "\r\nConnection: close\r\n\r\n";
    const bool sent = send(sock, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size());
    std::fill(request.begin(), request.end(), '\0');  // the token not left in freed memory
    if (!sent) {
        close(sock);
        return false;
    }
    // Until the Mac closes the connection (it does after its answer), the limit, the timeout or an abort
    const uint32_t start = GetHAL().millis();
    char buf[512];
    while (!abort.load() && GetHAL().millis() - start < kHttpTimeoutMs && raw.size() < kMaxResponse) {
        const int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            if (raw.empty()) {
                times.first_ms = GetHAL().millis() - begin;
            }
            raw.append(buf, static_cast<size_t>(n));
            times.last_ms = GetHAL().millis() - begin;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        }
    }
    close(sock);
    return !raw.empty();
}

// Diagnosis (temporary): which way packets go missing on the router. UDP to an echo on the Mac (port 8788: any server
// that sends each datagram back), ten, 100 ms apart, then half a second for late ones
void udp_probe(const std::string& host)
{
    constexpr int kCount = 10;
    auto& hal            = GetHAL();
    const int sock       = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        mclog::tagWarn(kTag, "udp {}: no socket", host);
        return;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = htons(8788);
    if (inet_aton(host.c_str(), &to.sin_addr) == 0) {
        close(sock);
        return;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    const uint32_t t0 = hal.millis();
    uint32_t back     = 0;
    int sent          = 0;
    int errors        = 0;
    int last_error    = 0;
    int first_back    = -1;
    for (int i = 0; i < kCount + 5; ++i) {
        if (i < kCount) {
            char msg[32];
            const int n = std::snprintf(msg, sizeof(msg), "sp %d %u", i, static_cast<unsigned>(hal.millis() - t0));
            if (sendto(sock, msg, n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == n) {
                ++sent;
            } else {
                ++errors;
                last_error = errno;
            }
        }
        const uint32_t round = hal.millis();
        while (hal.millis() - round < 100) {
            char buf[32];
            const int r = recv(sock, buf, sizeof(buf) - 1, 0);
            if (r <= 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            buf[r]  = '\0';
            int seq = -1;
            if (std::sscanf(buf, "sp %d", &seq) == 1 && seq >= 0 && seq < kCount) {
                if (first_back < 0) {
                    first_back = static_cast<int>(hal.millis() - t0);
                }
                back |= 1u << seq;
            }
        }
    }
    close(sock);
    mclog::tagInfo(kTag, "udp {} from boot ms {}: {} sent ({} errors, errno {}), {} back (mask {:03x}, the first at {} ms)",
                   host, t0, sent, errors, last_error, __builtin_popcount(back), back, first_back);
}

void fix_clock(int64_t macTime)
{
    if (macTime <= 0) {
        return;
    }
    const int64_t ours = static_cast<int64_t>(std::time(nullptr));
    if (std::llabs(macTime - ours) <= kClockFixSec) {
        return;
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(macTime);
    settimeofday(&tv, nullptr);
    GetHAL().syncSystemTimeToRtc();
    mclog::tagInfo(kTag, "clock set from the Mac's ({} s off)", static_cast<long long>(macTime - ours));
}

}  // namespace

struct UsageFeed::Job {
    std::string ssid;
    std::string password;
    Config config;
    int channel = 0;  // the remote link's: the radio goes back there
    std::atomic<bool> abort{false};
    std::atomic<bool> done{false};
    // The result, written before done
    FeedStatus status = FeedStatus::Waiting;
    Snapshot snapshot;
    int http_status   = 0;
    uint32_t join_ms  = 0;
    uint32_t total_ms = 0;
    HttpTimes http;
    std::string arp;
};

UsageFeed& UsageFeed::instance()
{
    static UsageFeed feed;
    return feed;
}

void UsageFeed::run_job(void* arg)
{
    Job* job             = static_cast<Job*>(arg);
    auto& hal            = GetHAL();
    const uint32_t start = hal.millis();
    if (hal.joinRouter(job->ssid, job->password, kJoinTimeoutMs, [job]() { return job->abort.load(); })) {
        job->join_ms = hal.millis() - start;
        udp_probe(job->config.host);
        std::string raw;
        HttpResponse response;
        if (!http_get(job->config, job->abort, raw, job->http)) {
            job->status = FeedStatus::Unreachable;
        } else if (!parseHttpResponse(raw, response)) {
            job->status = FeedStatus::BadReply;
        } else {
            job->http_status = response.status;
            if (response.status == 401 || response.status == 403) {
                job->status = FeedStatus::Unauthorized;
            } else if (response.status == 200 && parseSnapshot(response.body, job->snapshot)) {
                job->status = FeedStatus::Ok;
            } else {
                job->status = FeedStatus::BadReply;
            }
        }
    } else {
        job->status = FeedStatus::NoRouter;
    }
    job->arp = hal.routerArpFor(job->config.host);  // before leaving: whom the packets for the Mac went to
    hal.routerStatsDump();                          // diagnosis (temporary)
    hal.leaveRouter(job->channel);
    job->total_ms = hal.millis() - start;
    job->done     = true;  // last: the main loop takes the result and frees the job
    vTaskDelete(nullptr);
}

void UsageFeed::load_config()
{
    if (_loaded) {
        return;
    }
    _loaded = true;
    Settings settings(kNvsNamespace, false);
    Style style;
    if (parseStyle(settings.GetString(kNvsStyle, styleKey(Style::Claude)), style)) {
        _style = style;
    }
    Config c;
    c.host  = settings.GetString(kNvsHost);
    c.port  = settings.GetInt(kNvsPort, kDefaultPort);
    c.token = settings.GetString(kNvsToken);
    if (c.valid()) {
        _config = std::move(c);
        mclog::tagInfo(kTag, "the Mac at {}:{}", _config.host, _config.port);
    } else {
        _status = FeedStatus::NotSetUp;
        mclog::tagInfo(kTag, "not set up (tools/stackpet_usage_setup.py)");
    }
}

void UsageFeed::start_job(uint32_t nowMs)
{
    _next_ms = nowMs + kFetchEveryMs;
    if (!_config.valid()) {
        _status = FeedStatus::NotSetUp;
        return;
    }
    auto& link = stackpet::remote::RemoteLink::instance();
    if (!link.listening()) {
        return;  // the launcher listens from its first frame on: the WiFi is not up yet
    }
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    if (saved.empty()) {
        _status = FeedStatus::NoWifi;
        return;
    }
    Job* job      = new Job;
    job->ssid     = saved.front().ssid;
    job->password = saved.front().password;
    job->config   = _config;
    link.pause();
    job->channel = link.channel();
    if (xTaskCreate(&UsageFeed::run_job, "usage_fetch", kTaskStack, job, kTaskPriority, nullptr) != pdPASS) {
        link.resume(nowMs);
        delete job;
        mclog::tagWarn(kTag, "no room for the fetch task");
        return;
    }
    _job = job;
}

void UsageFeed::finish_job(uint32_t nowMs)
{
    if (_job == nullptr || !_job->done) {
        return;
    }
    Job* job = _job;
    _job     = nullptr;
    stackpet::remote::RemoteLink::instance().resume(nowMs);
    if (job->abort) {
        mclog::tagInfo(kTag, "fetch stopped after {} ms", job->total_ms);
    } else if (job->status == FeedStatus::Ok) {
        _status      = FeedStatus::Ok;
        _snapshot    = job->snapshot;
        _have        = true;
        _snapshot_ms = nowMs;
        mclog::tagInfo(kTag,
                       "snapshot in {} ms (WiFi {}, TCP {}, first byte {}, last {} ms; ARP {}): 5h {}%, week {}%, state {}",
                       job->total_ms, job->join_ms, job->http.connect_ms, job->http.first_ms, job->http.last_ms, job->arp,
                       _snapshot.session.pct, _snapshot.weekly.pct, static_cast<int>(_snapshot.state));
        fix_clock(_snapshot.ts);
    } else {
        _status = job->status;
        mclog::tagInfo(kTag, "fetch failed after {} ms: {} (HTTP {}; WiFi {}, TCP {}, first byte {} ms; ARP {})",
                       job->total_ms, status_name(job->status), job->http_status, job->join_ms, job->http.connect_ms,
                       job->http.first_ms, job->arp);
    }
    delete job;
}

void UsageFeed::update(uint32_t nowMs, bool wanted)
{
    load_config();
    finish_job(nowMs);
    if (wanted && !_wanted && (!_have || nowMs - _snapshot_ms >= kFetchEveryMs / 2)) {
        _next_ms = nowMs;  // the screensaver just came up: fetch now unless the last one is recent
    }
    if (!wanted && _job != nullptr) {
        _job->abort = true;  // the screensaver went: back to the remote sooner (the result is not needed now)
    }
    _wanted = wanted;
    if (wanted && _job == nullptr && static_cast<int32_t>(nowMs - _next_ms) >= 0) {
        start_job(nowMs);
    }
}

void UsageFeed::stop(uint32_t nowMs)
{
    if (_job == nullptr) {
        return;
    }
    _job->abort = true;
    while (!_job->done) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    finish_job(nowMs);
}

bool UsageFeed::fetching() const
{
    return _job != nullptr;
}

void UsageFeed::fill(ClockModel& m, uint32_t nowMs)
{
    const std::time_t t = std::time(nullptr);
    m.now               = static_cast<int64_t>(t);
    localtime_r(&t, &m.local);
    m.haveSnapshot = _have;
    m.snapshot     = _snapshot;
    m.ageMs        = _have ? nowMs - _snapshot_ms : 0;
    m.status       = _config.valid() ? _status : FeedStatus::NotSetUp;
}

Style UsageFeed::style()
{
    load_config();
    return _style;
}

void UsageFeed::setStyle(Style style)
{
    load_config();
    if (style == _style) {
        return;
    }
    _style = style;
    Settings settings(kNvsNamespace, true);
    settings.SetString(kNvsStyle, styleKey(style));
    mclog::tagInfo(kTag, "screensaver style: {}", styleKey(style));
}

void UsageFeed::handle_line(const std::string& line, uint32_t nowMs)
{
    Config c;
    std::string value;
    switch (parseCommand(line, c, &value)) {
        case Command::Set: {
            Settings settings(kNvsNamespace, true);
            settings.SetString(kNvsHost, c.host);
            settings.SetInt(kNvsPort, c.port);
            settings.SetString(kNvsToken, c.token);
            mclog::tagInfo(kTag, "set up: the Mac at {}:{} (a token of {} characters)", c.host, c.port,
                           c.token.size());
            _config  = std::move(c);
            _loaded  = true;
            _have    = false;  // another Mac's, maybe
            _status  = FeedStatus::Waiting;
            _next_ms = nowMs;
            break;
        }
        case Command::Clear: {
            Settings settings(kNvsNamespace, true);
            settings.EraseAll();
            _config = Config{};
            _have   = false;
            _status = FeedStatus::NotSetUp;
            mclog::tagInfo(kTag, "cleared");
            break;
        }
        case Command::Show:
            if (_config.valid()) {
                mclog::tagInfo(kTag, "the Mac at {}:{} (a token of {} characters), {}", _config.host, _config.port,
                               _config.token.size(), status_name(_status));
            } else {
                mclog::tagInfo(kTag, "not set up");
            }
            break;
        case Command::Style: {
            Style style;
            if (parseStyle(value, style) && style == this->style()) {
                mclog::tagInfo(kTag, "screensaver style: {} (already)", styleKey(style));
            } else if (parseStyle(value, style)) {
                setStyle(style);  // the launcher's screensaver follows it at its next redraw
            }
            break;
        }
        case Command::Timezone: {
            const std::string current = GetHAL().getTimezone();
            if (value == current) {
                mclog::tagInfo(kTag, "the Mac's timezone: {} (the robot's already)", value);
            } else if (current == kDefaultTz) {
                GetHAL().setTimezone(value);
                mclog::tagInfo(kTag, "the Mac's timezone: {}", value);
            } else {
                mclog::tagInfo(kTag, "the Mac's timezone {} not taken: the robot has {} (SETUP)", value, current);
            }
            break;
        }
        case Command::Bad:
            mclog::tagWarn(kTag, "bad line; stackpet usage <host> <port> <token> | clear | show | style <key> | tz <posix>");
            break;
        case Command::None:
            break;
    }
}

void UsageFeed::pollConsole(uint32_t nowMs)
{
    // The USB-Serial/JTAG port is a secondary console here (output only, no driver): what the Mac writes waits in the
    // peripheral's FIFO (the host is held off while it is full), read straight from it
    uint8_t buf[64];
    while (usb_serial_jtag_ll_rxfifo_data_available()) {
        const uint32_t n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        for (uint32_t i = 0; i < n; ++i) {
            const char ch = static_cast<char>(buf[i]);
            if (ch == '\n') {
                if (!_line_too_long) {
                    handle_line(_line, nowMs);
                }
                std::fill(_line.begin(), _line.end(), '\0');
                _line.clear();
                _line_too_long = false;
            } else if (_line.size() < kConsoleLineMax) {
                _line.push_back(ch);
            } else {
                _line_too_long = true;  // dropped whole
            }
        }
    }
}

}  // namespace stackpet::usage
