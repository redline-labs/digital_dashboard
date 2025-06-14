#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>
#include <string>
#include <csignal>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include "zenoh.hxx"

class SpeedPublisher {
private:
    std::unique_ptr<zenoh::Session> mSession;
    std::unique_ptr<zenoh::Publisher> mPublisher;
    bool mRunning;
    
    // Speed sweep parameters
    static constexpr double MAX_SPEED_MPS = 30.0;  // Maximum speed in m/s
    static constexpr double PUBLISH_RATE_HZ = 20.0; // 10 Hz
    static constexpr double SWEEP_PERIOD_SEC = 6.0; // Complete cycle every 6 seconds
    
public:
    SpeedPublisher() : mRunning(false) {}
    
    ~SpeedPublisher() {
        stop();
    }
    
    bool initialize() {
        try {
            // Create Zenoh configuration
            auto config = zenoh::Config::create_default();
            
            // Open Zenoh session
            mSession = std::make_unique<zenoh::Session>(zenoh::Session::open(std::move(config)));
            SPDLOG_INFO("Zenoh session opened successfully");
            
            // Create publisher for vehicle speed
            auto key_expr = zenoh::KeyExpr("vehicle/speed_mps");
            mPublisher = std::make_unique<zenoh::Publisher>(
                mSession->declare_publisher(key_expr)
            );
            
            SPDLOG_INFO("Zenoh speed publisher created for key: vehicle/speed_mps");
            return true;
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to initialize Zenoh: {}", e.what());
            return false;
        }
    }
    
    void start() {
        if (!mSession || !mPublisher) {
            SPDLOG_ERROR("Publisher not initialized. Call initialize() first.");
            return;
        }
        
        mRunning = true;
        SPDLOG_INFO("Starting speed publisher at {} Hz", PUBLISH_RATE_HZ);
        SPDLOG_INFO("Speed will sweep from 0 to {} m/s over {} seconds", MAX_SPEED_MPS, SWEEP_PERIOD_SEC);
        
        const auto sleep_duration = std::chrono::milliseconds(
            static_cast<int>(1000.0 / PUBLISH_RATE_HZ)
        );
        
        auto start_time = std::chrono::steady_clock::now();
        
        while (mRunning) {
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - start_time
            ).count() / 1000.0; // Convert to seconds
            
            // Calculate speed using a sine wave for smooth sweeping
            // This creates a pattern: 0 -> 30 -> 0 -> 30 -> 0 over SWEEP_PERIOD_SEC
            double phase = (elapsed / SWEEP_PERIOD_SEC) * 2.0 * M_PI;
            double speed_mps = (MAX_SPEED_MPS / 2.0) * (1.0 + std::sin(phase));
            
            // Publish the speed value
            publishSpeed(speed_mps);
            
            // Sleep to maintain the desired publish rate
            std::this_thread::sleep_for(sleep_duration);
        }
        
        SPDLOG_INFO("Speed publisher stopped");
    }
    
    void stop() {
        mRunning = false;
    }
    
private:
    void publishSpeed(double speed_mps) {
        try {
            // Convert speed to string
            std::string speed_str = std::to_string(speed_mps);
            
            // Create payload from string
            auto payload = zenoh::Bytes(speed_str);
            
            // Publish the data
            mPublisher->put(std::move(payload));
            
            //SPDLOG_DEBUG("Published speed: {:.2f} m/s", speed_mps);
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to publish speed: {}", e.what());
        }
    }
};

int main(int argc, char* argv[]) {
    // Set up logging
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    
    SPDLOG_INFO("Starting Zenoh Speed Publisher");
    SPDLOG_INFO("This will publish vehicle speed data to 'vehicle/speed_mps'");
    SPDLOG_INFO("Press Ctrl+C to stop...");
    
    SpeedPublisher publisher;
    
    if (!publisher.initialize()) {
        SPDLOG_ERROR("Failed to initialize publisher. Exiting.");
        return -1;
    }
    
    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, [](int signal) {
        SPDLOG_INFO("Received interrupt signal. Shutting down...");
        std::exit(0);
    });
    
    // Start publishing (this will run until interrupted)
    publisher.start();
    
    return 0;
} 