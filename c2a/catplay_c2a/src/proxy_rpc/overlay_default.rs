use crate::{
    proxy_rpc::{OverlayPeerState, OverlayPolicy},
    ui::UiState,
};
use catplay_bt::BluezManager;
use catplay_util::{EventReconciler, EventSleeper, EventToken, LazyAsync};
use log::{debug, warn};
use std::time::Duration;

pub struct OverlayPolicyDefault {
    bluez: LazyAsync<BluezManager>,
    peer: OverlayPeerState,
    overlay: Option<UiState>,
}

impl OverlayPolicyDefault {
    pub fn new() -> Self {
        Self {
            bluez: LazyAsync::new(Self::start_pin_agent),
            peer: OverlayPeerState::Unconnected,
            overlay: None,
        }
    }
    async fn start_pin_agent() -> BluezManager {
        loop {
            let mut mgr = BluezManager::new();
            match mgr.register_pin_agent().await {
                Ok(()) => {
                    if let Err(err) = mgr.set_discoverable("hci0", true).await {
                        warn!("Failed to set BlueZ adapter discoverable: {err}");
                    }
                    if let Err(err) = mgr.set_pairable("hci0", true).await {
                        warn!("Failed to set BlueZ adapter pairable: {err}");
                    }
                    return mgr;
                }
                Err(err) => {
                    debug!("BlueZ agent not available: {err}");
                    tokio::time::sleep(Duration::from_secs(2)).await;
                }
            }
        }
    }
}

impl OverlayPolicy for OverlayPolicyDefault {
    fn overlay(&mut self) -> Option<UiState> {
        if self.peer != OverlayPeerState::Unconnected {
            return None;
        }

        if let Some(req) = self
            .bluez
            .as_ref()
            .as_ref()
            .and_then(|bluez| bluez.get_pairing_request())
        {
            return Some(UiState::Pairing {
                device: req.remote_name.unwrap_or("unknown".into()),
                pin: format!("{}", req.passkey),
            });
        }

        // return Some(UiState::Connecting {
        //     device: "iPhone".into(),
        //     ticks: 10,
        // });

        Some(UiState::WaitingForConnection)
    }

    fn on_hid_interact(&mut self) -> bool {
        if let Some(req) = self
            .bluez
            .as_ref()
            .as_ref()
            .and_then(|bluez| bluez.get_pairing_request())
        {
            req.accept();
            return false;
        }

        true
    }

    fn set_peer_state(&mut self, state: OverlayPeerState) {
        self.peer = state;
    }
}

impl EventReconciler for OverlayPolicyDefault {
    type Error = ();

    async fn reconcile(&mut self) -> Result<(), Self::Error> {
        self.overlay = self.overlay();
        Ok(())
    }
}

impl EventSleeper for OverlayPolicyDefault {
    async fn sleep(&mut self) -> Option<EventToken> {
        self.bluez.sleep().await
    }
}
