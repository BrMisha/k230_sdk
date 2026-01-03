#include "datachannel_peer.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <algorithm>

namespace backend_comm {

DataChannelPeer::DataChannelPeer(const std::vector<std::string>& ice_servers) {
    rtc::Configuration config;

    // Parse ICE servers
    for (const auto& server : ice_servers) {
        config.iceServers.emplace_back(server);
    }

    pc_ = std::make_shared<rtc::PeerConnection>(config);
    setup_callbacks();

    std::cout << "[DataChannel] PeerConnection created with " << ice_servers.size()
              << " ICE servers" << std::endl;
}

DataChannelPeer::~DataChannelPeer() {
    std::cout << "[DataChannel] Destroying peer connection" << std::endl;

    // Clear callbacks first
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_local_description_ = nullptr;
        on_ice_candidate_ = nullptr;
        on_state_change_ = nullptr;
        on_data_channel_message_ = nullptr;
    }

    // Close data channel and peer connection
    if (dc_) {
        dc_->close();
        dc_.reset();
    }

    if (pc_) {
        pc_->close();
        pc_.reset();
    }

    connected_ = false;
}

void DataChannelPeer::setup_callbacks() {
    // Local description (SDP offer/answer)
    pc_->onLocalDescription([this](rtc::Description desc) {
        std::string type = desc.typeString();
        std::string sdp = std::string(desc);

        std::cout << "[DataChannel] Local description generated: " << type << std::endl;

        OnLocalDescription cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cb = on_local_description_;
        }
        if (cb) {
            cb(type, sdp);
        }
    });

    // ICE candidates
    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        std::string mid = candidate.mid();
        std::string cand = candidate.candidate();

        // Filter TCP candidates (same as GstWebRTCPeer)
        if (cand.find(" tcp ") != std::string::npos ||
            cand.find(" TCP ") != std::string::npos) {
            return;
        }

        // Parse mlineindex from mid (usually "0" or "1")
        uint32_t mlineindex = 0;
        try {
            mlineindex = std::stoul(mid);
        } catch (...) {
            // mid might be a string like "video" or "data", use 0
        }

        OnIceCandidate cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cb = on_ice_candidate_;
        }
        if (cb) {
            cb(mlineindex, cand);
        }
    });

    // State changes
    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        std::cout << "[DataChannel] PeerConnection state: " << static_cast<int>(state) << std::endl;

        bool is_connected = (state == rtc::PeerConnection::State::Connected);
        bool was_connected = connected_.exchange(is_connected);

        if (is_connected != was_connected) {
            OnStateChange cb;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cb = on_state_change_;
            }
            if (cb) {
                cb(is_connected);
            }
        }
    });

    // Gathering state
    pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        std::cout << "[DataChannel] Gathering state: " << static_cast<int>(state) << std::endl;
    });

    // Handle incoming data channels (if remote creates one)
    pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> incoming) {
        std::cout << "[DataChannel] Incoming data channel: " << incoming->label() << std::endl;
        setup_data_channel(incoming);
    });
}

void DataChannelPeer::setup_data_channel(std::shared_ptr<rtc::DataChannel> dc) {
    dc_ = dc;

    dc_->onOpen([this]() {
        std::cout << "[DataChannel] Data channel opened" << std::endl;
        connected_ = true;

        OnStateChange cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cb = on_state_change_;
        }
        if (cb) {
            cb(true);
        }
    });

    dc_->onClosed([this]() {
        std::cout << "[DataChannel] Data channel closed" << std::endl;
        connected_ = false;

        OnStateChange cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            cb = on_state_change_;
        }
        if (cb) {
            cb(false);
        }
    });

    dc_->onError([](std::string error) {
        std::cerr << "[DataChannel] Error: " << error << std::endl;
    });

    dc_->onMessage([this](auto data) {
        if (std::holds_alternative<std::string>(data)) {
            std::string msg = std::get<std::string>(data);
            std::cout << "[DataChannel] Received message: " << msg << std::endl;

            OnDataChannelMessage cb;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cb = on_data_channel_message_;
            }
            if (cb) {
                cb(msg);
            }
        }
        // Binary messages are ignored (we only send, not receive video)
    });
}

void DataChannelPeer::create_offer() {
    if (offer_sent_.exchange(true)) {
        std::cout << "[DataChannel] Offer already sent, skipping" << std::endl;
        return;
    }

    std::cout << "[DataChannel] Creating data channel and offer..." << std::endl;

    // Create data channel first (required for it to appear in SDP)
    auto dc = pc_->createDataChannel("video");
    setup_data_channel(dc);

    // Setting local description triggers offer generation
    pc_->setLocalDescription();
}

