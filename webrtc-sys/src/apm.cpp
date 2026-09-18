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

#include "livekit/apm.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_canceller3_factory.h"
#include "api/environment/environment_factory.h"

#include <iostream>
#include <memory>

namespace livekit_ffi {

namespace {

webrtc::EchoCanceller3Config ToAec3Config(const Aec3Tuning& t) {
  webrtc::EchoCanceller3Config cfg;
  auto& dn = cfg.suppressor.dominant_nearend_detection;
  dn.enr_threshold = t.nearend_enr_threshold;
  dn.enr_exit_threshold = t.nearend_enr_exit_threshold;
  dn.snr_threshold = t.nearend_snr_threshold;
  dn.hold_duration = t.nearend_hold_duration;
  dn.trigger_threshold = t.nearend_trigger_threshold;
  auto& lf = cfg.suppressor.nearend_tuning.mask_lf;
  lf.enr_transparent = t.nearend_mask_lf_enr_transparent;
  lf.enr_suppress = t.nearend_mask_lf_enr_suppress;
  lf.emr_transparent = t.nearend_mask_lf_emr_transparent;
  auto& hf = cfg.suppressor.nearend_tuning.mask_hf;
  hf.enr_transparent = t.nearend_mask_hf_enr_transparent;
  hf.enr_suppress = t.nearend_mask_hf_enr_suppress;
  hf.emr_transparent = t.nearend_mask_hf_emr_transparent;
  // Clamps out-of-range values the way AEC3 itself would; the return value
  // only says whether anything was clamped.
  webrtc::EchoCanceller3Config::Validate(&cfg);
  return cfg;
}

}  // namespace

Aec3Tuning aec3_default_tuning() {
  const webrtc::EchoCanceller3Config cfg;
  const auto& dn = cfg.suppressor.dominant_nearend_detection;
  const auto& lf = cfg.suppressor.nearend_tuning.mask_lf;
  const auto& hf = cfg.suppressor.nearend_tuning.mask_hf;
  Aec3Tuning t;
  t.nearend_enr_threshold = dn.enr_threshold;
  t.nearend_enr_exit_threshold = dn.enr_exit_threshold;
  t.nearend_snr_threshold = dn.snr_threshold;
  t.nearend_hold_duration = dn.hold_duration;
  t.nearend_trigger_threshold = dn.trigger_threshold;
  t.nearend_mask_lf_enr_transparent = lf.enr_transparent;
  t.nearend_mask_lf_enr_suppress = lf.enr_suppress;
  t.nearend_mask_lf_emr_transparent = lf.emr_transparent;
  t.nearend_mask_hf_enr_transparent = hf.enr_transparent;
  t.nearend_mask_hf_enr_suppress = hf.enr_suppress;
  t.nearend_mask_hf_emr_transparent = hf.emr_transparent;
  return t;
}

AudioProcessingModule::AudioProcessingModule(
    const AudioProcessingConfig& config) {
  webrtc::BuiltinAudioProcessingBuilder builder;
  if (config.aec3_tuning.has_value()) {
    builder.SetEchoControlFactory(
        std::make_unique<webrtc::EchoCanceller3Factory>(
            ToAec3Config(*config.aec3_tuning)));
  }
  apm_ = builder.Build(webrtc::CreateEnvironment());

  apm_->ApplyConfig(config.ToWebrtcConfig());
  apm_->Initialize();
}

int AudioProcessingModule::process_stream(const int16_t* src,
                                          size_t src_len,
                                          int16_t* dst,
                                          size_t dst_len,
                                          int sample_rate,
                                          int num_channels) {
  webrtc::StreamConfig stream_cfg(sample_rate, num_channels);
  return apm_->ProcessStream(src, stream_cfg, stream_cfg, dst);
}

int AudioProcessingModule::process_reverse_stream(const int16_t* src,
                                                  size_t src_len,
                                                  int16_t* dst,
                                                  size_t dst_len,
                                                  int sample_rate,
                                                  int num_channels) {
  webrtc::StreamConfig stream_cfg(sample_rate, num_channels);
  return apm_->ProcessReverseStream(src, stream_cfg, stream_cfg, dst);
}

int AudioProcessingModule::set_stream_delay_ms(int delay_ms) {
  return apm_->set_stream_delay_ms(delay_ms);
}

std::unique_ptr<AudioProcessingModule> create_apm(
    bool echo_canceller_enabled,
    bool gain_controller_enabled,
    bool high_pass_filter_enabled,
    bool noise_suppression_enabled) {
  AudioProcessingConfig config;
  config.echo_canceller_enabled = echo_canceller_enabled;
  config.gain_controller_enabled = gain_controller_enabled;
  config.high_pass_filter_enabled = high_pass_filter_enabled;
  config.noise_suppression_enabled = noise_suppression_enabled;
  return std::make_unique<AudioProcessingModule>(config);
}

std::unique_ptr<AudioProcessingModule> create_apm_with_aec3_tuning(
    bool echo_canceller_enabled,
    bool gain_controller_enabled,
    bool high_pass_filter_enabled,
    bool noise_suppression_enabled,
    Aec3Tuning tuning) {
  AudioProcessingConfig config;
  config.echo_canceller_enabled = echo_canceller_enabled;
  config.gain_controller_enabled = gain_controller_enabled;
  config.high_pass_filter_enabled = high_pass_filter_enabled;
  config.noise_suppression_enabled = noise_suppression_enabled;
  config.aec3_tuning = tuning;
  return std::make_unique<AudioProcessingModule>(config);
}

}  // namespace livekit_ffi
