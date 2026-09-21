use crate::rtsp_frame::{RtspError, RtspRequest, RtspResponse, RtspResult};
use catplay_util::{EventSleeper, EventToken, Sleep, event_select, notify::Notify, oneshot, sleep_until};
use log::{debug, warn};
use std::{
    collections::{BTreeMap, VecDeque},
    future::Future,
    mem,
    pin::Pin,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU32, Ordering},
    },
    task::{Context, Poll},
    time::{Duration, Instant},
};

#[derive(Clone)]
pub struct RtspQueue {
    inner: Arc<Inner>,
}

pub struct RtspDrain {
    inner: Arc<Inner>,
}

type RtspSender = oneshot::Sender<RtspResult<RtspResponse>>;

struct FifoPending {
    cseq: u32,
    sender: Option<RtspSender>,
}

/// A request that has been written to the socket and is waiting for its response.
/// Used only for diagnostics: how long did the peer take, and which requests time out.
struct InFlight {
    sent_at: Instant,
    label: String,
}

struct Inner {
    next_cseq: AtomicU32,

    pending_send: Mutex<Vec<RtspRequest>>,
    pending_response: Mutex<BTreeMap<u32, RtspSender>>,
    pending_response_fifo: Mutex<VecDeque<FifoPending>>,
    in_flight: Mutex<BTreeMap<u32, InFlight>>,

    notify_pending: Notify,
    notify_closed: Notify,

    fifo: bool,
    closed: Mutex<bool>,
}

impl RtspDrain {
    pub fn close(&mut self) {
        let mut closed = self.inner.closed.lock().unwrap();
        if *closed {
            return;
        }

        debug!("RTSP: Closing drain!");
        *closed = true;

        let pending = mem::take(&mut *self.inner.pending_response.lock().unwrap());
        for (_seq, future) in pending {
            let _ = future.send(Err(RtspError::Closed));
        }

        let pending_fifo = mem::take(&mut *self.inner.pending_response_fifo.lock().unwrap());
        for pending in pending_fifo {
            if let Some(future) = pending.sender {
                let _ = future.send(Err(RtspError::Closed));
            }
        }

        self.inner.in_flight.lock().unwrap().clear();
        self.inner.notify_pending.notify();
    }

    /// Forget the in-flight record for `cseq` and report what it was and how long it took.
    /// `None` when the request was never drained (or already reported).
    pub fn take_in_flight(&self, cseq: u32) -> Option<(String, Duration)> {
        let in_flight = self.inner.in_flight.lock().unwrap().remove(&cseq)?;
        Some((in_flight.label, in_flight.sent_at.elapsed()))
    }

    pub fn feed(&self, resp: RtspResponse) {
        if self.inner.fifo {
            if let Some(mut pending) = self.inner.pending_response_fifo.lock().unwrap().pop_front() {
                // FIFO responses carry no CSeq, so nobody looks the in-flight record up by it;
                // drop it here or the event socket accumulates one per request for the session.
                self.inner.in_flight.lock().unwrap().remove(&pending.cseq);
                if let Some(sender) = pending.sender.take() {
                    let _ = sender.send(Ok(resp));
                    debug!("RTSP: fullfilling FIFO promise for cseq={}", pending.cseq);
                } else {
                    debug!("RTSP: consuming FIFO response for dropped cseq={}", pending.cseq);
                }
            } else {
                debug!("RTSP: unexpected FIFO response without pending request");
            }
            return;
        }

        if let Some(cseq) = resp.cseq {
            if let Some(sender) = { self.inner.pending_response.lock().unwrap().remove(&cseq) } {
                let _ = sender.send(Ok(resp));
                debug!("RTSP: fullfilling promise for cseq={cseq}");
            } else {
                debug!("RTSP: unexpected response with cseq={cseq} (future was dropped?)");
            }
        } else {
            debug!("RTSP: unexpected response without cseq");
        }
    }

