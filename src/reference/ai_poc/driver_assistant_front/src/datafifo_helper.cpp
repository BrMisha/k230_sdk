#include "datafifo_helper.h"
#include <cstdio>
#include <memory>
#include <chrono>
#include <thread>
#include <unistd.h>

#include "black_box.h"
#include "media_streamer_rtsp.h"
#include "websocket_server.h"
#include "mqtt_client.h"

using namespace driver_assistant_detector;

static void release(void* stream) {
}

DatafifoHelper::DatafifoHelper(uint64_t reader_phy_addr, uint64_t writer_phy_addr, std::optional<std::string> bb_dir_path,
                               PendingDetections* pending, asio::ip::udp::socket* udp_socket,
                               websocket_server::WebSocketServer* ws_server, backend_comm::MqttClient* mqtt_client)
    : reader_handle((k_datafifo_handle)K_DATAFIFO_INVALID_HANDLE)
    , writer_handle((k_datafifo_handle)K_DATAFIFO_INVALID_HANDLE)
    , pending(pending)
    , udp_socket(udp_socket)
    , ws_server(ws_server)
    , mqtt_client_(mqtt_client)
    , bb_dir_path(std::move(bb_dir_path))
{
    k_datafifo_params_s params_reader = {10, DATAFIFO_DETECTOR_BLOCK_LEN, K_TRUE, DATAFIFO_READER};
    k_s32 ret = kd_datafifo_open_by_addr(&reader_handle, &params_reader, reader_phy_addr);
    if (ret != K_SUCCESS) {
        printf("open datafifo reader error:%x\n", ret);
        return;
    }

    k_datafifo_params_s params_writer = {2, DATAFIFO_FRONT_BLOCK_LEN, K_TRUE, DATAFIFO_WRITER};
    ret = kd_datafifo_open_by_addr(&writer_handle, &params_writer, writer_phy_addr);
    if (ret != K_SUCCESS) {
        printf("open datafifo writer error:%x\n", ret);
        kd_datafifo_close(reader_handle);
        reader_handle = (k_datafifo_handle)K_DATAFIFO_INVALID_HANDLE;
        return;
    }

    ret = kd_datafifo_cmd(writer_handle, DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK, (void*)release);
    if (ret != K_SUCCESS) {
        printf("set release func callback error:%x\n", ret);
    }

    printf("datafifo_init finish\n");

    read_fifo_thread = std::thread(&DatafifoHelper::read_fifo_task, this);
}

DatafifoHelper::~DatafifoHelper() {
    if (writer_handle != (k_datafifo_handle)K_DATAFIFO_INVALID_HANDLE) {
        kd_datafifo_write(writer_handle, NULL);
        kd_datafifo_close(writer_handle);
    }
    if (reader_handle != (k_datafifo_handle)K_DATAFIFO_INVALID_HANDLE) {
        kd_datafifo_close(reader_handle);
    }
    printf("datafifo_deinit finish\n");

    stop();
}

bool DatafifoHelper::Writer::write(void* pData)
{
    auto ret = kd_datafifo_write(writer_handle, pData);
    if (K_SUCCESS != ret) {
        printf("kd_datafifo_write error:%x\n", ret);
        return false;
    }

    ret = kd_datafifo_cmd(writer_handle, DATAFIFO_CMD_WRITE_DONE, NULL);
    if (K_SUCCESS != ret) {
        printf("DATAFIFO_CMD_WRITE_DONE error:%x\n", ret);
        return false;
    }

    return true;
}

void DatafifoHelper::stop()
{
    if (send_stop) return;

    send_stop = true;
    read_fifo_thread.join();
}

std::optional<DatafifoHelper::Writer> DatafifoHelper::writer()
{
    // call write NULL to flush
    k_s32 ret = kd_datafifo_write(writer_handle, NULL);
    if (K_SUCCESS != ret) {
        printf("kd_datafifo_write error:%x\n", ret);
        return std::nullopt;
    }

    k_u32 datafifo_avail_write_len = 0;
    ret = kd_datafifo_cmd(writer_handle, DATAFIFO_CMD_GET_AVAIL_WRITE_LEN, &datafifo_avail_write_len);
    if (K_SUCCESS != ret) {
        printf("get available write len error:%x\n", ret);
        return std::nullopt;
    }
    if (datafifo_avail_write_len < DATAFIFO_FRONT_BLOCK_LEN) {
        return std::nullopt;
    }

    return std::make_optional(Writer(writer_handle));
}

