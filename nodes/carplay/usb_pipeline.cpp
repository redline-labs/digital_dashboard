// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_pipeline.h"

#include "iap2_session.h"

#include "apple_usb/lockdown.h"
#include "apple_usb/muxd.h"
#include "apple_usb/ncm_bridge.h"

#include "airplay/receiver.h"
#include "iap2/mcp2221a_mfi_signer.h"
#include "apple_usb/usb_device.h"
#include "apple_usb/usbmuxd_server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace fs = std::filesystem;

namespace carplay
{
namespace
{

constexpr auto kRediscoverPoll = std::chrono::milliseconds(200);
constexpr auto kRediscoverTimeout = std::chrono::seconds(15);

// How often to look for a phone appearing, and how long to pause after a
// session ends before trying again -- long enough that a phone that fails
// repeatedly does not spin the log, short enough to feel immediate on a replug.
constexpr auto kDevicePoll = std::chrono::milliseconds(500);
constexpr auto kReattachDelay = std::chrono::seconds(2);
constexpr auto kMaxReattachDelay = std::chrono::seconds(30);

std::string shortUdid(const std::string& udid)
{
    return udid.size() > 8 ? udid.substr(0, 8) : udid;
}

VideoCodec toBridgeCodec(airplay::nalu::Codec codec)
{
    return codec == airplay::nalu::Codec::H265 ? VideoCodec::H265 : VideoCodec::H264;
}

// The configuration switch re-enumerates the phone, which invalidates the
// usbfs path captured before it. Everything downstream opens that path, so the
// DeviceInfo has to be re-read afterwards rather than reused.
std::optional<apple_usb::DeviceInfo> rediscover(const std::string& serial)
{
    const auto deadline = std::chrono::steady_clock::now() + kRediscoverTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& device : apple_usb::listAppleDevices())
        {
            if (device.serial == serial)
            {
                return device;
            }
        }
        std::this_thread::sleep_for(kRediscoverPoll);
    }
    return std::nullopt;
}

// Pair records live here, so this has to outlive a reboot: XDG_RUNTIME_DIR is
// tmpfs and gets wiped, which silently re-prompts for trust on the phone at
// every boot. The mux socket is created here too, which is fine -- it is
// unlinked on the way in and out.
std::string defaultStateDir()
{
    if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home != nullptr)
    {
        return (fs::path(data_home) / "carplay").string();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr)
    {
        return (fs::path(home) / ".local" / "share" / "carplay").string();
    }
    return (fs::temp_directory_path() / "carplay").string();
}

// --- Stage 2: device detection and the CarPlay configuration switch ---------
//
// Blocks until an Apple device shows up, or `stop` is set. Polling sysfs rather
// than subscribing to udev keeps this working unprivileged and inside a
// container, and half a second of latency on a plug event is imperceptible.
std::optional<apple_usb::DeviceInfo> waitForDevice(std::atomic<bool>& stop)
{
    bool announced = false;
    while (!stop.load())
    {
        const auto devices = apple_usb::listAppleDevices();
        if (!devices.empty())
        {
            for (const auto& device : devices)
            {
                SPDLOG_INFO("[usb] found {:04x}:{:04x} udid={} at {} (config {})",
                            device.vid, device.pid, device.serial, device.usbfs_path,
                            device.active_configuration);
            }
            if (devices.size() > 1)
            {
                SPDLOG_WARN("[usb] {} Apple devices present; using {}", devices.size(),
                            devices.front().serial);
            }
            return devices.front();
        }

        if (!announced)
        {
            announced = true;
            SPDLOG_INFO("[usb] waiting for a phone. Plug one in, unlocked; on a VM, confirm "
                        "it is attached to the guest.");
        }
        std::this_thread::sleep_for(kDevicePoll);
    }
    return std::nullopt;
}

