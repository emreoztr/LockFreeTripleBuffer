// LockFreeTripleBuffering.h : Include file for standard system include files,
// or project specific include files.

#pragma once
#include <array>
#include <atomic>


enum class ConsumeMode {
	Try, // Non-blocking
	Wait // Blocking
};

template<typename T>
class LockFreeTripleBuffer
{
private:
	struct alignas(1) State {
		uint8_t stateIndex : 2;
		uint8_t hasNewData : 1;
		uint8_t reserved : 5;
		bool operator==(const State&) const = default;
	};

public:
	LockFreeTripleBuffer() = default;
	~LockFreeTripleBuffer() = default;

	template <typename U>
	void produce(U&& data) noexcept {
		_buffers[_producerState.stateIndex] = std::forward<U>(data);
		_producerState.hasNewData = 1;
		auto currentSharedState = _sharedState.load(std::memory_order_acquire);
		while (!_sharedState.compare_exchange_weak(currentSharedState, _producerState, std::memory_order_release, std::memory_order_relaxed));
		_producerState.stateIndex = currentSharedState.stateIndex;
		_sharedState.notify_one();
	}

	template <ConsumeMode Mode = ConsumeMode::Wait>
	bool consume_wait(T& data) noexcept {
		auto currentSharedState = _sharedState.load(std::memory_order_relaxed);

		if (currentSharedState.hasNewData == 0) {
			if constexpr (Mode == ConsumeMode::Try) {
				return false;
			}
			else {
				while (currentSharedState.hasNewData == 0) {
					_sharedState.wait(currentSharedState, std::memory_order_relaxed);
					currentSharedState = _sharedState.load(std::memory_order_relaxed);
				}
			}
		}
		_consumerState.hasNewData = 0;
		while (!_sharedState.compare_exchange_weak(currentSharedState, _consumerState, std::memory_order_acq_rel, std::memory_order_relaxed));
		_consumerState.stateIndex = currentSharedState.stateIndex;
		data = std::move_if_noexcept(_buffers[_consumerState.stateIndex]);
		return true;
	}

private:
#ifdef __cpp_lib_hardware_interference_size
	using std::hardware_destructive_interference_size;
#else
	constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

	alignas(hardware_destructive_interference_size) std::array<T, 3> _buffers;
	alignas(hardware_destructive_interference_size) std::atomic<State> _sharedState{ .stateIndex = 1, .hasNewData = 0, .reserved = 0 };
	alignas(hardware_destructive_interference_size) State _producerState{.stateIndex = 0, .hasNewData = 0, .reserved = 0};
	alignas(hardware_destructive_interference_size) State _consumerState{.stateIndex = 2, .hasNewData = 0, .reserved = 0};
};

