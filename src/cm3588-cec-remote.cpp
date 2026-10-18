// CM3588 HDMI-CEC -> Linux uinput keyboard bridge.
// Target: Debian 13 / Linux 6.1 RK3588, with /dev/cec0 and /dev/uinput.
// Deliberately independent of WayDroid: Weston forwards its keyboard events.
#include <linux/cec.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
volatile sig_atomic_t stopping = 0;
void stop_handler(int) { stopping = 1; }

std::runtime_error sys_error(const char* what) {
    return std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

constexpr int key_for_cec(unsigned code) {
    switch (code) {
        case CEC_OP_UI_CMD_SELECT: return KEY_SELECT;
        case CEC_OP_UI_CMD_UP: return KEY_UP;
        case CEC_OP_UI_CMD_DOWN: return KEY_DOWN;
        case CEC_OP_UI_CMD_LEFT: return KEY_LEFT;
        case CEC_OP_UI_CMD_RIGHT: return KEY_RIGHT;
        case CEC_OP_UI_CMD_BACK: return KEY_BACK;
        case CEC_OP_UI_CMD_DEVICE_ROOT_MENU: return KEY_HOMEPAGE;
        default: return -1;
    }
}

static_assert(key_for_cec(CEC_OP_UI_CMD_SELECT) == KEY_SELECT);
static_assert(key_for_cec(CEC_OP_UI_CMD_BACK) == KEY_BACK);

class Keyboard {
public:
    Keyboard() : fd_(::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)) {
        if (fd_.get() < 0) throw sys_error("open /dev/uinput");
        if (::ioctl(fd_.get(), UI_SET_EVBIT, EV_KEY) < 0 ||
            ::ioctl(fd_.get(), UI_SET_EVBIT, EV_SYN) < 0)
            throw sys_error("uinput UI_SET_EVBIT");

        // A few ordinary keys ensure udev/libinput classify this as a keyboard.
        const int keys[] = {KEY_A, KEY_B, KEY_C, KEY_SPACE, KEY_ENTER,
                            KEY_SELECT, KEY_ESC, KEY_BACK, KEY_UP, KEY_DOWN,
                            KEY_LEFT, KEY_RIGHT, KEY_HOMEPAGE};
        for (int key : keys)
            if (::ioctl(fd_.get(), UI_SET_KEYBIT, key) < 0)
                throw sys_error("uinput UI_SET_KEYBIT");

        uinput_setup setup{};
        setup.id.bustype = BUS_USB;
        setup.id.vendor = 0x1;
        setup.id.product = 0x1;
        std::snprintf(setup.name, sizeof(setup.name), "CM3588 HDMI-CEC Remote");
        if (::ioctl(fd_.get(), UI_DEV_SETUP, &setup) < 0 ||
            ::ioctl(fd_.get(), UI_DEV_CREATE) < 0)
            throw sys_error("create uinput keyboard");
        created_ = true;
        std::puts("Virtual keyboard ready");
        std::fflush(stdout);
    }
    ~Keyboard() {
        release();
        if (created_) ::ioctl(fd_.get(), UI_DEV_DESTROY);
    }
    void press(int key) {
        if (held_ == key) { last_press_ = Clock::now(); return; }
        release();
        write_event(EV_KEY, key, 1);
        sync();
        held_ = key;
        last_press_ = Clock::now();
    }
    void release() {
        if (held_ < 0) return;
        write_event(EV_KEY, held_, 0);
        sync();
        held_ = -1;
    }
    void check_timeout() {
        // Safety net for a missed CEC RELEASE. A held remote normally
        // sends repeat PRESSED messages, which refresh last_press_.
        if (held_ >= 0 && Clock::now() - last_press_ > std::chrono::milliseconds(1500)) {
            std::fprintf(stderr, "CEC release timeout: key %d\n", held_);
            release();
        }
    }
private:
    using Clock = std::chrono::steady_clock;
    void write_event(unsigned short type, unsigned short code, int value) {
        input_event ev{};
        ev.type = type;
        ev.code = code;
        ev.value = value;
        if (::write(fd_.get(), &ev, sizeof(ev)) != sizeof(ev))
            throw sys_error("write uinput event");
    }
    void sync() { write_event(EV_SYN, SYN_REPORT, 0); }
    Fd fd_;
    bool created_ = false;
    int held_ = -1;
    Clock::time_point last_press_{};
};

