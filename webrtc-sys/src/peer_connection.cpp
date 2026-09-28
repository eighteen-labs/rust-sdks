/*
 * Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/peer_connection.h"
#include "livekit/peer_connection_factory.h"

#include <memory>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "livekit/candidate.h"
#include "livekit/data_channel.h"
#include "livekit/jsep.h"
#include "livekit/media_stream.h"
#include "livekit/rtc_error.h"
#include "livekit/rtp_transceiver.h"
#include "rtc_base/logging.h"

namespace livekit_ffi {

webrtc::PeerConnectionInterface::RTCConfiguration to_native_rtc_configuration(
    RtcConfiguration config) {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config{};

  for (auto item : config.ice_servers) {
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.username = item.username.c_str();
    ice_server.password = item.password.c_str();

    for (auto url : item.urls)
      ice_server.urls.emplace_back(url.c_str());

    rtc_config.servers.push_back(ice_server);
  }

  rtc_config.continual_gathering_policy =
      static_cast<webrtc::PeerConnectionInterface::ContinualGatheringPolicy>(
          config.continual_gathering_policy);

  rtc_config.type =
      static_cast<webrtc::PeerConnectionInterface::IceTransportsType>(
          config.ice_transport_type);

  rtc_config.enable_sctp_snap = config.enable_sctp_snap;

  return rtc_config;
}

inline webrtc::PeerConnectionInterface::RTCOfferAnswerOptions
to_native_offer_answer_options(const RtcOfferAnswerOptions& options) {
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions rtc_options;
  rtc_options.offer_to_receive_video = options.offer_to_receive_video;
  rtc_options.offer_to_receive_audio = options.offer_to_receive_audio;
  rtc_options.voice_activity_detection = options.voice_activity_detection;
  rtc_options.ice_restart = options.ice_restart;
  rtc_options.use_rtp_mux = options.use_rtp_mux;
  rtc_options.raw_packetization_for_video = options.raw_packetization_for_video;
  rtc_options.num_simulcast_layers = options.num_simulcast_layers;
  rtc_options.use_obsolete_sctp_sdp = options.use_obsolete_sctp_sdp;
  return rtc_options;
}

PeerConnection::PeerConnection(
    std::shared_ptr<RtcRuntime> rtc_runtime,
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pc_factory,
    rust::Box<PeerConnectionObserverWrapper> observer)
    : rtc_runtime_(std::move(rtc_runtime)),
      pc_factory_(std::move(pc_factory)),
      observer_(std::move(observer)) {
  RTC_LOG(LS_VERBOSE) << "PeerConnection::PeerConnection()";
}

PeerConnection::~PeerConnection() {
  RTC_LOG(LS_VERBOSE) << "PeerConnection::~PeerConnection()";
}

bool PeerConnection::Initialize(
    webrtc::PeerConnectionInterface::RTCConfiguration config) {
  webrtc::PeerConnectionDependencies deps{this};
  auto result =
      pc_factory_->CreatePeerConnectionOrError(config, std::move(deps));

  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to create peer connection: "
                      << result.error().message();
    return false;
  }
  webrtc::MutexLock lock(&handle_mutex_);
  peer_connection_ = std::move(result.value());
  return true;
}

// ato patch: see handle_mutex_ in peer_connection.h.
webrtc::scoped_refptr<webrtc::PeerConnectionInterface> PeerConnection::handle()
    const {
  webrtc::MutexLock lock(&handle_mutex_);
  return peer_connection_;
}

// ato patch: what a call on a disposed PeerConnection reports, in the shape
// WebRTC itself uses for a call on a closed one.
static webrtc::RTCError disposed_error() {
  return webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                          "PeerConnection is disposed");
}

// ato patch: completes an async call made on a disposed PeerConnection. The
// Rust completions block on a channel send, so they must never run on the
// calling thread, which is a tokio worker; WebRTC reports a closed
// PeerConnection's errors from the signaling thread, and so does this.
template <typename Complete>
static void complete_disposed(const std::shared_ptr<RtcRuntime>& rtc_runtime,
                              rust::Box<PeerContext> ctx,
                              Complete complete) {
  auto owned_ctx = std::make_shared<rust::Box<PeerContext>>(std::move(ctx));
  rtc_runtime->signaling_thread()->PostTask(
      [owned_ctx, complete] { complete(std::move(*owned_ctx)); });
}

void PeerConnection::set_configuration(RtcConfiguration config) const {
  auto pc = handle();
  if (!pc) {
    throw std::runtime_error(serialize_error(to_error(disposed_error())));
  }
  auto result = pc->SetConfiguration(to_native_rtc_configuration(config));

  if (!result.ok()) {
    throw std::runtime_error(serialize_error(to_error(result)));
  }
}

void PeerConnection::create_offer(
    RtcOfferAnswerOptions options,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, std::unique_ptr<SessionDescription>)>
        on_success,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_error) const {
  auto pc = handle();
  if (!pc) {
    complete_disposed(rtc_runtime_, std::move(ctx),
                      [on_error](rust::Box<PeerContext> ctx) {
                        on_error(std::move(ctx), to_error(disposed_error()));
                      });
    return;
  }
  webrtc::scoped_refptr<NativeCreateSdpObserver> observer =
      webrtc::make_ref_counted<NativeCreateSdpObserver>(std::move(ctx), on_success,
                                                     on_error);

  pc->CreateOffer(observer.get(), to_native_offer_answer_options(options));
}

void PeerConnection::create_answer(
    RtcOfferAnswerOptions options,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, std::unique_ptr<SessionDescription>)>
        on_success,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_error) const {
  auto pc = handle();
  if (!pc) {
    complete_disposed(rtc_runtime_, std::move(ctx),
                      [on_error](rust::Box<PeerContext> ctx) {
                        on_error(std::move(ctx), to_error(disposed_error()));
                      });
    return;
  }
  webrtc::scoped_refptr<NativeCreateSdpObserver> observer =
      webrtc::make_ref_counted<NativeCreateSdpObserver>(std::move(ctx), on_success,
                                                     on_error);

  pc->CreateAnswer(observer.get(), to_native_offer_answer_options(options));
}

void PeerConnection::set_local_description(
    std::unique_ptr<SessionDescription> desc,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_complete) const {
  auto pc = handle();
  if (!pc) {
    complete_disposed(rtc_runtime_, std::move(ctx),
                      [on_complete](rust::Box<PeerContext> ctx) {
                        on_complete(std::move(ctx), to_error(disposed_error()));
                      });
    return;
  }
  webrtc::scoped_refptr<NativeSetLocalSdpObserver> observer =
      webrtc::make_ref_counted<NativeSetLocalSdpObserver>(std::move(ctx),
                                                       on_complete);

  pc->SetLocalDescription(desc->clone()->release(), observer);
}

void PeerConnection::set_remote_description(
    std::unique_ptr<SessionDescription> desc,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_complete) const {
  auto pc = handle();
  if (!pc) {
    complete_disposed(rtc_runtime_, std::move(ctx),
                      [on_complete](rust::Box<PeerContext> ctx) {
                        on_complete(std::move(ctx), to_error(disposed_error()));
                      });
    return;
  }
  webrtc::scoped_refptr<NativeSetRemoteSdpObserver> observer =
      webrtc::make_ref_counted<NativeSetRemoteSdpObserver>(std::move(ctx),
                                                        on_complete);

  pc->SetRemoteDescription(desc->clone()->release(), observer);
}

void PeerConnection::restart_ice() const {
  if (auto pc = handle()) {
    pc->RestartIce();
  }
}

void PeerConnection::add_ice_candidate(
    std::shared_ptr<IceCandidate> candidate,
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, RtcError)> on_complete) const {
  auto pc = handle();
  if (!pc) {
    complete_disposed(rtc_runtime_, std::move(ctx),
                      [on_complete](rust::Box<PeerContext> ctx) {
                        on_complete(std::move(ctx), to_error(disposed_error()));
                      });
    return;
  }
  auto owned_ctx = std::make_shared<rust::Box<PeerContext>>(std::move(ctx));
  pc->AddIceCandidate(
      candidate->release(), [owned_ctx, on_complete](const webrtc::RTCError& err) {
        on_complete(std::move(*owned_ctx), to_error(err));
      });
}

std::shared_ptr<DataChannel> PeerConnection::create_data_channel(
    rust::String label,
    DataChannelInit init) const {
  auto pc = handle();
  if (!pc) {
    throw std::runtime_error(serialize_error(to_error(disposed_error())));
  }
  webrtc::DataChannelInit rtc_init = to_native_data_channel_init(init);
  auto result = pc->CreateDataChannelOrError(label.c_str(), &rtc_init);

  if (!result.ok()) {
    throw std::runtime_error(serialize_error(to_error(result.error())));
  }

  return std::make_shared<DataChannel>(rtc_runtime_, result.value());
}

std::shared_ptr<RtpSender> PeerConnection::add_track(
    std::shared_ptr<MediaStreamTrack> track,
    const rust::Vec<rust::String>& stream_ids) const {
  auto pc = handle();
  if (!pc) {
    throw std::runtime_error(serialize_error(to_error(disposed_error())));
  }
  std::vector<std::string> std_stream_ids(stream_ids.begin(), stream_ids.end());
  auto result = pc->AddTrack(track->rtc_track(), std_stream_ids);
  if (!result.ok()) {
    throw std::runtime_error(serialize_error(to_error(result.error())));
  }

  return std::make_shared<RtpSender>(rtc_runtime_, result.value(), pc);
}

void PeerConnection::remove_track(std::shared_ptr<RtpSender> sender) const {
  auto pc = handle();
  if (!pc) {
    throw std::runtime_error(serialize_error(to_error(disposed_error())));
  }
  auto error = pc->RemoveTrackOrError(sender->rtc_sender());
  if (!error.ok())
    throw std::runtime_error(serialize_error(to_error(error)));
}

void PeerConnection::get_stats(
    rust::Box<PeerContext> ctx,
    rust::Fn<void(rust::Box<PeerContext>, rust::String)> on_stats) const {
  auto pc = handle();
  if (!pc) {
    // An empty report, which the Rust side reads as no stats.
    complete_disposed(rtc_runtime_, std::move(ctx),
                      [on_stats](rust::Box<PeerContext> ctx) {
                        on_stats(std::move(ctx), rust::String());
                      });
    return;
  }
  auto observer = webrtc::make_ref_counted<NativeRtcStatsCollector<PeerContext>>(
      std::move(ctx), on_stats);
  pc->GetStats(observer.get());
}

std::shared_ptr<RtpTransceiver> PeerConnection::add_transceiver(
    std::shared_ptr<MediaStreamTrack> track,
    RtpTransceiverInit init) const {
  auto pc = handle();
  if (!pc) {
    throw std::runtime_error(serialize_error(to_error(disposed_error())));
  }
  auto result = pc->AddTransceiver(track->rtc_track(),
                                   to_native_rtp_transceiver_init(init));
  if (!result.ok())
    throw std::runtime_error(serialize_error(to_error(result.error())));

  return std::make_shared<RtpTransceiver>(rtc_runtime_, result.value(), pc);
}

std::shared_ptr<RtpTransceiver> PeerConnection::add_transceiver_for_media(
    MediaType media_type,
    RtpTransceiverInit init) const {
  auto pc = handle();
  if (!pc) {
    throw std::runtime_error(serialize_error(to_error(disposed_error())));
  }
  auto result = pc->AddTransceiver(static_cast<webrtc::MediaType>(media_type),
                                   to_native_rtp_transceiver_init(init));

  if (!result.ok())
    throw std::runtime_error(serialize_error(to_error(result.error())));

  return std::make_shared<RtpTransceiver>(rtc_runtime_, result.value(), pc);
}

rust::Vec<RtpSenderPtr> PeerConnection::get_senders() const {
  rust::Vec<RtpSenderPtr> vec;
  auto pc = handle();
  if (!pc) {
    return vec;
  }
  for (auto sender : pc->GetSenders())
    vec.push_back(
        RtpSenderPtr{std::make_shared<RtpSender>(rtc_runtime_, sender, pc)});

  return vec;
}

rust::Vec<RtpReceiverPtr> PeerConnection::get_receivers() const {
  rust::Vec<RtpReceiverPtr> vec;
  auto pc = handle();
  if (!pc) {
    return vec;
  }
  for (auto receiver : pc->GetReceivers())
    vec.push_back(RtpReceiverPtr{
        std::make_shared<RtpReceiver>(rtc_runtime_, receiver, pc)});

  return vec;
}

rust::Vec<RtpTransceiverPtr> PeerConnection::get_transceivers() const {
  rust::Vec<RtpTransceiverPtr> vec;
  auto pc = handle();
  if (!pc) {
    return vec;
  }
  for (auto transceiver : pc->GetTransceivers())
    vec.push_back(RtpTransceiverPtr{
        std::make_shared<RtpTransceiver>(rtc_runtime_, transceiver, pc)});

  return vec;
}

// ato patch: the description getters share one shape — no handle or no
// description both read as none.
static std::unique_ptr<SessionDescription> clone_description(
    const webrtc::SessionDescriptionInterface* description) {
  if (description)
    return std::make_unique<SessionDescription>(description->Clone());

  return nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::current_local_description()
    const {
  auto pc = handle();
  return pc ? clone_description(pc->current_local_description()) : nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::current_remote_description()
    const {
  auto pc = handle();
  return pc ? clone_description(pc->current_remote_description()) : nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::pending_local_description()
    const {
  auto pc = handle();
  return pc ? clone_description(pc->pending_local_description()) : nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::pending_remote_description()
    const {
  auto pc = handle();
  return pc ? clone_description(pc->pending_remote_description()) : nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::local_description() const {
  auto pc = handle();
  return pc ? clone_description(pc->local_description()) : nullptr;
}

std::unique_ptr<SessionDescription> PeerConnection::remote_description() const {
  auto pc = handle();
  return pc ? clone_description(pc->remote_description()) : nullptr;
}

// ato patch: a disposed PeerConnection reports the states WebRTC gives a
// closed one.
PeerConnectionState PeerConnection::connection_state() const {
  auto pc = handle();
  if (!pc) {
    return PeerConnectionState::Closed;
  }
  return static_cast<PeerConnectionState>(pc->peer_connection_state());
}

SignalingState PeerConnection::signaling_state() const {
  auto pc = handle();
  if (!pc) {
    return SignalingState::Closed;
  }
  return static_cast<SignalingState>(pc->signaling_state());
}

IceGatheringState PeerConnection::ice_gathering_state() const {
  auto pc = handle();
  if (!pc) {
    return IceGatheringState::IceGatheringComplete;
  }
  return static_cast<IceGatheringState>(pc->ice_gathering_state());
}

IceConnectionState PeerConnection::ice_connection_state() const {
  auto pc = handle();
  if (!pc) {
    return IceConnectionState::IceConnectionClosed;
  }
  return static_cast<IceConnectionState>(pc->ice_connection_state());
}

void PeerConnection::close() const {
  if (auto pc = handle()) {
    pc->Close();
  }
}

// ato patch (see docs/adr on the LiveKit PeerConnection retention): close AND
// release the native handle. Without this, one closed PeerConnection per
// session stays alive for the lifetime of the PeerConnectionFactory, retained
// inside the prebuilt libwebrtc. `peer_connection_` holds the PeerConnection
// PROXY, so releasing it from any thread marshals destruction to the thread
// that owns the connection. The handle is taken under the lock but closed and
// released outside it: Close() blocks on the signaling thread, which may be
// running an observer callback that calls back into this wrapper.
void PeerConnection::dispose() const {
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  {
    webrtc::MutexLock lock(&handle_mutex_);
    pc = std::move(peer_connection_);
    peer_connection_ = nullptr;
  }
  if (!pc) {
    return;
  }
  pc->Close();
}

// PeerConnectionObserver

void PeerConnection::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  observer_->on_signaling_change(static_cast<SignalingState>(new_state));
}

void PeerConnection::OnAddStream(
    webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
  observer_->on_add_stream(std::make_unique<MediaStream>(rtc_runtime_, stream));
}

void PeerConnection::OnRemoveStream(
    webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
  // Find current MediaStream
  // observer_->on_remove_stream(std::make_unique<MediaStream>(rtc_runtime_,
  // stream));
}

void PeerConnection::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  observer_->on_data_channel(
      std::make_shared<DataChannel>(rtc_runtime_, data_channel));
}

void PeerConnection::OnRenegotiationNeeded() {
  observer_->on_renegotiation_needed();
}

void PeerConnection::OnNegotiationNeededEvent(uint32_t event_id) {
  observer_->on_negotiation_needed_event(event_id);
}

void PeerConnection::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  observer_->on_ice_connection_change(
      static_cast<IceConnectionState>(new_state));
}

void PeerConnection::OnStandardizedIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  observer_->on_standardized_ice_connection_change(
      static_cast<IceConnectionState>(new_state));
}

void PeerConnection::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  observer_->on_connection_change(static_cast<PeerConnectionState>(new_state));
}

void PeerConnection::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  observer_->on_ice_gathering_change(static_cast<IceGatheringState>(new_state));
}

void PeerConnection::OnIceCandidate(
    const webrtc::IceCandidate* candidate) {
  auto new_candidate = webrtc::CreateIceCandidate(candidate->sdp_mid(),
                                                  candidate->sdp_mline_index(),
                                                  candidate->candidate());
  observer_->on_ice_candidate(
      std::make_unique<IceCandidate>(std::move(new_candidate)));
}

void PeerConnection::OnIceCandidateError(const std::string& address,
                                         int port,
                                         const std::string& url,
                                         int error_code,
                                         const std::string& error_text) {
  observer_->on_ice_candidate_error(address, port, url, error_code, error_text);
}

void PeerConnection::OnIceCandidateRemoved(const webrtc::IceCandidate* ice_candidate) {
  rust::Vec<CandidatePtr> vec;
  if(ice_candidate != nullptr) {
    vec.push_back(CandidatePtr{std::make_unique<Candidate>(ice_candidate->candidate())});
  }
  observer_->on_ice_candidates_removed(std::move(vec));
}

void PeerConnection::OnIceConnectionReceivingChange(bool receiving) {
  observer_->on_ice_connection_receiving_change(receiving);
}

void PeerConnection::OnIceSelectedCandidatePairChanged(
    const webrtc::CandidatePairChangeEvent& event) {
  CandidatePairChangeEvent e{};
  e.selected_candidate_pair.local =
      std::make_unique<Candidate>(event.selected_candidate_pair.local);
  e.selected_candidate_pair.remote =
      std::make_unique<Candidate>(event.selected_candidate_pair.remote);
  e.last_data_received_ms = event.last_data_received_ms;
  e.reason = event.reason;
  e.estimated_disconnected_time_ms = event.estimated_disconnected_time_ms;

  observer_->on_ice_selected_candidate_pair_changed(std::move(e));
}

void PeerConnection::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  rust::Vec<MediaStreamPtr> vec;

  for (const auto& item : streams) {
    vec.push_back(
        MediaStreamPtr{std::make_unique<MediaStream>(rtc_runtime_, item)});
  }

  observer_->on_add_track(
      std::make_unique<RtpReceiver>(rtc_runtime_, receiver, handle()),
      std::move(vec));
}

void PeerConnection::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  observer_->on_track(std::make_unique<RtpTransceiver>(
      rtc_runtime_, transceiver, handle()));
}

void PeerConnection::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  observer_->on_remove_track(
      std::make_unique<RtpReceiver>(rtc_runtime_, receiver, handle()));
}

void PeerConnection::OnInterestingUsage(int usage_pattern) {
  observer_->on_interesting_usage(usage_pattern);
}

}  // namespace livekit_ffi