// Puts an already-detected phone into the CarPlay configuration, re-reading it
// afterwards because the switch re-enumerates it.
std::optional<apple_usb::DeviceInfo> switchToCarPlay(apple_usb::DeviceInfo device)
{
    if (device.active_configuration == apple_usb::kCarPlayConfiguration)
    {
        SPDLOG_INFO("[usb] already in configuration {}", apple_usb::kCarPlayConfiguration);
        return device;
    }

    SPDLOG_INFO("[usb] switching udid={} from configuration {} to {}",
                shortUdid(device.serial), device.active_configuration,
                apple_usb::kCarPlayConfiguration);

    if (!apple_usb::switchToCarPlayConfiguration(device))
    {
        SPDLOG_ERROR("[usb] configuration switch failed for udid={}", shortUdid(device.serial));
        return std::nullopt;
    }

    const auto refreshed = rediscover(device.serial);
    if (!refreshed)
    {
        SPDLOG_ERROR("[usb] udid={} did not come back after the configuration switch. On a "
                     "VM this usually means the hypervisor handed the re-enumerating device "
                     "back to the host.", shortUdid(device.serial));
        return std::nullopt;
    }

    if (refreshed->active_configuration != apple_usb::kCarPlayConfiguration)
    {
        SPDLOG_ERROR("[usb] udid={} is in configuration {}, expected {}. Something "
                     "re-enumerated it -- check that the system usbmuxd is stopped.",
                     shortUdid(refreshed->serial), refreshed->active_configuration,
                     apple_usb::kCarPlayConfiguration);
        return std::nullopt;
    }

    SPDLOG_INFO("[usb] udid={} now in configuration {} at {}",
                shortUdid(refreshed->serial), refreshed->active_configuration,
                refreshed->usbfs_path);
    return refreshed;
}

// Everything the stages below need that outlives an individual phone: the MFi
// coprocessor sits on I2C and is unaffected by a phone coming and going, so it
// is set up once and lent to each session.
struct SessionContext
{
    const UsbPipelineOptions& options;
    std::string state_dir;
    iap2::MfiSigner* mfi_signer = nullptr;
    std::shared_ptr<std::mutex> mfi_mutex;
};