    pub fn drain(&mut self) -> Vec<RtspRequest> {
        let out = mem::take(&mut *self.inner.pending_send.lock().unwrap());

        let now = Instant::now();
        let mut in_flight = self.inner.in_flight.lock().unwrap();
        for req in &out {
            if let Some(cseq) = req.cseq {
                in_flight.insert(
                    cseq,
                    InFlight {
                        sent_at: now,
                        label: format!("{} {}", req.method, req.url),
                    },
                );
            }
        }

        out
    }
}

impl Drop for RtspDrain {
    fn drop(&mut self) {
        self.close()
    }
}

impl EventSleeper for RtspDrain {
    async fn sleep(&mut self) -> Option<EventToken> {
        // Wake up when there is a pending request to be serialized and stored into socket's buffer.
        event_select!(self.inner.notify_pending.clone())
    }
}

impl RtspQueue {
    /// A normal RTSP connection with a remote.
    pub fn new() -> (Self, RtspDrain) {
        Self::create(false)
    }

    /// A special case for the event socket - match responses to requests in FIFO mode
    /// as iPhone does not generate CSeq in response.
    pub fn fifo() -> (Self, RtspDrain) {
        Self::create(true)
    }

    pub fn is_closed(&self) -> bool {
        *self.inner.closed.lock().unwrap()
    }

    pub fn size(&self) -> usize {
        let fifo_size = self
            .inner
            .pending_response_fifo
            .lock()
            .unwrap()
            .iter()
            .filter(|entry| entry.sender.is_some())
            .count();
        let size = self.inner.pending_response.lock().unwrap().len();

        fifo_size + size
    }

    pub fn outgoing_size(&self) -> usize {
        self.inner.pending_send.lock().unwrap().len()
    }

    fn create(fifo: bool) -> (Self, RtspDrain) {
        let inner = Arc::new(Inner {
            next_cseq: AtomicU32::new(0),
            pending_send: Mutex::new(Vec::new()),
            pending_response: Mutex::new(BTreeMap::new()),
            pending_response_fifo: Mutex::new(VecDeque::new()),
            in_flight: Mutex::new(BTreeMap::new()),
            notify_pending: Notify::new(),
            notify_closed: Notify::new(),
            closed: Mutex::new(false),
            fifo,
        });

        (Self { inner: inner.clone() }, RtspDrain { inner })
    }

    pub fn queue(&self, mut req: RtspRequest, deadline: Option<Instant>) -> (u32, RtspFuture) {
        let closed = self.inner.closed.lock().unwrap();

        let cseq = self.inner.next_cseq.fetch_add(1, Ordering::Relaxed);
        req.cseq.replace(cseq);

        let (tx, rx) = oneshot::channel::<RtspResult<RtspResponse>>();
        if !*closed {
            if self.inner.fifo {
                let mut fifo = self.inner.pending_response_fifo.lock().unwrap();
                fifo.push_back(FifoPending { cseq, sender: Some(tx) });
                let mut pending = self.inner.pending_send.lock().unwrap();
                pending.push(req);
            } else {
                self.inner.pending_response.lock().unwrap().insert(cseq, tx);
                let mut pending = self.inner.pending_send.lock().unwrap();
                pending.push(req);
            }

            self.inner.notify_pending.notify();
        } else {
            let _ = tx.send(Err(RtspError::Closed));
        }

        (
            cseq,
            RtspFuture {
                inner: rx,
                cseq,
                parent: self.inner.clone(),
                timeout: None,
                deadline,
            },
        )
    }

    pub fn request_timeout(&self, req: RtspRequest, timeout_: Duration) -> RtspFuture {
        let (_cseq, fut) = self.queue(req, Some(Instant::now() + timeout_));
        fut
    }

    pub fn request_until(&self, req: RtspRequest, deadline: Instant) -> RtspFuture {
        let (_cseq, fut) = self.queue(req, Some(deadline));
        fut
    }

    pub fn request(&self, req: RtspRequest) -> RtspFuture {
        let (_cseq, fut) = self.queue(req, None);
        fut
    }
}

impl EventSleeper for RtspQueue {
    async fn sleep(&mut self) -> Option<EventToken> {
        // Allow waking up when drain is marked closed
        event_select!(self.inner.notify_closed.clone())
    }
}

