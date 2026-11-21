#include "socketserver.h"
#include <cassert>
#include <csignal>
#include <future>
#include <iostream>
#include <vector>

std::atomic<std::sig_atomic_t> interrupt_received{0};

static std::vector<uint8_t> Frame(uint32_t pixels = 2, uint16_t channel = 0)
{
    std::vector<uint8_t> bytes(24 + 3 * pixels);
    bytes[0] = WIFI_COMMAND_PIXELDATA64;
    bytes[2] = channel & 255;
    bytes[3] = channel >> 8;
    for (unsigned i = 0; i < 4; ++i) bytes[4 + i] = (pixels >> (8 * i)) & 255;
    for (size_t i = 24; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 71);
    return bytes;
}

static std::vector<uint8_t> Compressed(std::span<const uint8_t> bytes)
{
    std::vector<uint8_t> result(16 + compressBound(bytes.size()));
    uLongf size = result.size() - 16;
    assert(compress(result.data() + 16, &size, bytes.data(), bytes.size()) == Z_OK);
    for (unsigned i = 0; i < 4; ++i)
    {
        result[i] = (uint32_t(COMPRESSED_HEADER_TAG) >> (8 * i)) & 255;
        result[4 + i] = (uint32_t(size) >> (8 * i)) & 255;
        result[8 + i] = (uint32_t(bytes.size()) >> (8 * i)) & 255;
    }
    result.resize(16 + size);
    return result;
}

static int Connect(const SocketServer & server)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    timeval timeout{2, 0};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.Port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    return fd;
}

static void Send(int fd, std::span<const uint8_t> bytes)
{
    while (!bytes.empty())
    {
        const auto count = write(fd, bytes.data(), bytes.size());
        assert(count > 0);
        bytes = bytes.subspan(count);
    }
}

static SocketResponse Response(int fd)
{
    SocketResponse response{};
    auto bytes = std::span(reinterpret_cast<uint8_t *>(&response), sizeof(response));
    while (!bytes.empty())
    {
        const auto count = read(fd, bytes.data(), bytes.size());
        assert(count > 0);
        bytes = bytes.subspan(count);
    }
    return response;
}