// Claim a playback logical address if CEC has no existing configuration.
// The kernel manages the physical address from HDMI EDID; never hardcode it.
void configure_cec(int fd) {
    cec_log_addrs addrs{};
    if (::ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &addrs) < 0)
        throw sys_error("CEC_ADAP_G_LOG_ADDRS");
    if (addrs.num_log_addrs) return;

    __u16 phys{};
    if (::ioctl(fd, CEC_ADAP_G_PHYS_ADDR, &phys) < 0)
        throw sys_error("CEC_ADAP_G_PHYS_ADDR");
    if (phys == CEC_PHYS_ADDR_INVALID) return; // HDMI not yet connected.

    addrs = {};
    addrs.num_log_addrs = 1;
    addrs.cec_version = CEC_OP_CEC_VERSION_2_0;
    addrs.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
    addrs.log_addr_type[0] = CEC_LOG_ADDR_TYPE_PLAYBACK;
    addrs.all_device_types[0] = CEC_OP_ALL_DEVTYPE_PLAYBACK;
    std::snprintf(addrs.osd_name, sizeof(addrs.osd_name), "CM3588 Android");
    // No RC passthrough to kernel's input subsystem: our adapter lacks CAP_RC.
    if (::ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &addrs) < 0 && errno != EBUSY)
        throw sys_error("CEC_ADAP_S_LOG_ADDRS");
    std::puts("CEC playback address registration requested");
}

// Log an invalid CEC physical address once per disconnected period. This is
// particularly useful on dual-HDMI RK3588 hosts where /dev/cec0 can belong to
// HDMI TX0 even while the picture is presented via HDMI TX1.
void warn_if_cec_disconnected(int fd, const char* dev, bool& warned) {
    __u16 physical{};
    if (::ioctl(fd, CEC_ADAP_G_PHYS_ADDR, &physical) < 0)
        throw sys_error("CEC_ADAP_G_PHYS_ADDR");
    if (physical != CEC_PHYS_ADDR_INVALID) {
        warned = false;
        return;
    }
    if (warned) return;
    std::fprintf(stderr,
        "CEC unavailable on %s: physical address f.f.f.f (0xffff); "
        "no TV remote events can be received on this adapter.\n"
        "Check the selected HDMI output and CEC controller mapping: "
        "udevadm info --query=path --name=%s\n",
        dev, dev);
    warned = true;
}

int local_address(int fd) {
    cec_log_addrs addrs{};
    if (::ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &addrs) < 0)
        throw sys_error("CEC_ADAP_G_LOG_ADDRS");
    if (addrs.num_log_addrs && addrs.log_addr[0] < CEC_LOG_ADDR_UNREGISTERED)
        return addrs.log_addr[0];
    return -1;
}

void reply_power_on(int fd, int local, unsigned source) {
    if (local < 0 || source == CEC_LOG_ADDR_UNREGISTERED) return;
    cec_msg reply{};
    reply.len = 3;
    reply.msg[0] = static_cast<__u8>((local << 4) | source);
    reply.msg[1] = CEC_MSG_REPORT_POWER_STATUS;
    reply.msg[2] = CEC_OP_POWER_STATUS_ON;
    // We report 'on' because Android TV is running. A future version
    // could derive this from WayDroid/Weston state.
    if (::ioctl(fd, CEC_TRANSMIT, &reply) < 0 && errno != EAGAIN)
        std::perror("CEC_TRANSMIT power status");
}