void DatafifoHelper::read_fifo_task()
{
    k_u32 readLen = 0;
    k_s32 s32Ret = K_SUCCESS;
    int counter = 0;

    std::unique_ptr<black_box> streamer_file;
    if (bb_dir_path.has_value()) {
        streamer_file = std::make_unique<black_box>(bb_dir_path.value(), 1920, 1080);
    }

    MediaStreamerRtsp streamer_rtsp;
    streamer_rtsp.init("live", 1920, 1080);

    bool recording_started = false;
    std::vector<uint8_t> header_buffer;
    uint64_t header_buffer_pts = 0;  // PTS of buffered HEADER (for stale data detection)
    std::vector<uint8_t> periodic_header_buffer;  // Buffer for periodic HEADER frames

    while (!send_stop) {
        readLen = 0;
        s32Ret = kd_datafifo_cmd(reader_handle, DATAFIFO_CMD_GET_AVAIL_READ_LEN, &readLen);
        if (K_SUCCESS != s32Ret) {
            printf("get available read len error:%x\n", s32Ret);
            break;
        }

        if (readLen > 0) {
            auto start_time = std::chrono::steady_clock::now();

            k_char *pBuf;
            s32Ret = kd_datafifo_read(reader_handle, reinterpret_cast<void **>(&pBuf));
            if (K_SUCCESS != s32Ret) {
                printf("read error:%x\n", s32Ret);
                break;
            }

            auto frame = reinterpret_cast<DataFifoFrame_t*>(pBuf);

            // Push raw frame to WebRTC sessions immediately - Android will parse H.265 on its own
            if (mqtt_client_)
                mqtt_client_->push_video_to_sessions(frame->data, frame->data_len, frame->pts, frame->type);

            /*uint64_t microseconds = pts;
            uint64_t milliseconds = microseconds / 1000;
            uint64_t seconds = milliseconds / 1000;
            uint64_t minutes = seconds / 60;
            uint64_t hours = minutes / 60;
            printf("Read frame. pts: %lu %02lu:%02lu:%02lu.%03lu, type %d, len %u\n", pts,
                   hours, minutes % 60, seconds % 60, milliseconds % 1000, frame->type, frame->data_len);*/

            if (!recording_started) {
                if (frame->type == 3) {
                    // Buffer Type 3 (VPS/SPS/PPS) - don't write yet
                    header_buffer.assign(frame->data, frame->data + frame->data_len);
                    header_buffer_pts = frame->pts;  // Store HEADER PTS for validation
                    printf("Header buffered, size=%zu bytes, PTS=%lu\n", header_buffer.size(), frame->pts);
                }
                else if (frame->type == 2 && !header_buffer.empty()) {
                    // Validate HEADER and I-frame PTS are close (within 2 seconds)
                    // This detects stale HEADER frames from previous encoder sessions
                    uint64_t pts_diff = (frame->pts > header_buffer_pts)
                                      ? (frame->pts - header_buffer_pts)
                                      : (header_buffer_pts - frame->pts);

                    if (pts_diff > 2000000) {  // More than 2 seconds apart
                        printf("WARNING: Stale HEADER detected (PTS gap = %lu us = %.1f sec)\n",
                               pts_diff, pts_diff / 1000000.0);
                        printf("  HEADER PTS: %lu, I-frame PTS: %lu\n", header_buffer_pts, frame->pts);
                        printf("  Discarding stale HEADER, waiting for fresh one\n");
                        header_buffer.clear();
                        header_buffer_pts = 0;
                        // Don't start recording yet - wait for next HEADER that matches I-frame PTS
                    }
                    else {
                    // PTS gap is acceptable - HEADER and I-frame are from same session
                    // Combine header + IDR and write together
                    std::vector<uint8_t> combined;
                    combined.reserve(header_buffer.size() + frame->data_len);
                    combined.insert(combined.end(), header_buffer.begin(), header_buffer.end());
                    combined.insert(combined.end(), frame->data, frame->data + frame->data_len);

                    int ret = -1;

                    if (streamer_file != nullptr)
                        ret = streamer_file->write_video_frame(combined.data(), combined.size(), frame->pts,
                            true);

                    if (streamer_rtsp.is_ready())
                        ret = streamer_rtsp.write_video_frame(combined.data(), combined.size(), frame->pts,
                            true);

                    if (ret == 0) {
                        recording_started = true;
                        printf("First frame written (header+IDR), total size=%zu bytes, recording started\n",
                               combined.size());
                    } else {
                        printf("Failed to write first frame (header+IDR): error %d\n", ret);
                    }
                    header_buffer.clear();
                    }
                }
                // Discard Type 1 (P-frames) until we have header+IDR written
            }
            else {
                // After recording started: combine periodic HEADERs with I-frames, write P-frames normally
                if (frame->type == 3) {
                    // Periodic HEADER frame - buffer it to combine with next I-frame
                    // I-frames need their VPS/SPS/PPS to decode properly
                    periodic_header_buffer.assign(frame->data, frame->data + frame->data_len);
                }
                else if (frame->type == 2 && !periodic_header_buffer.empty()) {
                    // I-frame following HEADER - combine them
                    std::vector<uint8_t> combined;
                    combined.reserve(periodic_header_buffer.size() + frame->data_len);
                    combined.insert(combined.end(), periodic_header_buffer.begin(), periodic_header_buffer.end());
                    combined.insert(combined.end(), frame->data, frame->data + frame->data_len);

                    if (streamer_file != nullptr)
                        streamer_file->write_video_frame(combined.data(), combined.size(), frame->pts, true);
                    if (streamer_rtsp.is_ready())
                        streamer_rtsp.write_video_frame(combined.data(), combined.size(), frame->pts, true);

                    periodic_header_buffer.clear();
                }
                else {
                    // P-frame or I-frame without header - write normally
                    if (streamer_file != nullptr)
                        streamer_file->write_video_frame(frame->data, frame->data_len, frame->pts, frame->type == 2);
                    if (streamer_rtsp.is_ready())
                        streamer_rtsp.write_video_frame(frame->data, frame->data_len, frame->pts, frame->type == 2);

                    // Log standalone I-frames (shouldn't happen)
                    if (frame->type == 2) {
                        printf("WARNING: Standalone I-frame without HEADER (type 2), size=%u bytes, PTS=%lu\n",
                               frame->data_len, frame->pts);
                    }
                }
            }

            // blink
            if (counter++ > 100) {
                counter = 0;

                if (streamer_file) {
                    streamer_file->flush_files();
                }
            }

            s32Ret = kd_datafifo_cmd(reader_handle, DATAFIFO_CMD_READ_DONE, pBuf);
            if (K_SUCCESS != s32Ret) {
                printf("read done error:%x\n", s32Ret);
                break;
            }

            std::vector<DetectionNormalizedCommon>  _pending_pre_detections;
            std::vector<DetectionNormalizedCommon>  _pending_detections;
            DetectedSituation _pending_detections_situation;
            uint64_t _pending_detections_pts;
            {
                std::lock_guard lock(pending->mutex);

                if (pending->pts == UINT64_MAX)    // No pending detections
                    _pending_detections_pts = UINT64_MAX;
                else {
                    // If we have the first pts and pending->pts is valid, then we got a detection result.
                    // In this case take these values to process it
                    _pending_detections_pts = pending->pts;
                    _pending_pre_detections = std::move(pending->pre_detections);
                    _pending_detections = std::move(pending->detections);
                    _pending_detections_situation = pending->situation;
                    pending->pts = UINT64_MAX;
                }
            }

            if (_pending_detections_pts != UINT64_MAX) {
                if (streamer_file) {
                    streamer_file->write_detections(_pending_detections_pts, _pending_pre_detections, _pending_detections, _pending_detections_situation);
                }
                // Broadcast via WebSocket
                if (ws_server) {
                    ws_server->broadcast_detections(_pending_detections_pts, _pending_detections_situation, _pending_detections);
                }

                // TODO: impl in future
                /*std::lock_guard<std::mutex> lock(stream_endpoint_mutex);
                if (stream_endpoint_detections.port() != 0) {
                    udp_socket->send_to(asio::buffer(common_buf, len), stream_endpoint_detections);
                }*/
            }

            auto duration_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_time).count();
            if (duration_ms > 10.0) {
                printf("WARNING: Frame processing took %.2f ms\n", duration_ms);
            }
        }
        else {
            usleep(10000);
        }
    }

    // Do dot stop rtsp beause of to long
    //streamer_rtsp.stop();

    printf("read_fifo finished\n");
}