pub struct RtspFuture {
    inner: oneshot::Receiver<RtspResult<RtspResponse>>,
    cseq: u32,
    parent: Arc<Inner>,
    deadline: Option<Instant>,
    timeout: Option<Sleep>,
}

impl Future for RtspFuture {
    type Output = RtspResult<RtspResponse>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // SAFETY: `this` is used only for field projection. Once `timeout`
        // contains a `Sleep`, it is never moved out or replaced.
        let this = unsafe { self.get_unchecked_mut() };

        if this.timeout.is_none()
            && let Some(deadline) = this.deadline
        {
            this.timeout.replace(sleep_until(deadline));
        }

        if let Some(timeout) = &mut this.timeout
            // SAFETY: `timeout` is pinned in place by the enclosing `RtspFuture`.
            // We never move it after insertion above.
            && unsafe { Pin::new_unchecked(timeout) }.poll(cx).is_ready()
        {
            match this.parent.in_flight.lock().unwrap().remove(&this.cseq) {
                Some(in_flight) => warn!(
                    "RTSP: {} cseq={} timed out after {:?} without a response",
                    in_flight.label,
                    this.cseq,
                    in_flight.sent_at.elapsed()
                ),
                None => warn!("RTSP: cseq={} timed out before it was even sent", this.cseq),
            }
            return Poll::Ready(Err(RtspError::Timeout));
        }

        match Pin::new(&mut this.inner).poll(cx) {
            Poll::Ready(Ok(Ok(resp))) => Poll::Ready(Ok(resp)),
            Poll::Ready(Ok(Err(err))) => Poll::Ready(Err(err)),
            Poll::Ready(Err(_recv_err)) => Poll::Ready(Err(RtspError::Closed)),
            Poll::Pending => Poll::Pending,
        }
    }
}

impl Drop for RtspFuture {
    fn drop(&mut self) {
        if self.parent.fifo {
            let mut fifo = self.parent.pending_response_fifo.lock().unwrap();
            if let Some(pos) = fifo.iter().position(|entry| entry.cseq == self.cseq) {
                fifo[pos].sender.take();
                debug!("RTSP: FIFO future for cseq={} dropped, keeping response slot", self.cseq);
            }
        } else {
            let mut pending = self.parent.pending_response.lock().unwrap();
            if pending.remove(&self.cseq).is_some() {
                debug!("RTSP: future for cseq={} dropped, cleaning up", self.cseq);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rtsp_frame::{HttpStatus, RtspMethod};

    #[test]
    fn dropped_fifo_future_is_removed_from_pending_queue() {
        let (queue, _drain) = RtspQueue::fifo();

        let fut = queue.request(RtspRequest::new(RtspMethod::Post, "/command"));
        assert_eq!(queue.size(), 1);
        assert_eq!(queue.outgoing_size(), 1);

        drop(fut);
        assert_eq!(queue.size(), 0);
        assert_eq!(queue.outgoing_size(), 1);
    }

    #[tokio::test]
    async fn dropped_fifo_future_response_does_not_complete_next_request() {
        let (queue, mut drain) = RtspQueue::fifo();

        let stale = queue.request(RtspRequest::new(RtspMethod::Post, "/stale"));
        drop(stale);

        let current = queue.request(RtspRequest::new(RtspMethod::Post, "/command"));
        let sent = drain.drain();
        assert_eq!(sent.len(), 2);
        assert_eq!(sent[0].url.as_ref(), "/stale");
        assert_eq!(sent[1].url.as_ref(), "/command");

        drain.feed(RtspResponse::new(None, HttpStatus::Ok));
        assert_eq!(queue.size(), 1);

        drain.feed(RtspResponse::new(None, HttpStatus::Ok));

        let response = current.await.expect("current FIFO response");
        assert_eq!(response.status, HttpStatus::Ok);
        assert_eq!(queue.size(), 0);

        // Both in-flight records were released by the FIFO responses
        assert!(drain.take_in_flight(0).is_none());
        assert!(drain.take_in_flight(1).is_none());
    }
}
