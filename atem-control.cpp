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
				blog(LOG_INFO,
				     "[Shot Presets] ATEM mix transition -> %d "
				     "(%d frames)",
				     req.input, req.duration_frames);
			} else {
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

		connected_.store(true);
		BMDSwitcherInputId current = 0;
		mixEffectBlock_->GetProgramInput(&current);
		blog(LOG_INFO,
		     "[Shot Presets] ATEM connected (current program input %lld)",
		     (long long)current);
		return true;
	}

	void disconnect()
	{
		connected_.store(false);
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
	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::queue<AtemRequest> queue_;

	IBMDSwitcherDiscovery *discovery_ = nullptr;
	IBMDSwitcher *switcher_ = nullptr;
	IBMDSwitcherMixEffectBlock *mixEffectBlock_ = nullptr;
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

} /* extern "C" */