// Runs one attachment of one phone, from claiming the mux through to the iAP2
// session ending. Returns when the phone goes away or `stop` is set; the return
// value says whether the bring-up itself succeeded.
bool runAttachedSession(const apple_usb::DeviceInfo& device, const SessionContext& ctx,
                        ZenohBridge& bridge, std::atomic<bool>& stop)
{
    const UsbPipelineOptions& options = ctx.options;
    const std::string& state_dir = ctx.state_dir;
    std::error_code ec;

    // --- Stage 3: usbmux over the vendor-specific interface, then the socket --
    apple_usb::MuxHost mux(device);
    if (!mux.open())
    {
        SPDLOG_ERROR("[muxd] could not open the mux for udid={}. Another driver may hold "
                     "the interface -- check `lsusb -t` and that the system usbmuxd is "
                     "stopped.", shortUdid(device.serial));
        return false;
    }
    SPDLOG_INFO("[muxd] mux open for udid={}", shortUdid(device.serial));

    // Session-scoped stop: set when the node is shutting down *or* the phone
    // has gone. Everything below waits on this rather than the node-wide flag,
    // so an unplug unwinds one session without ending the process.
    std::atomic<bool> session_stop{false};
    std::thread device_watch([&session_stop, &stop, &mux] {
        while (!session_stop.load())
        {
            if (stop.load() || !mux.alive())
            {
                session_stop.store(true);
                break;
            }
            std::this_thread::sleep_for(kDevicePoll);
        }
    });
    // Any exit from here on has to release the watchdog before returning.
    const auto finish = [&session_stop, &device_watch](bool result) {
        session_stop.store(true);
        if (device_watch.joinable())
        {
            device_watch.join();
        }
        return result;
    };

    const std::string socket_path =
        (fs::path(state_dir) / ("usbmuxd-" + shortUdid(device.serial) + ".sock")).string();
    fs::remove(socket_path, ec);

    apple_usb::UsbmuxdServer usbmuxd(mux, socket_path, state_dir);
    if (!usbmuxd.start())
    {
        SPDLOG_ERROR("[usbmuxd] could not serve on {}", socket_path);
        mux.close();
        return finish(false);
    }
    SPDLOG_INFO("[usbmuxd] serving {} on {}", shortUdid(device.serial), socket_path);
    SPDLOG_INFO("[usbmuxd] sanity-check it independently with:");
    SPDLOG_INFO("[usbmuxd]   USBMUXD_SOCKET_ADDRESS=UNIX:{} idevice_id -l", socket_path);

    bool ok = true;
    std::unique_ptr<apple_usb::CarkitChannel> carkit;

    // --- Stage 4: lockdown pairing + the carkit TLS channel -------------------
    if (options.max_stage >= 4)
    {
        // The trust prompt has no deadline; abort only if the node is stopping
        // or the phone was pulled out while we waited.
        carkit = apple_usb::openCarkitChannel(device.serial, socket_path, state_dir,
                                              [&session_stop] { return session_stop.load(); });
        if (!carkit)
        {
            if (!session_stop.load())
            {
                SPDLOG_ERROR("[carkit] could not open the carkit channel for udid={}. "
                             "Delete {} to force a fresh pairing.",
                             shortUdid(device.serial), state_dir);
            }
            ok = false;
        }
        else
        {
            SPDLOG_INFO("[carkit] carkit TLS channel up (iAP2) udid={}",
                        shortUdid(device.serial));
        }
    }
    else
    {
        SPDLOG_INFO("[node] stopping after stage 3 as requested");
    }

    // --- Stage 6: NCM <-> TAP link -------------------------------------------
    //
    // Brought up *before* the iAP2 session, not after: the phone asks for the
    // accessory endpoint moments after authentication, and the address only
    // exists once this bridge is running.
    std::unique_ptr<apple_usb::NcmBridge> ncm;
    if (ok && options.max_stage >= 6)
    {
        ncm = std::make_unique<apple_usb::NcmBridge>(device);
        if (!ncm->start())
        {
            SPDLOG_ERROR("[ncm] bridge did not start. Needs CAP_NET_ADMIN for the TAP device "
                         "(setcap cap_net_admin+ep on this binary, or run as root).");
            ncm.reset();
            ok = false;
        }
        else
        {
            SPDLOG_INFO("[ncm] {} up, accessory link-local {}", ncm->interfaceName(),
                        ncm->linkLocalAddress());
        }
    }

    // --- Stage 7: the AirPlay RTSP receiver ----------------------------------
    //
    // Started before the iAP2 session for the same reason the NCM bridge is:
    // the phone dials port 7000 within milliseconds of CarPlayStartSession, and
    // anything not listening by then just gets connection-refused.
    // One coprocessor, two consumers: iAP2 authentication and AirPlay
    // /auth-setup. It sits on a single I2C bus, and the two run on different
    // threads, so access is serialised. It is owned by the caller and shared by
    // every session, since it has nothing to do with the phone.
    auto mfi_mutex = ctx.mfi_mutex;

    std::unique_ptr<airplay::Receiver> receiver;
    if (ok && options.max_stage >= 7)
    {
        airplay::ReceiverConfig receiver_config;
        if (ctx.mfi_signer != nullptr)
        {
            iap2::MfiSigner* signer = ctx.mfi_signer;
            receiver_config.mfi_certificate = [signer, mfi_mutex]() -> std::vector<uint8_t> {
                std::lock_guard<std::mutex> lock(*mfi_mutex);
                return signer->certificate().value_or(std::vector<uint8_t>{});
            };
            receiver_config.mfi_sign =
                [signer, mfi_mutex](const std::vector<uint8_t>& digest) -> std::vector<uint8_t> {
                std::lock_guard<std::mutex> lock(*mfi_mutex);
                return signer->signChallenge(digest).value_or(std::vector<uint8_t>{});
            };
            receiver_config.mfi_protocol_major = [signer, mfi_mutex]() {
                std::lock_guard<std::mutex> lock(*mfi_mutex);
                return signer->protocolMajor();
            };
        }
        receiver = std::make_unique<airplay::Receiver>(receiver_config);

        // Hand decoded access units straight to the dashboard. The parameter
        // sets are cached and re-sent ahead of every keyframe because zenoh has
        // no retained messages: a widget that starts late would otherwise never
        // sync.
        auto parameter_sets = std::make_shared<std::vector<uint8_t>>();
        receiver->setVideoHandler([&bridge, parameter_sets,
                                   &receiver_config](const airplay::VideoPacket& packet) {
            if (packet.is_config)
            {
                // Publish the parameter sets as their own message (the widget
                // caches them and prepends to the next access unit) and keep a
                // copy so we can republish before every keyframe -- zenoh has no
                // retained messages, so a widget that starts or restarts later
                // must see config again to sync.
                *parameter_sets = packet.data;

                VideoFrame config;
                config.codec = toBridgeCodec(packet.codec);
                config.is_config = true;
                config.width_px = static_cast<uint16_t>(receiver_config.width);
                config.height_px = static_cast<uint16_t>(receiver_config.height);
                config.data = packet.data.data();
                config.len = packet.data.size();
                bridge.publishVideo(config);
                return;
            }

            // Republish parameter sets immediately before each keyframe.
            if (packet.keyframe && !parameter_sets->empty())
            {
                VideoFrame config;
                config.codec = toBridgeCodec(packet.codec);
                config.is_config = true;
                config.width_px = static_cast<uint16_t>(receiver_config.width);
                config.height_px = static_cast<uint16_t>(receiver_config.height);
                config.data = parameter_sets->data();
                config.len = parameter_sets->size();
                bridge.publishVideo(config);
            }

            VideoFrame frame;
            frame.codec = toBridgeCodec(packet.codec);
            frame.is_keyframe = packet.keyframe;
            frame.width_px = static_cast<uint16_t>(receiver_config.width);
            frame.height_px = static_cast<uint16_t>(receiver_config.height);
            frame.data = packet.data.data();
            frame.len = packet.data.size();
            bridge.publishVideo(frame);
        });

        // Publish decoded PCM audio onto zenoh for the widget's QAudioSink.
        receiver->setAudioHandler([&bridge](const airplay::AudioPacket& packet) {
            AudioChunk chunk;
            chunk.sample_rate_hz = packet.sample_rate;
            chunk.channels = packet.channels;
            // Map the CarPlay audioType category onto the dashboard's stream
            // classes. Everything unrecognised is treated as a nav/alert prompt.
            if (packet.audio_type == "media")
            {
                chunk.stream = AudioStream::Music;
            }
            else if (packet.audio_type == "telephony")
            {
                chunk.stream = AudioStream::Call;
            }
            else if (packet.audio_type == "speechRecognition")
            {
                chunk.stream = AudioStream::Siri;
            }
            else
            {
                chunk.stream = AudioStream::NavPrompt;
            }
            chunk.pcm = packet.data.data();
            chunk.len = packet.data.size();
            bridge.publishAudio(chunk);
        });

        if (!receiver->start())
        {
            SPDLOG_ERROR("[airplay] receiver did not start");
            receiver.reset();
            ok = false;
        }
        else
        {
            // Session state combines the recording status and the mic status,
            // both of which change independently; keep the current values in a
            // shared struct so either handler can publish the whole picture.
            const auto config = receiver_config;
            std::atomic<bool>* recording_flag = options.recording;
            struct SessionShare
            {
                std::mutex mutex;
                bool recording = false;
                bool mic_active = false;
                uint32_t mic_rate = 0;
                uint8_t mic_channels = 0;
            };
            auto share = std::make_shared<SessionShare>();
            const auto publish_session = [&bridge, config, share]() {
                std::lock_guard<std::mutex> lock(share->mutex);
                SessionState state;
                state.device_connected = share->recording;
                state.phase = share->recording ? SessionPhase::Recording : SessionPhase::Idle;
                state.main_width_px = static_cast<uint16_t>(config.width);
                state.main_height_px = static_cast<uint16_t>(config.height);
                state.mic_active = share->mic_active;
                state.mic_sample_rate_hz = share->mic_rate;
                state.mic_channels = share->mic_channels;
                bridge.publishSession(state);
            };

            receiver->setStatusHandler([recording_flag, share, publish_session](bool recording) {
                if (recording_flag != nullptr)
                {
                    recording_flag->store(recording);
                }
                {
                    std::lock_guard<std::mutex> lock(share->mutex);
                    share->recording = recording;
                }
                publish_session();
            });

            // Mic uplink: when the phone opens a mic stream, tell the widget to
            // start capturing (via session mic_active); the captured PCM comes
            // back on the mic topic and is fed to the receiver's uplink below.
            receiver->setMicStatusHandler(
                [share, publish_session](bool active, uint32_t rate, uint8_t channels) {
                    {
                        std::lock_guard<std::mutex> lock(share->mutex);
                        share->mic_active = active;
                        share->mic_rate = rate;
                        share->mic_channels = channels;
                    }
                    SPDLOG_INFO("[node] microphone {} ({} Hz, {} ch)",
                                active ? "requested" : "released", rate, channels);
                    publish_session();
                });

            airplay::Receiver* rx = receiver.get();

            // Captured mic PCM from the dashboard -> the phone.
            bridge.setMicHandler([rx](const AudioChunk& chunk) {
                if (chunk.pcm != nullptr && chunk.len > 0)
                {
                    rx->feedMic(std::vector<uint8_t>(chunk.pcm, chunk.pcm + chunk.len));
                }
            });

            // Route the dashboard's touch events to the phone over the event
            // channel. The widget reports x/y in 0..10000 across its area; the
            // receiver wants 0..1.
            bridge.setInputHandler([rx](const InputEvent& event) {
                switch (event.kind)
                {
                    case InputEvent::Kind::TouchDown:
                    case InputEvent::Kind::TouchMove:
                        rx->sendTouch(event.x / 10000.0f, event.y / 10000.0f, true);
                        break;
                    case InputEvent::Kind::TouchUp:
                        rx->sendTouch(event.x / 10000.0f, event.y / 10000.0f, false);
                        break;
                    default:
                        break;
                }
            });
        }
    }

    // --- Stage 5: iAP2 link layer, identification, MFi auth ------------------
    if (ok && carkit && options.max_stage >= 5)
    {
        Iap2SessionOptions iap2_options;
        iap2_options.allow_missing_mfi = options.allow_missing_mfi;
        iap2_options.signer = ctx.mfi_signer;

        if (ncm)
        {
            apple_usb::NcmBridge* bridge = ncm.get();
            iap2_options.endpoint_provider =
                [bridge]() -> std::optional<Iap2SessionOptions::Endpoint> {
                if (!bridge->running() || bridge->linkLocalAddress().empty())
                {
                    return std::nullopt;
                }
                Iap2SessionOptions::Endpoint endpoint;
                endpoint.link_local_address = bridge->linkLocalAddress();
                endpoint.device_identifier = bridge->hostMac();
                return endpoint;
            };
        }

        // Accumulate now-playing metadata: the phone sends partial updates (a
        // track change carries title/artist/album; a tick may carry only
        // elapsed), so merge each into a persistent state before publishing.
        auto now_playing = std::make_shared<NowPlaying>();
        auto now_playing_mutex = std::make_shared<std::mutex>();
        auto now_playing_valid = std::make_shared<std::atomic<bool>>(false);
        auto now_playing_last_log = std::make_shared<std::string>();
        iap2_options.now_playing_handler = [&bridge, now_playing, now_playing_mutex,
                                            now_playing_valid,
                                            now_playing_last_log](const iap2::NowPlaying& update) {
            std::lock_guard<std::mutex> lock(*now_playing_mutex);
            NowPlaying& np = *now_playing;
            if (update.title)
            {
                np.title = *update.title;
            }
            if (update.artist)
            {
                np.artist = *update.artist;
            }
            if (update.album)
            {
                np.album = *update.album;
            }
            if (update.app_name)
            {
                np.app = *update.app_name;
            }
            if (update.duration_ms)
            {
                np.duration_sec = static_cast<float>(*update.duration_ms) / 1000.0f;
            }
            if (update.elapsed_ms)
            {
                np.elapsed_sec = static_cast<float>(*update.elapsed_ms) / 1000.0f;
            }
            if (update.status)
            {
                np.playing = (*update.status == iap2::PlaybackStatus::kPlaying);
            }
            now_playing_valid->store(true);

            // Announce the track at INFO only when what a user would see
            // actually changes; the phone re-sends the same state ~2/s.
            const std::string signature =
                np.title + '\x1f' + np.artist + '\x1f' + (np.playing ? "1" : "0");
            if (signature != *now_playing_last_log)
            {
                *now_playing_last_log = signature;
                SPDLOG_INFO("[node] now playing: '{}' / '{}' ({})", np.title, np.artist,
                            np.playing ? "playing" : "paused");
            }
            bridge.publishNowPlaying(np);
        };

        // Album artwork arrives asynchronously as a file transfer after a track
        // change. Fold it into the accumulated state and bump the sequence so
        // the widget knows to refresh the image.
        iap2_options.artwork_handler = [&bridge, now_playing, now_playing_mutex,
                                        now_playing_valid](const std::vector<uint8_t>& image) {
            std::lock_guard<std::mutex> lock(*now_playing_mutex);
            NowPlaying& np = *now_playing;
            np.album_art = image;
            ++np.album_art_seq;
            now_playing_valid->store(true);
            bridge.publishNowPlaying(np);
        };

        // Navigation: guidance and maneuver updates are already merged into one
        // iap2::NavGuidance by the session; map it onto the bridge's struct.
        auto nav_state = std::make_shared<NavGuidance>();
        auto nav_mutex = std::make_shared<std::mutex>();
        auto nav_valid = std::make_shared<std::atomic<bool>>(false);
        iap2_options.nav_handler = [&bridge, nav_state, nav_mutex,
                                    nav_valid](const iap2::NavGuidance& g) {
            std::lock_guard<std::mutex> lock(*nav_mutex);
            NavGuidance& nav = *nav_state;
            // A non-zero route-guidance state means guidance is active.
            nav.active = g.status.value_or(0) != 0;
            if (g.road_name)
            {
                nav.road_name = *g.road_name;
            }
            if (g.after_road_name)
            {
                nav.after_road_name = *g.after_road_name;
            }
            if (g.destination_name)
            {
                nav.destination_name = *g.destination_name;
            }
            if (g.maneuver_type)
            {
                nav.maneuver_type = *g.maneuver_type;
            }
            if (g.turn_angle)
            {
                nav.maneuver_angle_deg = *g.turn_angle;
            }
            if (g.junction_type)
            {
                nav.junction_type = *g.junction_type;
            }
            // iap2::NavGuidance names these confusingly:
            //   distance_to_destination = total distance remaining
            //   remain_distance         = distance to the next maneuver
            if (g.distance_to_destination)
            {
                nav.distance_remaining_m = static_cast<float>(*g.distance_to_destination);
            }
            if (g.remain_distance)
            {
                nav.distance_to_maneuver_m = static_cast<float>(*g.remain_distance);
            }
            if (g.time_to_destination)
            {
                nav.time_remaining_sec = static_cast<float>(*g.time_to_destination);
            }
            if (g.eta_epoch)
            {
                nav.eta_epoch_sec = *g.eta_epoch;
            }
            nav_valid->store(true);
            SPDLOG_DEBUG("[node] nav publish: active={} road='{}' dest='{}' toManeuver={}m "
                         "remain={}m eta_in={}s",
                         nav.active, nav.road_name, nav.destination_name,
                         nav.distance_to_maneuver_m, nav.distance_remaining_m,
                         nav.time_remaining_sec);
            bridge.publishNav(nav);
        };

        // Call state: the session's CallTracker already folds per-call updates
        // into a single phase; map that phase onto the bridge's enum.
        auto call_valid = std::make_shared<std::atomic<bool>>(false);
        auto last_call = std::make_shared<CallState>();
        auto call_mutex = std::make_shared<std::mutex>();
        iap2_options.call_handler = [&bridge, call_valid, last_call,
                                     call_mutex](const iap2::CallTracker& tracker) {
            std::lock_guard<std::mutex> lock(*call_mutex);
            CallState& call = *last_call;
            switch (tracker.phase())
            {
                case iap2::CallTracker::Phase::kActive: call.phase = CallPhase::Active; break;
                case iap2::CallTracker::Phase::kRinging: call.phase = CallPhase::Incoming; break;
                case iap2::CallTracker::Phase::kEnded: call.phase = CallPhase::Idle; break;
            }
            call.remote_name = tracker.name();
            call.remote_number = tracker.number();
            call_valid->store(true);
            bridge.publishCall(call);
        };

        // GPS location the phone can dead-reckon from. A GPS source publishes
        // fixes on nodes/carplay/location; cache the latest and hand it to the
        // session, which uplinks NMEA while the phone is asking for location. A
        // --location fix, when given, wins over anything published.
        auto latest_fix = std::make_shared<LocationFix>();
        auto fix_mutex = std::make_shared<std::mutex>();
        auto fix_valid = std::make_shared<std::atomic<bool>>(false);
        if (options.static_location)
        {
            std::lock_guard<std::mutex> lock(*fix_mutex);
            *latest_fix = *options.static_location;
            fix_valid->store(true);
        }
        else
        {
            bridge.setLocationHandler([latest_fix, fix_mutex, fix_valid](const LocationFix& fix) {
                std::lock_guard<std::mutex> lock(*fix_mutex);
                *latest_fix = fix;
                fix_valid->store(true);
            });
        }
        iap2_options.location_provider =
            [latest_fix, fix_mutex, fix_valid]() -> std::optional<iap2::LocationFix> {
            if (!fix_valid->load())
            {
                return std::nullopt;  // no GPS source has published yet
            }
            std::lock_guard<std::mutex> lock(*fix_mutex);
            iap2::LocationFix out;
            out.latitude_deg = latest_fix->latitude_deg;
            out.longitude_deg = latest_fix->longitude_deg;
            out.altitude_m = latest_fix->altitude_m;
            out.speed_knots = latest_fix->speed_knots;
            out.course_deg = latest_fix->course_deg;
            out.satellites = latest_fix->satellites;
            out.hdop = latest_fix->hdop;
            out.utc_epoch_ms = latest_fix->utc_epoch_ms;
            out.valid = latest_fix->valid;
            return out;
        };

        // zenoh has no retained messages, so re-publish the last-known metadata
        // periodically -- otherwise a dashboard that connects between updates
        // (a paused track, a steady navigation screen) shows nothing.
        std::thread metadata_republish([&bridge, now_playing, now_playing_mutex, now_playing_valid,
                                        nav_state, nav_mutex, nav_valid, last_call, call_mutex,
                                        call_valid, &session_stop]() {
            while (!session_stop.load())
            {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (now_playing_valid->load())
                {
                    std::lock_guard<std::mutex> lock(*now_playing_mutex);
                    bridge.publishNowPlaying(*now_playing);
                }
                if (nav_valid->load())
                {
                    std::lock_guard<std::mutex> lock(*nav_mutex);
                    bridge.publishNav(*nav_state);
                }
                if (call_valid->load())
                {
                    std::lock_guard<std::mutex> lock(*call_mutex);
                    bridge.publishCall(*last_call);
                }
            }
        });

        if (!runIap2Session(*carkit, iap2_options, session_stop))
        {
            SPDLOG_ERROR("[iap2] session did not complete");
            ok = false;
        }

        session_stop.store(true);  // release the republish thread if the session ended
        metadata_republish.join();
    }

    // Hold the session open so the sockets above can be poked at from another
    // terminal. A failed bring-up falls straight through instead: the caller
    // wants to back off and retry, not sit on a half-open session.
    while (ok && !session_stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    SPDLOG_INFO("[node] tearing down the USB pipeline");
    // Detach the bridge callbacks that capture the receiver before it is
    // destroyed, so a late zenoh mic/input delivery cannot call into a corpse.
    bridge.setMicHandler(nullptr);
    bridge.setInputHandler(nullptr);
    bridge.setLocationHandler(nullptr);
    if (receiver)
    {
        receiver->stop();
    }
    if (ncm)
    {
        ncm->stop();
    }
    if (carkit)
    {
        carkit->close();
    }
    usbmuxd.stop();
    mux.close();
    fs::remove(socket_path, ec);
    return finish(ok);
}

}  // namespace

