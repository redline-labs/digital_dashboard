#ifndef AGENT_CONTROL_CONTROL_SOCKET_H_
#define AGENT_CONTROL_CONTROL_SOCKET_H_

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent_control
{

// A unix-domain socket that speaks newline-delimited JSON.
//
// Runs entirely on its own threads and never touches Qt. That separation is the
// whole point: the accept loop must keep answering while the GUI thread is
// blocked, otherwise the interface goes dark exactly when a hang needs
// diagnosing. Marshalling onto the GUI thread happens above this class, in the
// dispatcher, where it can be given a deadline.
//
// Shape follows apple_usb::UsbmuxdServer (accept thread + one thread per
// client, stop() via shutdown()+close()+join), with the client-thread bookkeeping
// mutex-guarded here because stop() and acceptLoop() touch the same vector.
class ControlSocket
{
  public:
    // Called on a client thread, once per received line. Returns the response
    // line to write back, or an empty string to write nothing (notifications).
    using LineHandler = std::function<std::string(std::string_view)>;

    // Refuse a line longer than this rather than growing without bound; a
    // caller that never sends '\n' would otherwise be an unbounded allocation.
    // Screenshot responses are the large payload here and run well under 8 MB.
    static constexpr std::size_t kMaxLineBytes = 8u * 1024u * 1024u;

    explicit ControlSocket(LineHandler handler);
    ~ControlSocket();

    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    bool start(const std::string& socket_path);
    void stop();

    bool running() const { return run_.load(); }
    const std::string& socketPath() const { return socket_path_; }

  private:
    void acceptLoop();
    void clientLoop(int client_fd);
    void reapFinishedClients();

    LineHandler handler_;
    std::string socket_path_;
    int server_fd_ = -1;
    std::atomic<bool> run_{false};
    std::thread accept_thread_;

    // Each client thread carries a flag it sets on the way out, so finished
    // threads can be joined and dropped on the next accept. Without that, a long
    // session of connect/disconnect cycles leaves a joinable std::thread per
    // past client, each still holding a kernel thread handle.
    struct Client
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    std::mutex clients_mutex_;
    std::vector<Client> clients_;
};

}  // namespace agent_control

#endif  // AGENT_CONTROL_CONTROL_SOCKET_H_
