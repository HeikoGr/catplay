use std::time::Duration;

use catplay_tokio::TcpHelper;
use catplay_util::{AsyncShutdown, EventReconciler, EventSleeper, EventToken, LazyAsync, event_select, sleep};
use log::warn;

use crate::{
    carplay_tx::AirPlayRtspClient,
    msg::{StreamType, TeardownPayload},
    rtp::RtpReceiver,
    rtsp_frame::{RtspError, RtspResult},
    screen::tx::ScreenTransmitSession,
};

enum TeardownPendingStream {
    Audio(RtpReceiver),
    Screen(TcpHelper<ScreenTransmitSession>, String),
}

impl TeardownPendingStream {
    async fn shutdown(&mut self) {
        match self {
            TeardownPendingStream::Audio(s) => {
                s.shutdown().await;
            }
            TeardownPendingStream::Screen(s, _) => {
                s.shutdown().await;
            }
        }
    }
}

pub(super) struct TeardownTask {
    pub _stream_type: StreamType,
    task: LazyAsync<RtspResult<()>>,
    pub completed: Option<RtspResult<()>>,
}

impl TeardownTask {
    pub fn audio(client: AirPlayRtspClient, latency: Duration, rtp: RtpReceiver, stream_type: StreamType, immediate: bool) -> Self {
        let task = Self::run(client, latency, stream_type, TeardownPendingStream::Audio(rtp), immediate);
        Self {
            _stream_type: stream_type,
            task: LazyAsync::new(move || task),
            completed: None,
        }
    }

    pub fn screen(
        client: AirPlayRtspClient,
        latency: Duration,

        screen: TcpHelper<ScreenTransmitSession>,
        uuid: String,
        stream_type: StreamType,
        immediate: bool,
    ) -> Self {
        let task = Self::run(client, latency, stream_type, TeardownPendingStream::Screen(screen, uuid), immediate);
        Self {
            _stream_type: stream_type,
            task: LazyAsync::new(move || task),
            completed: None,
        }
    }

    pub async fn wait(&mut self) -> RtspResult<()> {
        if let Some(completed) = self.completed.clone() {
            return completed;
        }

        if let Some(ret) = self.task.take() {
            self.completed.replace(ret.clone());
            return ret;
        }

        event_select!(self.task);

        let ret = self
            .task
            .take()
            .unwrap_or_else(|| Err(RtspError::UnexpectedState("teardown task lost its result".into())));
        self.completed.replace(ret.clone());
        ret
    }

    fn take_completed(&mut self) -> Option<RtspResult<()>> {
        if self.completed.is_none() {
            self.completed = self.task.take();
        }

        self.completed.clone()
    }

    async fn run(
        client: AirPlayRtspClient,
        latency: Duration,

        stream_type: StreamType,
        mut stream: TeardownPendingStream,
        immediate: bool,
    ) -> RtspResult<()> {
        const TEARDOWN_SAFETY_MARGIN: Duration = Duration::from_millis(10);
        // Some receivers (seen on CarPlay 210.81, a Stellantis NAC Wave 2) need a moment after
        // ACK-ing an audio TEARDOWN before they accept a SETUP for the same stream again. Siri
        // re-opens its microphone stream ~40ms after tearing it down; with the follow-up SETUP
        // going out 5ms after the ACK, that receiver stalled for 10s and then answered 400.
        const TEARDOWN_AUDIO_SETTLE: Duration = Duration::from_millis(250);

        // Stop recording (if possible; not a hard requirement) and wait for `latency` plus a small margin
        // to ensure everything already transmitted will be played
        if !immediate {
            warn!("Teardown {stream_type:?}: waiting until latency {latency:?}");
            sleep(latency + TEARDOWN_SAFETY_MARGIN).await;
        }

        let is_audio = matches!(stream, TeardownPendingStream::Audio(_));
        let teardown = match &stream {
            TeardownPendingStream::Screen(_, uuid) => TeardownPayload::screen(stream_type, uuid.as_str()),
            TeardownPendingStream::Audio(_) => TeardownPayload::new(&[stream_type]),
        };

        warn!("Teardown {stream_type:?}: shutting down TCP/UDP session");
        stream.shutdown().await;
        warn!("Teardown {stream_type:?}: dropping FD");
        drop(stream);

        // Shutdown the TCP socket first, then immediately send TEARDOWN. This ordering is important.
        // sleep(Duration::from_millis(5)).await;
        warn!("Teardown {stream_type:?}: sending TEARDOWN request");
        client.teardown(teardown).await?;
        warn!("Teardown {stream_type:?}: ACK-ed by receiver");

        // The teardown only counts as finished once the receiver had time to release the stream;
        // whoever drains this task (e.g. the next SETUP of the same stream) waits this out too.
        if is_audio {
            sleep(TEARDOWN_AUDIO_SETTLE).await;
        }
        Ok(())
    }
}

impl EventSleeper for TeardownTask {
    async fn sleep(&mut self) -> Option<EventToken> {
        if self.completed.is_some() {
            return None;
        }

        let ret = self.task.sleep().await?;
        self.completed = self.task.take();
        Some(ret)
    }
}

impl EventReconciler for TeardownTask {
    type Error = RtspError;

    async fn reconcile(&mut self) -> RtspResult<()> {
        self.take_completed().unwrap_or(Ok(()))
    }
}

impl AsyncShutdown for TeardownPendingStream {
    async fn shutdown(&mut self) {
        match self {
            TeardownPendingStream::Audio(s) => {
                s.shutdown().await;
            }
            TeardownPendingStream::Screen(s, _) => {
                s.shutdown().await;
            }
        }
    }
}