void DataChannelPeer::set_remote_description(const std::string& type, const std::string& sdp) {
    std::cout << "[DataChannel] Setting remote description: " << type << std::endl;

    rtc::Description::Type desc_type;
    if (type == "answer") {
        desc_type = rtc::Description::Type::Answer;
    } else if (type == "offer") {
        desc_type = rtc::Description::Type::Offer;
    } else {
        std::cerr << "[DataChannel] Unknown description type: " << type << std::endl;
        return;
    }

    rtc::Description desc(sdp, desc_type);
    pc_->setRemoteDescription(desc);
}

void DataChannelPeer::add_ice_candidate(uint32_t mlineindex, const std::string& candidate) {
    if (candidate.empty()) {
        return;
    }

    // Filter TCP candidates
    if (candidate.find(" tcp ") != std::string::npos ||
        candidate.find(" TCP ") != std::string::npos) {
        return;
    }

    std::string mid = std::to_string(mlineindex);
    rtc::Candidate cand(candidate, mid);
    pc_->addRemoteCandidate(cand);
}

void DataChannelPeer::send_video_frame(const uint8_t* data, size_t size,
                                        uint64_t pts_us, uint8_t type) {
    if (!dc_ || !dc_->isOpen()) {
        return;
    }

    // Input bandwidth measurement
    bytes_received_ += size;
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_bw_print_).count();
    if (elapsed_ms >= 1000) {
        double in_mbps = (bytes_received_ * 8.0) / (elapsed_ms * 1000.0);
        double out_mbps = (bytes_sent_ * 8.0) / (elapsed_ms * 1000.0);
        double drop_mbps = (bytes_dropped_ * 8.0) / (elapsed_ms * 1000.0);
        size_t buf = dc_->bufferedAmount();
        std::cout << "[DataChannel] In: " << std::fixed << std::setprecision(2) << in_mbps
                  << " Mbps, Out: " << out_mbps
                  << " Mbps, Drop: " << drop_mbps << " Mbps, Buf: " << (buf / 1024) << " KB" << std::endl;
        bytes_received_ = 0;
        bytes_sent_ = 0;
        bytes_dropped_ = 0;
        last_bw_print_ = now;
    }

    // Flow control: skip P-frames when buffer is building up
    // Frame types: 1=P-frame, 2=header (VPS/SPS/PPS), 3=keyframe (I-frame)
    size_t buffered = dc_->bufferedAmount();
    constexpr size_t BUFFER_THRESHOLD = 256 * 1024;  // 256KB
    if (buffered > BUFFER_THRESHOLD && type == 1) {
        bytes_dropped_ += size;
        return;
    }

    // Simple frame format: [8B pts][1B type][video_data...]
    // SCTP delivers complete messages with known size, so len field is redundant
    const size_t HEADER_SIZE = 9;  // pts(8) + type(1)

    // Build message_variant directly to avoid copy in send()
    rtc::message_variant msg = rtc::binary(HEADER_SIZE + size);
    auto& frame = std::get<rtc::binary>(msg);
    std::byte* ptr = frame.data();

    // Header
    memcpy(ptr, &pts_us, 8);
    ptr += 8;
    *ptr++ = static_cast<std::byte>(type);

    // Video data
    memcpy(ptr, data, size);

    // Send with move - no copy inside libdatachannel
    dc_->send(std::move(msg));
    bytes_sent_ += HEADER_SIZE + size;

    frame_count_++;
    if (frame_count_ % 300 == 0) {
        std::cout << "[DataChannel] Sent " << frame_count_ << " frames" << std::endl;
    }
}

void DataChannelPeer::send_binary(const uint8_t* data, size_t size) {
    if (!dc_ || !dc_->isOpen()) {
        std::cerr << "[DataChannel] Cannot send binary - data channel not open" << std::endl;
        return;
    }

    std::vector<std::byte> bytes(size);
    memcpy(bytes.data(), data, size);
    dc_->send(bytes);
}

void DataChannelPeer::send_data(const std::string& message) {
    if (!dc_ || !dc_->isOpen()) {
        std::cerr << "[DataChannel] Cannot send data - data channel not open" << std::endl;
        return;
    }

    dc_->send(message);
    std::cout << "[DataChannel] Sent: " << message << std::endl;
}

void DataChannelPeer::set_on_local_description(OnLocalDescription cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_local_description_ = std::move(cb);
}

void DataChannelPeer::set_on_ice_candidate(OnIceCandidate cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_ice_candidate_ = std::move(cb);
}

void DataChannelPeer::set_on_state_change(OnStateChange cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_state_change_ = std::move(cb);
}

void DataChannelPeer::set_on_data_channel_message(OnDataChannelMessage cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_data_channel_message_ = std::move(cb);
}

}  // namespace backend_comm
