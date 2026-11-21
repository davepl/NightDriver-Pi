// SPDX-License-Identifier: GPL-3.0-or-later
#include "macethernet.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef __APPLE__
#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_media.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

namespace
{
// Explicit media mappings avoid treating an autoselect or unknown media word
// as evidence of an active gigabit link. Extended media comes from IFXMEDIA.
int LinkSpeedMbps(int media)
{
    switch (IFM_SUBTYPE(media))
    {
        case IFM_1000_SX: case IFM_1000_LX: case IFM_1000_CX:
        case IFM_1000_T: case IFM_1000_CX_SGMII: case IFM_1000_KX:
        case IFM_1000_SGMII:
            return 1000;
        case IFM_2500_T: case IFM_2500_SX: case IFM_2500_KX:
        case IFM_2500_X:
            return 2500;
        case IFM_5000_T: case IFM_5000_KR: case IFM_5000_KR_S:
        case IFM_5000_KR1:
            return 5000;
        case IFM_10G_SR: case IFM_10G_LR: case IFM_10G_CX4:
        case IFM_10G_T: case IFM_10G_KX4: case IFM_10G_KR:
        case IFM_10G_CR1: case IFM_10G_ER: case IFM_10G_TWINAX:
        case IFM_10G_TWINAX_LONG: case IFM_10G_LRM: case IFM_10G_SFI:
        case IFM_10G_AOC:
            return 10000;
        case IFM_20G_KR2:
            return 20000;
        case IFM_25G_PCIE: case IFM_25G_CR: case IFM_25G_KR:
        case IFM_25G_SR: case IFM_25G_LR: case IFM_25G_ACC:
        case IFM_25G_AOC: case IFM_25G_T: case IFM_25G_CR_S:
        case IFM_25G_CR1: case IFM_25G_KR_S: case IFM_25G_AUI:
        case IFM_25G_KR1:
            return 25000;
        case IFM_40G_CR4: case IFM_40G_SR4: case IFM_40G_XLPPI:
        case IFM_40G_LR4: case IFM_40G_KR4: case IFM_40G_XLAUI:
        case IFM_40G_XLAUI_AC: case IFM_40G_ER4:
            return 40000;
        default:
            return 0;
    }
}

class MacEthernetTransport final : public EthernetTransport
{
    int _bpf = -1;
    int _control = -1;
    int _linkSpeed = 0;
    std::string _interface;
    std::chrono::steady_clock::time_point _wireReady{};

    [[noreturn]] void Fail(const std::string & operation, int error) const
    {
        std::string message = "Colorlight " + operation + " on interface '" + _interface + "': " + std::strerror(error);
        if (error == EACCES || error == EPERM)
            message += "; run with sudo or use an account permitted to open /dev/bpf* (see docs/colorlight.md)";
        throw std::runtime_error(message);
    }

    ifreq InterfaceRequest() const
    {
        ifreq request{};
        std::memcpy(request.ifr_name, _interface.c_str(), _interface.size() + 1);
        return request;
    }