void process_message(int fd, const cec_msg& msg, int local, Keyboard& keyboard) {
    if (msg.len < 2) return;  // Ignore CEC poll messages.
    const unsigned source = msg.msg[0] >> 4;
    const unsigned destination = msg.msg[0] & 0x0f;
    if (local < 0 || destination != static_cast<unsigned>(local)) return;

    switch (msg.msg[1]) {
        case CEC_MSG_USER_CONTROL_PRESSED: {
            if (source != CEC_LOG_ADDR_TV || msg.len < 3) return;
            const int key = key_for_cec(msg.msg[2]);
            if (key >= 0) keyboard.press(key);
            break;
        }
        case CEC_MSG_USER_CONTROL_RELEASED:
            if (source == CEC_LOG_ADDR_TV) keyboard.release();
            break;
        case CEC_MSG_GIVE_DEVICE_POWER_STATUS:
            reply_power_on(fd, local, source);
            break;
        default:
            // Standard CEC framework replies and other unsupported requests
            // are left to the kernel's normal (non-passthrough) handling.
            break;
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "Usage: %s [/dev/cec0]\n", argv[0]);
        return 2;
    }
    const char* dev = argc == 2 ? argv[1] : "/dev/cec0";
    struct sigaction sa{};
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    try {
        Keyboard keyboard;
        // Allow udev/Weston a moment to discover the new keyboard.
        ::usleep(500000);
        Fd cec(::open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC));
        if (cec.get() < 0) throw sys_error("open CEC device");

        configure_cec(cec.get());
        __u32 mode = CEC_MODE_INITIATOR | CEC_MODE_EXCL_FOLLOWER;
        if (::ioctl(cec.get(), CEC_S_MODE, &mode) < 0)
            throw sys_error("CEC_S_MODE exclusive follower");

        int local = local_address(cec.get());
        bool warned_disconnected = false;
        warn_if_cec_disconnected(cec.get(), dev, warned_disconnected);
        std::printf("CEC listening: %s, logical address %d\n", dev, local);
        std::fflush(stdout);
        auto next_check = std::chrono::steady_clock::now();
        while (!stopping) {
            pollfd pfd{cec.get(), POLLIN | POLLPRI, 0};
            int ret = ::poll(&pfd, 1, 200);
            if (ret < 0 && errno != EINTR) throw sys_error("poll CEC");
            if (ret > 0 && (pfd.revents & (POLLHUP | POLLNVAL | POLLERR)))
                throw std::runtime_error("CEC adapter hangup/error");

            if (pfd.revents & POLLPRI) {
                while (true) {
                    cec_event event{};
                    if (::ioctl(cec.get(), CEC_DQEVENT, &event) < 0) {
                        if (errno != EAGAIN) throw sys_error("CEC_DQEVENT");
                        break;
                    }
                    if (event.event == CEC_EVENT_STATE_CHANGE) {
                        keyboard.release();
                        local = local_address(cec.get());
                        std::printf("CEC state: physical=0x%04x, LA=%d\n",
                                    event.state_change.phys_addr, local);
                        std::fflush(stdout);
                    } else if (event.event == CEC_EVENT_LOST_MSGS) {
                        keyboard.release();
                        std::fprintf(stderr, "CEC lost %u messages\n",
                                     event.lost_msgs.lost_msgs);
                    }
                }
            }
            if (pfd.revents & POLLIN) {
                while (true) {
                    cec_msg msg{};
                    if (::ioctl(cec.get(), CEC_RECEIVE, &msg) < 0) {
                        if (errno != EAGAIN) throw sys_error("CEC_RECEIVE");
                        break;
                    }
                    process_message(cec.get(), msg, local, keyboard);
                }
            }
            keyboard.check_timeout();
            if (std::chrono::steady_clock::now() >= next_check) {
                configure_cec(cec.get()); // Re-register after HDMI disconnection/reset.
                local = local_address(cec.get());
                warn_if_cec_disconnected(cec.get(), dev, warned_disconnected);
                next_check = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "cm3588-cec-remote: %s\n", ex.what());
        return 1;
    }
}
