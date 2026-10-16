use super::state::{Job, State};
use super::std;
use super::wake::{AlignedChunk, Chunk, PollWorker, Wake};
use crate::prefetch::{KeystreamProvider, Schedule};
use std::sync::Arc;

pub(super) struct CoreWorker<P: KeystreamProvider> {
    pub(super) shared: Arc<Wake<State<P>>>,
    pub(super) chunk_size: usize,
    pub(super) request_alignment: usize,
    pub(super) provider: Option<P>,
    pub(super) active_generation: u64,
}

impl<P: KeystreamProvider> PollWorker for CoreWorker<P> {
    type State = State<P>;
    fn shared(&self) -> &Arc<Wake<State<P>>> {
        &self.shared
    }
    fn idle(state: &State<P>) -> bool {
        !state.stop && !state.ready()
    }
    fn stop(state: &mut State<P>) {
        state.stop = true;
    }
    fn poll(&mut self) -> bool {
        let (provider, job) = {
            let mut state = self.shared.lock();
            if state.stop {
                return true;
            }
            if !state.ready() {
                return false;
            }
            let start = match state.schedule {
                Schedule::Continuous { .. } => state.scheduled,
                Schedule::Finite { .. } => state.scheduled.max(state.consumed),
            };
            let remaining = match state.schedule {
                Schedule::Continuous { capacity } => {
                    // Consumption can leave a one-byte deficit. Refill in
                    // whole request units instead of issuing an extra AES
                    // request for a single block.
                    let deficit = capacity.saturating_sub(state.available) as u128;
                    deficit.div_ceil(self.request_alignment as u128) * self.request_alignment as u128
                }
                Schedule::Finite { target } => (target as u128).saturating_sub(start),
            };
            let len = remaining
                .min(self.chunk_size as u128)
                .min(u128::MAX - start) as usize;
            if len == 0 {
                state.failed = true;
                return false;
            }
            let job = Job {
                generation: state.generation,
                nonce: state.nonce,
                offset: start,
                len,
            };
            state.scheduled = start + len as u128;
            state.in_flight = Some(job);
            (state.provider.clone(), job)
        };
        if self.active_generation != job.generation {
            self.provider = Some(provider);
            self.active_generation = job.generation;
        }
        // The provider needs an aligned output address. AlignedChunk shares
        // this Arc without copying and hides the padding from consumers.
        let storage_len = job
            .len
            .checked_add(64)
            .expect("prefetch chunk size overflow");
        let mut storage = Arc::<[u8]>::new_uninit_slice(storage_len);
        let allocation = Arc::get_mut(&mut storage).expect("new Arc allocation must be unique");
        let start = allocation.as_ptr().align_offset(64);
        let aligned = &mut allocation[start..start + job.len];
        // The provider contract requires a complete overwrite on success.
        // Keep both output and padding uninitialized until that write.
        let output = unsafe { core::slice::from_raw_parts_mut(aligned.as_mut_ptr().cast::<u8>(), aligned.len()) };
        let result = self
            .provider
            .as_mut()
            .unwrap()
            .generate_keystream(job.nonce, job.offset, output);
        let mut state = self.shared.lock();
        if state.stop {
            return true;
        }
        if state.generation != job.generation {
            return false;
        }
        state.in_flight = None;
        if result.is_err() {
            state.failed = true;
            self.provider = None;
            return false;
        }
        // The provider initialized exactly this range; padding need not be
        // initialized because AlignedChunk never exposes it as u8.
        let bytes = unsafe { AlignedChunk::from_initialized(storage, start, job.len) };
        state.cycle.generated_bytes += job.len;
        state.generated_position = job.offset + job.len as u128;
        let useful_start = job.offset.max(state.consumed);
        if useful_start < state.generated_position {
            state.available += (state.generated_position - useful_start) as usize;
            state.chunks.push_back(Chunk {
                bytes,
                position: job.offset,
            });
        }
        false
    }
}
