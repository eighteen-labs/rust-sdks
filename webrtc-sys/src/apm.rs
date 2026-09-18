// Copyright 2025 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use crate::impl_thread_safety;

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    /// The AEC3 suppressor knobs that decide how much of the near end
    /// survives while the far end is loud. Every field maps onto one member
    /// of `webrtc::EchoCanceller3Config`; `aec3_default_tuning()` returns
    /// WebRTC's own defaults so an untouched tuning changes nothing.
    #[derive(Debug, Clone, Copy, PartialEq)]
    pub struct Aec3Tuning {
        /// `dominant_nearend_detection.enr_threshold`: the echo-to-nearend
        /// ratio below which the near end is declared dominant.
        pub nearend_enr_threshold: f32,
        /// `dominant_nearend_detection.enr_exit_threshold`.
        pub nearend_enr_exit_threshold: f32,
        /// `dominant_nearend_detection.snr_threshold`.
        pub nearend_snr_threshold: f32,
        /// `dominant_nearend_detection.hold_duration`, in blocks.
        pub nearend_hold_duration: i32,
        /// `dominant_nearend_detection.trigger_threshold`, in blocks.
        pub nearend_trigger_threshold: i32,
        /// `suppressor.nearend_tuning.mask_lf.enr_transparent`.
        pub nearend_mask_lf_enr_transparent: f32,
        /// `suppressor.nearend_tuning.mask_lf.enr_suppress`.
        pub nearend_mask_lf_enr_suppress: f32,
        /// `suppressor.nearend_tuning.mask_lf.emr_transparent`.
        pub nearend_mask_lf_emr_transparent: f32,
        /// `suppressor.nearend_tuning.mask_hf.enr_transparent`.
        pub nearend_mask_hf_enr_transparent: f32,
        /// `suppressor.nearend_tuning.mask_hf.enr_suppress`.
        pub nearend_mask_hf_enr_suppress: f32,
        /// `suppressor.nearend_tuning.mask_hf.emr_transparent`.
        pub nearend_mask_hf_emr_transparent: f32,
    }

    unsafe extern "C++" {
        include!("livekit/apm.h");

        type AudioProcessingModule;

        unsafe fn process_stream(
            self: Pin<&mut AudioProcessingModule>,
            src: *const i16,
            src_len: usize,
            dst: *mut i16,
            dst_len: usize,
            sample_rate: i32,
            num_channels: i32,
        ) -> i32;

        unsafe fn process_reverse_stream(
            self: Pin<&mut AudioProcessingModule>,
            src: *const i16,
            src_len: usize,
            dst: *mut i16,
            dst_len: usize,
            sample_rate: i32,
            num_channels: i32,
        ) -> i32;

        fn set_stream_delay_ms(self: Pin<&mut AudioProcessingModule>, delay: i32) -> i32;

        fn create_apm(
            echo_canceller_enabled: bool,
            gain_controller_enabled: bool,
            high_pass_filter_enabled: bool,
            noise_suppression_enabled: bool,
        ) -> UniquePtr<AudioProcessingModule>;

        /// WebRTC's stock `EchoCanceller3Config` values, read from the
        /// linked libwebrtc rather than copied.
        fn aec3_default_tuning() -> Aec3Tuning;

        /// Like `create_apm`, but the echo canceller is built through an
        /// `EchoCanceller3Factory` carrying `tuning`.
        fn create_apm_with_aec3_tuning(
            echo_canceller_enabled: bool,
            gain_controller_enabled: bool,
            high_pass_filter_enabled: bool,
            noise_suppression_enabled: bool,
            tuning: Aec3Tuning,
        ) -> UniquePtr<AudioProcessingModule>;
    }
}

impl_thread_safety!(ffi::AudioProcessingModule, Send + Sync);