    void CheckLink()
    {
        auto request = InterfaceRequest();
        if (ioctl(_control, SIOCGIFFLAGS, &request) < 0)
            Fail("checking interface flags", errno);
        if ((request.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
            throw std::runtime_error("Colorlight interface '" + _interface + "' is down or has no active link");

        ifmediareq media{};
        std::memcpy(media.ifm_name, _interface.c_str(), _interface.size() + 1);
        // XMEDIA preserves extended media types (2.5/5/25 GbE, etc.).
        if (ioctl(_control, SIOCGIFXMEDIA, &media) < 0)
            Fail("checking active Ethernet media", errno);
        if (IFM_TYPE(media.ifm_active) != IFM_ETHER)
            throw std::runtime_error("Colorlight requires a wired Ethernet interface: " + _interface);
        if ((media.ifm_status & (IFM_AVALID | IFM_ACTIVE)) != (IFM_AVALID | IFM_ACTIVE))
            throw std::runtime_error("Colorlight Ethernet link is inactive on '" + _interface + "'");
        _linkSpeed = LinkSpeedMbps(media.ifm_active);
        if (_linkSpeed < 1000 || (media.ifm_active & IFM_FDX) == 0)
            throw std::runtime_error("Colorlight requires a verified active gigabit, full-duplex Ethernet link on '" +
                                     _interface + "'; inspect it with ifconfig " + _interface);
    }

    void OpenBpf()
    {
        int lastError = EBUSY;
        for (int index = 0; index < 1024; ++index)
        {
            const auto path = "/dev/bpf" + std::to_string(index);
            _bpf = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (_bpf >= 0)
                return;
            const int error = errno;
            if (error == EACCES || error == EPERM)
                Fail("opening BPF device", error);
            if (error == ENOENT || error == ENXIO)
                break;
            if (error != EBUSY)
                Fail("opening BPF device", error);
            lastError = error;
        }
        Fail("finding an available BPF device", lastError);
    }

  public:
    explicit MacEthernetTransport(const std::string & interface) : _interface(interface)
    {
        if (interface.empty() || interface.size() >= IFNAMSIZ)
            throw std::runtime_error("Colorlight interface name must contain 1..15 characters");
        if (if_nametoindex(interface.c_str()) == 0)
            Fail("finding interface", errno ? errno : ENODEV);

        _control = socket(AF_INET, SOCK_DGRAM, 0);
        if (_control < 0)
            Fail("opening interface control socket", errno);
        try
        {
            if (fcntl(_control, F_SETFD, FD_CLOEXEC) < 0)
                Fail("setting interface control descriptor flags", errno);
            CheckLink();
            auto request = InterfaceRequest();
            if (ioctl(_control, SIOCGIFMTU, &request) < 0)
                Fail("checking interface MTU", errno);
            if (request.ifr_mtu < 1500)
                throw std::runtime_error("Colorlight requires interface MTU >= 1500: " + interface);

            OpenBpf();
            request = InterfaceRequest();
            if (ioctl(_bpf, BIOCSETIF, &request) < 0)
                Fail("binding BPF to interface", errno);
            u_int linkType = 0;
            if (ioctl(_bpf, BIOCGDLT, &linkType) < 0)
                Fail("checking BPF data link type", errno);
            if (linkType != DLT_EN10MB)
                throw std::runtime_error("Colorlight requires an Ethernet BPF data link: " + interface);

            // Without this flag the output routine replaces our protocol's
            // fixed source MAC. See the macOS bpf(4) BIOCSHDRCMPLT contract.
            u_int completeHeader = 1;
            if (ioctl(_bpf, BIOCSHDRCMPLT, &completeHeader) < 0)
                Fail("preserving complete Ethernet headers", errno);

            // This descriptor only transmits. Drop all captured traffic instead
            // of buffering copies of local or remote Ethernet frames.
            bpf_insn reject[] = {{BPF_RET | BPF_K, 0, 0, 0}};
            bpf_program filter{1, reject};
            if (ioctl(_bpf, BIOCSETF, &filter) < 0)
                Fail("disabling BPF receive capture", errno);
        }
        catch (...)
        {
            if (_bpf >= 0) close(_bpf);
            close(_control);
            throw;
        }
    }

    ~MacEthernetTransport() override
    {
        if (_bpf >= 0) close(_bpf);
        if (_control >= 0) close(_control);
    }

    void Send(std::span<const uint8_t> packet) override
    {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(100);
        for (;;)
        {
            const auto sent = write(_bpf, packet.data(), packet.size());
            if (sent >= 0)
            {
                if (static_cast<size_t>(sent) != packet.size())
                    throw std::runtime_error("Colorlight short BPF Ethernet write on '" + _interface + "'");
                const auto wireTime = nanoseconds(((packet.size() + 24) * 8000 + _linkSpeed - 1) / _linkSpeed);
                _wireReady = std::max(steady_clock::now(), _wireReady) + wireTime;
                return;
            }
            const int error = errno;
            if (error == EINTR && steady_clock::now() < deadline)
                continue;
            if ((error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS) && steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(microseconds(50));
                continue;
            }
            Fail("writing Ethernet frame through BPF", error);
        }
    }

    void FinishRows(std::chrono::microseconds guard) override
    {
        CheckLink();
        // bpf(4) writes accept one complete packet at a time. macOS exposes no
        // BPF transmit-queue drain ioctl; allow projected wire completion plus
        // the receiver guard before enqueueing display/sync packets.
        std::this_thread::sleep_until(std::max(std::chrono::steady_clock::now(), _wireReady) + guard);
    }
};
}
#endif

std::unique_ptr<EthernetTransport> CreateMacColorlightTransport(const std::string & interface)
{
#ifdef __APPLE__
    return std::make_unique<MacEthernetTransport>(interface);
#else
    (void) interface;
    throw std::runtime_error("macOS BPF Ethernet transport is unavailable on this platform");
#endif
}

void ConfigureMacColorlightDisplayThread(std::chrono::microseconds period, size_t packetCount)
{
#ifdef __APPLE__
    if (period.count() <= 0 || packetCount == 0)
        throw std::invalid_argument("Colorlight display timing requires a positive period and a prepared frame");
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.numer || !timebase.denom)
        throw std::runtime_error("Cannot obtain the macOS display timing clock");
    const int qosError = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (qosError)
        throw std::runtime_error("Cannot set macOS display thread QoS: " + std::string(std::strerror(qosError)));
    const uint64_t periodNs = uint64_t(period.count()) * 1000;
    // Reserve only a bounded fraction of one core. The small current display
    // needs about 1 ms; larger packet sets get up to 4 ms, never over 25%.
    const uint64_t computationNs = std::min({periodNs / 4, uint64_t(4000000),
        std::max(uint64_t(1000000), uint64_t(packetCount) * 8000)});
    const uint64_t constraintNs = std::min(periodNs / 2, computationNs + 2000000);
    auto Ticks = [&](uint64_t ns) { return static_cast<uint32_t>(ns * timebase.denom / timebase.numer); };
    thread_time_constraint_policy_data_t policy{};
    policy.period = Ticks(periodNs);
    policy.computation = Ticks(computationNs);
    policy.constraint = Ticks(constraintNs);
    policy.preemptible = TRUE;
    const auto thread = mach_thread_self();
    const auto result = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS)
        throw std::runtime_error("Cannot configure bounded macOS display scheduling (Mach error " + std::to_string(result) + ")");
#else
    (void) period;
    (void) packetCount;
#endif
}
