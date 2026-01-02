#include "datachannel_peer.h"
#include <iostream>
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

    // Skip every 2nd and 3rd P-frame to reduce bandwidth (~10fps instead of 30fps for P-frames)
    // Always send headers (type=2) and keyframes (type=1)
    if (type == 0) {
        pframe_count_++;
        if ((pframe_count_ % 3) != 1) {  // Send only 1st of every 3 P-frames
            frame_count_++;
            return;
        }
    }

    // Memory protection: check buffered amount
    size_t buffered = dc_->bufferedAmount();
    constexpr size_t MAX_BUFFERED = 2 * 1024 * 1024;  // 2MB max
    if (buffered > MAX_BUFFERED) {
        frame_count_++;
        return;  // Silent drop to prevent memory growth
    }

    // Chunking protocol for WebRTC data channel (16KB limit)
    // Chunk format: [4B frame_id][2B chunk_idx][2B total_chunks][payload]
    // First chunk payload: [8B pts][4B len][1B type][video_data...]
    // Subsequent chunks: [video_data...]

    const size_t CHUNK_HEADER_SIZE = 8;      // frame_id(4) + chunk_idx(2) + total_chunks(2)
    const size_t FRAME_HEADER_SIZE = 13;     // pts(8) + len(4) + type(1)
    const size_t MAX_CHUNK_SIZE = 15000;     // Safe limit below 16KB
    const size_t MAX_PAYLOAD = MAX_CHUNK_SIZE - CHUNK_HEADER_SIZE;

    // Total payload = frame header + video data
    const size_t total_payload = FRAME_HEADER_SIZE + size;

    // Calculate number of chunks needed
    uint16_t total_chunks = static_cast<uint16_t>((total_payload + MAX_PAYLOAD - 1) / MAX_PAYLOAD);
    uint32_t frame_id = static_cast<uint32_t>(frame_count_);

    // Build frame header once
    uint8_t frame_header[FRAME_HEADER_SIZE];
    memcpy(frame_header, &pts_us, 8);          // PTS (8 bytes, little-endian)
    uint32_t data_len = static_cast<uint32_t>(size);
    memcpy(frame_header + 8, &data_len, 4);    // Data length (4 bytes)
    frame_header[12] = type;                    // Type (1 byte)

    size_t payload_offset = 0;  // Offset into logical payload (frame_header + video_data)

    for (uint16_t chunk_idx = 0; chunk_idx < total_chunks; chunk_idx++) {
        size_t remaining = total_payload - payload_offset;
        size_t chunk_payload_size = (remaining > MAX_PAYLOAD) ? MAX_PAYLOAD : remaining;
        size_t chunk_size = CHUNK_HEADER_SIZE + chunk_payload_size;

        std::vector<std::byte> chunk(chunk_size);
        std::byte* ptr = chunk.data();

        // Chunk header
        memcpy(ptr, &frame_id, 4);
        ptr += 4;
        memcpy(ptr, &chunk_idx, 2);
        ptr += 2;
        memcpy(ptr, &total_chunks, 2);
        ptr += 2;

        // Chunk payload (may span frame_header and video_data)
        size_t written = 0;
        while (written < chunk_payload_size) {
            if (payload_offset < FRAME_HEADER_SIZE) {
                // Copy from frame header
                size_t from_header = std::min(FRAME_HEADER_SIZE - payload_offset,
                                               chunk_payload_size - written);
                memcpy(ptr, frame_header + payload_offset, from_header);
                ptr += from_header;
                payload_offset += from_header;
                written += from_header;
            } else {
                // Copy from video data
                size_t video_offset = payload_offset - FRAME_HEADER_SIZE;
                size_t from_video = chunk_payload_size - written;
                memcpy(ptr, data + video_offset, from_video);
                payload_offset += from_video;
                written += from_video;
            }
        }

        // Send chunk
        dc_->send(chunk);
    }

    frame_count_++;
    if (frame_count_ % 300 == 0) {  // Log every 10 seconds at 30fps
        std::cout << "[DataChannel] Sent " << frame_count_ << " frames (" << total_chunks
                  << " chunks/frame)" << std::endl;
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