class ScheduledOutput : public DisplayOutput
{
  public:
    double presentedAt = 0;
    int refreshes = 0;
    int Width() const override { return 2; }
    int Height() const override { return 1; }
    std::chrono::microseconds RefreshInterval() const override { return std::chrono::milliseconds(5); }
    void Refresh() override { ++refreshes; }
    void Present(std::span<const CRGB> frame) override
    {
        assert(frame.size() == 2 && frame[0].r == 1 && frame[1].b == 6);
        presentedAt = CAppTime::CurrentTime();
        interrupt_received = 1;
    }
};

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    for (size_t invalid : {size_t(0), std::numeric_limits<size_t>::max(), size_t(std::numeric_limits<uint32_t>::max())})
    {
        bool rejected = false;
        try { SocketServer invalidServer(0, invalid); } catch (const std::invalid_argument &) { rejected = true; }
        assert(rejected);
    }
    LEDBufferManager buffers(5);
    auto valid = Frame();
    assert(SocketServer::ProcessIncomingData(buffers, valid, 2));
    assert(buffers.Size() == 1);
    auto buffer = buffers.PopOldestBuffer();
    assert(buffer && buffer.value()->ColorData().size() == 2);
    assert(buffer.value()->ColorData()[0].r == valid[24]);
    assert(buffer.value()->ColorData()[1].b == valid[29]);
    for (uint16_t channel : {0, 1, 3, 257})
        assert(SocketServer::ProcessIncomingData(buffers, Frame(2, channel), 2));
    assert(!SocketServer::ProcessIncomingData(buffers, Frame(2, 2), 2));
    for (size_t size = 0; size < valid.size(); ++size)
        assert(!SocketServer::ProcessIncomingData(buffers, std::span(valid).first(size), 2));
    auto extra = valid;
    extra.push_back(0);
    assert(!SocketServer::ProcessIncomingData(buffers, extra, 2));
    assert(!SocketServer::ProcessIncomingData(buffers, valid, 1));
    auto huge = valid;
    for (int i = 4; i < 8; ++i) huge[i] = 255;
    assert(!SocketServer::ProcessIncomingData(buffers, huge, 2));

    auto compressed = Compressed(valid);
    std::vector<uint8_t> expanded(valid.size());
    assert(SocketServer::DecompressBuffer(std::span(compressed).subspan(16), expanded));
    assert(expanded == valid);
    auto truncated = compressed;
    truncated.pop_back();
    assert(!SocketServer::DecompressBuffer(std::span(truncated).subspan(16), expanded));
    std::vector<uint8_t> tooSmall(valid.size() - 1);
    assert(!SocketServer::DecompressBuffer(std::span(compressed).subspan(16), tooSmall));
    compressed.push_back(0);
    assert(!SocketServer::DecompressBuffer(std::span(compressed).subspan(16), expanded));
    assert(!SocketServer::DecompressBuffer({}, expanded));

    // Exercise actual TCP reception/status and both shutdown states. Tiny
    // compressed frames are larger than raw frames, as produced by NDSCPP.
    for (bool activeClient : {false, true})
    {
        LEDBufferManager networkBuffers(5);
        SocketServer server(0, 2);
        assert(server.begin());
        auto worker = std::async(std::launch::async, [&] { return server.ProcessIncomingConnectionsLoop(networkBuffers); });
        int fd = -1;
        if (activeClient)
        {
            fd = Connect(server);
            Send(fd, valid);
            assert(Response(fd).size == sizeof(SocketResponse));
            const auto onWire = Compressed(valid);
            assert(onWire.size() > valid.size());
            Send(fd, onWire);
            auto response = Response(fd);
            assert(response.bufferPos == 2 && response.bufferSize == 5);
            assert(networkBuffers.PopOldestBuffer().value()->ColorData()[0].r == valid[24]);
            assert(networkBuffers.PopOldestBuffer().value()->ColorData()[1].b == valid[29]);
            // Leave an incomplete header pending when shutdown begins.
            Send(fd, std::span(valid).first(1));
        }
        // Let the worker actually block in accept/read before requesting stop.
        // Without this, an idle-server test can pass by stopping before it runs.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const auto stopped = std::chrono::steady_clock::now();
        server.end();
        assert(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        assert(worker.get());
        assert(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(2));
        if (fd >= 0) close(fd);
    }

    // Preparation selects against the future latch time, atomically leaving
    // later frames queued. Its earlier wake-up time must not shift selection.
    {
        LEDBufferManager deadlines(5);
        const CRGB a[] = {{10,0,0}}, b[] = {{20,0,0}}, c[] = {{30,0,0}};
        deadlines.PushNewBuffer(std::make_unique<LEDBuffer>(a,1,100,0));
        deadlines.PushNewBuffer(std::make_unique<LEDBuffer>(b,1,101,0));
        deadlines.PushNewBuffer(std::make_unique<LEDBuffer>(c,1,102,0));
        auto newest = deadlines.PopNewestBufferDueAt(101.5);
        assert(newest && newest->ColorData()[0].r==20 && deadlines.Size()==1);
        assert(!deadlines.PopNewestBufferDueAt(101.9));
        assert(deadlines.PopNewestBufferDueAt(102.0)->ColorData()[0].r==30);
    }

    const double due = CAppTime::CurrentTime() + 0.08;
    const auto seconds = uint64_t(due);
    const auto micros = uint64_t((due - seconds) * MICROS_PER_SECOND);
    const CRGB colors[] = {{1, 2, 3}, {4, 5, 6}};
    LEDBufferManager scheduled(2);
    scheduled.PushNewBuffer(std::make_unique<LEDBuffer>(colors, 2, seconds, micros));
    ScheduledOutput output;
    MatrixDraw::RunDrawLoop(scheduled, output);
    assert(output.presentedAt >= due - 0.001);
    assert(output.refreshes >= 2); // Idle refresh runs while a future frame waits.
    // A backlog must produce only the newest due image at a display tick.
    interrupt_received = 0;
    LEDBufferManager backlog(10);
    const CRGB obsolete[] = {{99, 0, 0}, {99, 0, 0}};
    for (int i = 0; i < 8; ++i)
        backlog.PushNewBuffer(std::make_unique<LEDBuffer>(obsolete, 2, 0, i));
    backlog.PushNewBuffer(std::make_unique<LEDBuffer>(colors, 2, 0, 9));
    ScheduledOutput coalesced;
    MatrixDraw::RunDrawLoop(backlog, coalesced);
    assert(backlog.Size() == 0 && coalesced.presentedAt > 0);
    std::cout << "input, compression, scheduling and shutdown tests passed\n";
}