bool runUsbPipeline(const UsbPipelineOptions& options, ZenohBridge& bridge,
                    std::atomic<bool>& stop)
{
    const std::string state_dir =
        options.state_dir.empty() ? defaultStateDir() : options.state_dir;

    std::error_code ec;
    fs::create_directories(state_dir, ec);
    if (ec)
    {
        SPDLOG_ERROR("[node] cannot create state dir {}: {}", state_dir, ec.message());
        return false;
    }
    SPDLOG_INFO("[node] state dir {}", state_dir);

    // The coprocessor is on I2C, not USB, so it is initialised once and outlives
    // every phone that comes and goes below.
    auto mfi_signer = std::make_unique<iap2::Mcp2221aMfiSigner>();
    if (!mfi_signer->init())
    {
        SPDLOG_WARN("[mfi] coprocessor unavailable");
        mfi_signer.reset();
    }

    SessionContext ctx{options, state_dir, mfi_signer.get(), std::make_shared<std::mutex>()};

    // Supervisor loop: one pass per phone attachment. A phone can be unplugged
    // and plugged back in as many times as the user likes -- each replug starts
    // a fresh mux, socket, and iAP2 session, because none of that state
    // survives the re-enumeration.
    bool ever_ok = false;
    auto retry_delay = kReattachDelay;
    while (!stop.load())
    {
        const auto found = waitForDevice(stop);
        if (!found)
        {
            break;  // stop was set while waiting
        }

        if (options.max_stage < 3)
        {
            SPDLOG_INFO("[node] stopping after stage 2 as requested");
            return switchToCarPlay(*found).has_value();
        }

        bool attached = false;
        if (const auto device = switchToCarPlay(*found))
        {
            attached = runAttachedSession(*device, ctx, bridge, stop);
            ever_ok |= attached;
        }
        if (stop.load())
        {
            break;
        }

        // Tell the dashboard the phone is gone, rather than leaving the last
        // frame and track up. The recording flag has to be cleared too or the
        // idle publisher in main() keeps deferring to a session that has ended.
        if (options.recording != nullptr)
        {
            options.recording->store(false);
        }
        bridge.publishSession(SessionState{});

        // Back off when the same phone keeps failing -- a phone whose owner
        // tapped "Don't Trust" would otherwise re-run the whole bring-up every
        // two seconds forever. A clean attach, or the phone being unplugged,
        // resets the delay so a genuine replug is picked up immediately.
        if (attached || apple_usb::listAppleDevices().empty())
        {
            retry_delay = kReattachDelay;
        }
        else
        {
            retry_delay = std::min(retry_delay * 2, kMaxReattachDelay);
            SPDLOG_WARN("[node] bring-up failed with the phone still attached; retrying in {}s",
                        retry_delay.count());
        }

        SPDLOG_INFO("[node] session ended; waiting for a phone to be plugged in");
        for (auto waited = std::chrono::seconds(0); waited < retry_delay && !stop.load();
             waited += std::chrono::seconds(1))
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    SPDLOG_INFO("[node] USB pipeline stopped");
    return ever_ok;
}

}  // namespace carplay
