/* ============================================================
 * atem-control.cpp — in-process BMD ATEM control for the
 * Shot Presets plugin.
 *
 * One dedicated worker thread:
 *   - CoInitializeEx(MTA)
 *   - holds the IBMDSwitcher / IBMDSwitcherMixEffectBlock COM refs
 *   - waits on a condition variable for queued switch requests
 *   - performs SetProgramInput synchronously (it's fast — single COM
 *     call), then loops
 * The plugin enqueues an int and returns; render threads never block on
 * BMD I/O. Auto-reconnects if the connection drops between requests.
 * ============================================================ */

#include "atem-control.h"

#include <obs-module.h>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <BMDSwitcherAPI.h>
#endif

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace {

#ifdef _WIN32

/* ── BMD MixEffect callback ─────────────────────────────────────
 * BMD calls our Notify() from inside its own thread when state
 * changes on the mix-effect block. We only care about transition
 * position changes — when the position crosses 0.5 during an in-
 * flight mix transition, that's the visible midpoint of a fade. We
 * bump an atomic counter the OBS-side filter_tick polls so it can
 * react without static-delay guessing. */
static std::atomic<uint64_t> g_mix_midpoint_counter{0};

class MixEffectCallback : public IBMDSwitcherMixEffectBlockCallback {
public:
	explicit MixEffectCallback(IBMDSwitcherMixEffectBlock *me)
		: refCount_(1), me_(me) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override
	{
		if (!ppv) return E_POINTER;
		if (memcmp(&iid, &IID_IUnknown, sizeof(IID)) == 0 ||
		    memcmp(&iid, &IID_IBMDSwitcherMixEffectBlockCallback,
		           sizeof(IID)) == 0) {
			*ppv = static_cast<IBMDSwitcherMixEffectBlockCallback *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef(void) override { return ++refCount_; }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		const ULONG n = --refCount_;
		if (n == 0) delete this;
		return n;
	}

	HRESULT STDMETHODCALLTYPE Notify(
		BMDSwitcherMixEffectBlockEventType eventType) override
	{
		if (eventType ==
		    bmdSwitcherMixEffectBlockEventTypeTransitionPositionChanged) {
			double pos = 0.0;
			if (me_ &&
			    SUCCEEDED(me_->GetTransitionPosition(&pos))) {
				/* Fire on every callback that's at or past 0.5
				 * AFTER a previous report below 0.5 — but the
				 * callback fires often enough that simpler
				 * "armed" tracking via atomic is enough.
				 * One mix transition produces many position-
				 * change events; we only want one midpoint
				 * fire per transition. last_pos_ tracks
				 * previous to detect the rising 0.5 crossing. */
				double prev = last_pos_.exchange(pos);
				if (prev < 0.5 && pos >= 0.5) {
					g_mix_midpoint_counter.fetch_add(1);
				}
				/* Reset on transition end so a new one can fire
				 * its own midpoint. */
				if (pos >= 1.0 - 1e-6 || pos <= 1e-6)
					last_pos_.store(0.0);
			}
		}
		return S_OK;
	}

private:
	std::atomic<ULONG> refCount_;
	IBMDSwitcherMixEffectBlock *me_;
	std::atomic<double> last_pos_{0.0};
};

struct AtemRequest {
	enum Kind { CUT, MIX } kind;
	int input;
	int duration_frames; /* MIX only */
};

class AtemWorker {
public:
	AtemWorker() = default;
	~AtemWorker() { stop(); }

	void start()
	{
		if (running_.exchange(true))
			return;
		thread_ = std::thread([this]() { run(); });
	}

	void stop()
	{
		if (!running_.exchange(false))
			return;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			cv_.notify_all();
		}
		if (thread_.joinable())
			thread_.join();
	}

	void enqueueCut(int input)
	{
		if (input <= 0)
			return;
		AtemRequest r{AtemRequest::CUT, input, 0};
		push(r);
	}

	void enqueueMix(int input, int duration_frames)
	{
		if (input <= 0)
			return;
		if (duration_frames < 1)
			duration_frames = 1;
		if (duration_frames > 250) /* ATEM max is 250 frames */
			duration_frames = 250;
		AtemRequest r{AtemRequest::MIX, input, duration_frames};
		push(r);
	}

	bool isConnected() const { return connected_.load(); }
	int  lastProgramInput() const { return last_program_input_.load(); }

private:
	void push(const AtemRequest &r)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			/* Coalesce: if multiple requests pile up faster than
			 * we can dispatch, only the latest matters. */
			while (!queue_.empty())
				queue_.pop();
			queue_.push(r);
		}
		cv_.notify_one();
	}

	void run()
	{
		HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		blog(LOG_INFO,
		     "[Shot Presets] ATEM worker thread started (CoInit=0x%lx)",
		     (long)comInit);

		/* Try an initial connect. If it fails (no ATEM plugged in
		 * yet), we'll retry on first switch request. */
		std::string err;
		if (!connect(err))
			blog(LOG_INFO,
			     "[Shot Presets] ATEM not yet available: %s "
			     "(will retry on first trigger)",
			     err.c_str());

		while (running_.load()) {
			AtemRequest req{AtemRequest::CUT, 0, 0};
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_.wait(lock, [this]() {
					return !running_.load() ||
					       !queue_.empty();
				});
				if (!running_.load())
					break;
				req = queue_.front();
				queue_.pop();
			}

			if (req.input <= 0)
				continue;

			if (!connected_.load()) {
				std::string e;
				if (!connect(e)) {
					blog(LOG_WARNING,
					     "[Shot Presets] ATEM switch to %d "
					     "skipped — connect failed: %s",
					     req.input, e.c_str());
					continue;
				}
			}

			HRESULT hr;
			const char *opName;
			if (req.kind == AtemRequest::MIX) {
				hr = performMix(req.input,
				                 req.duration_frames);
				opName = "PerformAutoTransition(MIX)";
			} else {
				hr = mixEffectBlock_->SetProgramInput(
					static_cast<BMDSwitcherInputId>(
						req.input));
				opName = "SetProgramInput";
			}
			if (FAILED(hr)) {
				blog(LOG_WARNING,
				     "[Shot Presets] ATEM %s(%d) failed: "
				     "hr=0x%lx — disconnecting for retry on "
				     "next trigger",
				     opName, req.input, (long)hr);
				disconnect();
			} else if (req.kind == AtemRequest::MIX) {
				last_program_input_.store(req.input);
				blog(LOG_INFO,
				     "[Shot Presets] ATEM mix transition -> %d "
				     "(%d frames)",
				     req.input, req.duration_frames);
			} else {
				last_program_input_.store(req.input);
				blog(LOG_INFO,
				     "[Shot Presets] ATEM program input -> %d",
				     req.input);
			}
		}

		disconnect();
		if (SUCCEEDED(comInit))
			CoUninitialize();
		blog(LOG_INFO, "[Shot Presets] ATEM worker thread stopped");
	}

	HRESULT performMix(int input, int durationFrames)
	{
		if (!mixEffectBlock_)
			return E_POINTER;

		IBMDSwitcherTransitionParameters *txParams = nullptr;
		HRESULT hr = mixEffectBlock_->QueryInterface(
			IID_IBMDSwitcherTransitionParameters,
			reinterpret_cast<void **>(&txParams));
		if (FAILED(hr) || !txParams)
			return FAILED(hr) ? hr : E_POINTER;
		hr = txParams->SetNextTransitionStyle(
			bmdSwitcherTransitionStyleMix);
		txParams->Release();
		if (FAILED(hr))
			return hr;

		IBMDSwitcherTransitionMixParameters *mixParams = nullptr;
		hr = mixEffectBlock_->QueryInterface(
			IID_IBMDSwitcherTransitionMixParameters,
			reinterpret_cast<void **>(&mixParams));
		if (FAILED(hr) || !mixParams)
			return FAILED(hr) ? hr : E_POINTER;
		hr = mixParams->SetRate(static_cast<uint32_t>(durationFrames));
		mixParams->Release();
		if (FAILED(hr))
			return hr;

		hr = mixEffectBlock_->SetPreviewInput(
			static_cast<BMDSwitcherInputId>(input));
		if (FAILED(hr))
			return hr;

		return mixEffectBlock_->PerformAutoTransition();
	}

	bool connect(std::string &error)
	{
		disconnect();

		HRESULT hr = CoCreateInstance(
			CLSID_CBMDSwitcherDiscovery, nullptr, CLSCTX_ALL,
			IID_IBMDSwitcherDiscovery,
			reinterpret_cast<void **>(&discovery_));
		if (FAILED(hr) || !discovery_) {
			error = "CoCreateInstance(CBMDSwitcherDiscovery) "
			        "failed — is BMDSwitcherAPI64.dll registered? "
			        "(install the ATEM Switchers software)";
			return false;
		}

		BMDSwitcherConnectToFailure failReason =
			bmdSwitcherConnectToFailureNoResponse;
		BSTR address = SysAllocString(L"");
		hr = discovery_->ConnectTo(address, &switcher_, &failReason);
		SysFreeString(address);
		if (FAILED(hr) || !switcher_) {
			error = "ConnectTo(\"\") failed — no ATEM found via "
			        "USB / network discovery";
			return false;
		}

		IBMDSwitcherMixEffectBlockIterator *it = nullptr;
		hr = switcher_->CreateIterator(
			IID_IBMDSwitcherMixEffectBlockIterator,
			reinterpret_cast<void **>(&it));
		if (FAILED(hr) || !it) {
			error = "CreateIterator(MixEffectBlock) failed";
			return false;
		}
		hr = it->Next(&mixEffectBlock_);
		it->Release();
		if (FAILED(hr) || !mixEffectBlock_) {
			error = "Switcher has no mix-effect block";
			return false;
		}

		callback_ = new MixEffectCallback(mixEffectBlock_);
		mixEffectBlock_->AddCallback(callback_);

		connected_.store(true);
		BMDSwitcherInputId current = 0;
		mixEffectBlock_->GetProgramInput(&current);
		last_program_input_.store((int)current);
		blog(LOG_INFO,
		     "[Shot Presets] ATEM connected (current program input %lld) "
		     "+ MixEffect callback registered",
		     (long long)current);
		return true;
	}

	void disconnect()
	{
		connected_.store(false);
		if (mixEffectBlock_ && callback_) {
			mixEffectBlock_->RemoveCallback(callback_);
		}
		if (callback_) {
			callback_->Release();
			callback_ = nullptr;
		}
		if (mixEffectBlock_) {
			mixEffectBlock_->Release();
			mixEffectBlock_ = nullptr;
		}
		if (switcher_) {
			switcher_->Release();
			switcher_ = nullptr;
		}
		if (discovery_) {
			discovery_->Release();
			discovery_ = nullptr;
		}
	}

	std::atomic<bool> running_{false};
	std::atomic<bool> connected_{false};
	std::atomic<int>  last_program_input_{-1};
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::queue<AtemRequest> queue_;

	IBMDSwitcherDiscovery *discovery_ = nullptr;
	IBMDSwitcher *switcher_ = nullptr;
	IBMDSwitcherMixEffectBlock *mixEffectBlock_ = nullptr;
	MixEffectCallback *callback_ = nullptr;
};

static AtemWorker g_worker;
#endif /* _WIN32 */

} // namespace

extern "C" {

void atem_init(void)
{
#ifdef _WIN32
	g_worker.start();
#endif
}

void atem_shutdown(void)
{
#ifdef _WIN32
	g_worker.stop();
#endif
}

void atem_set_program_input(int input)
{
#ifdef _WIN32
	g_worker.enqueueCut(input);
#else
	(void)input;
#endif
}

void atem_perform_mix_transition(int input, int duration_frames)
{
#ifdef _WIN32
	g_worker.enqueueMix(input, duration_frames);
#else
	(void)input;
	(void)duration_frames;
#endif
}

int atem_is_connected(void)
{
#ifdef _WIN32
	return g_worker.isConnected() ? 1 : 0;
#else
	return 0;
#endif
}

int atem_get_last_program_input(void)
{
#ifdef _WIN32
	return g_worker.lastProgramInput();
#else
	return -1;
#endif
}

unsigned long long atem_get_mix_midpoint_counter(void)
{
#ifdef _WIN32
	return (unsigned long long)g_mix_midpoint_counter.load();
#else
	return 0ULL;
#endif
}

} /* extern "C" */
