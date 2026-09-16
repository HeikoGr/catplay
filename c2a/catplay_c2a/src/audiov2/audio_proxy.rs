use std::time::Duration;

use catplay_carplay::{
    audio::{AudioPlayerBox, AudioRecorderBox, AudioStreamBasicDescription},
    carplay_tx::{AirPlayTransmitter, AirPlayTransmitterProxyRef},
    msg::{AudioFormat, AudioType, StreamType},
    rtsp_frame::{RtspError, RtspResult},
};
use log::{debug, error, warn};

use crate::proxy::pcm_proxy::PcmProxyPlayer;

pub struct AudioProxyUtil;

impl AudioProxyUtil {
    /// Whether the car listed `format` for `stream_type` in its own `/info`.
    ///
    /// Nothing negotiates this: the format handed to the car follows from
    /// whatever the iPhone's stream decoded to, so it can be one the car never
    /// claimed to take. A v210.81 car describes a whole stream type at once
    /// and leaves `audio_type` unset, so entries without one count for all.
    fn car_accepts(car: &AirPlayTransmitterProxyRef, stream_type: StreamType, format: AudioFormat) -> bool {
        let formats = &car.info_cached().audio_formats;

        // A car that says nothing about audio isn't claiming it can't take this.
        if formats.is_empty() {
            return true;
        }

        formats
            .iter()
            .filter(|entry| entry.stream_type == stream_type)
            .any(|entry| entry.audio_output_formats.unwrap_or_default().contains(format))
    }

    pub async fn open_audio(
        car: AirPlayTransmitterProxyRef,
        _latency: Duration,
        mut stream_type: StreamType,
        audio_type: AudioType,
        audio_format: AudioFormat,
        pcm_format: AudioStreamBasicDescription,
        duplex: bool,
    ) -> RtspResult<(AudioPlayerBox<i16>, Option<AudioRecorderBox<i16>>)> {
        debug!(
            "Starting stream {stream_type:?} with {audio_format:?}; mapped pcm is {:?}",
            AudioFormat::try_from(pcm_format)
        );

        // The codec the iPhone picked ends here. `audio_format` above is the
        // wire format of the Wi-Fi stream and may well be AAC-LC or Opus;
        // `pcm_format` is what the decoder produced from it, and everything
        // from here towards the car is plain PCM. So the format announced to
        // the car is derived from the decoded stream rather than from what the
        // iPhone sent - and by the same token, the formats advertised to the
        // iPhone (see audio_defaults.rs) describe this decoder, not the car.
        let audio_format = AudioFormat::try_from(pcm_format).map_err(|_| RtspError::Unknown)?;

        // Open a proxy audio stream towards the car

        let rate = pcm_format.sample_rate;

        let audio_pair = PcmProxyPlayer::pair(rate, pcm_format.channels_per_frame as _);
        let mut mic_player = None;
        let mut pending_mic = None;

        if duplex {
            // If audio type is Telephony or SpeechRecognition, open a duplex stream with a microphone too
            // in such case, `open_microphone` will follow `open_audio` (that order is a small implementation detail)
            // and we will have an instance of AudioRecorder to return as a microphone proxy
            let p = PcmProxyPlayer::pair(rate, pcm_format.channels_per_frame as _);
            pending_mic.replace(Box::new(p.1) as _);
            mic_player.replace(Box::new(p.0) as _);
        }

        let player_guard = audio_pair.0.guard.clone();

        // Media arrives on MainHighAudio and needs more buffer than the short
        // prompts and alerts the other stream types carry. Picked before the
        // remap below, which leaves every stream looking like MainAudio.
        let pcm_latency = match stream_type {
            StreamType::MainHighAudio => Duration::from_millis(75),
            _ => Duration::from_millis(32), // 75 too small
        };

        // The car is never told about MainHighAudio: by the time audio reaches
        // it the stream is decoded PCM, so it goes out as ordinary MainAudio.
        if stream_type == StreamType::MainHighAudio {
            stream_type = StreamType::MainAudio;
        }

        if !Self::car_accepts(&car, stream_type, audio_format) {
            warn!("Car never advertised {audio_format:?} on {stream_type:?}, opening the stream anyway");
        }

        let guard_fut = car.setup_audio(
            pcm_latency,
            stream_type,
            audio_format,
            audio_type,
            mic_player,
            Box::new(audio_pair.1),
        );

        let guard = guard_fut
            .await // blocks RTSP thread with a 2s timeout
            .inspect_err(|err| error!("Failed to setup audio transmitter? {err}"))?;
        player_guard.lock().unwrap().replace(guard);

        Ok((Box::new(audio_pair.0), pending_mic))
    }
}
