#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace vocalsynth
{

/** Lock-free single-producer / single-consumer FIFO of short MIDI messages (1..3 bytes, no sysex).

    Producer (audio thread): push(). Wait-free, never allocates, never blocks. A full FIFO drops the
    message and increments the overflow counter. Messages of 0 or more than 3 bytes are rejected
    without counting an overflow.
    Consumer (one other thread): drain(), discardAll(), getNumReady().
    Any thread: getOverflowCount().

    Note: juce::AbstractFifo keeps one slot free, so at most Capacity - 1 messages fit. */
template <int Capacity>
class ShortMidiFifo
{
public:
    static_assert (Capacity >= 2);

    /** POD FIFO entry: one short MIDI message. */
    struct Message
    {
        uint8_t bytes[3];
        uint8_t size;
    };

    static constexpr int capacity = Capacity;
    static constexpr int usableCapacity = Capacity - 1;

    /** Producer only. Returns false if the message was rejected (invalid size) or dropped (full). */
    bool push (const uint8_t* data, int size) noexcept
    {
        if (data == nullptr || size < 1 || size > 3)
            return false;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 < 1)
        {
            // Single producer: a plain load + store is enough and avoids a locked RMW on the audio thread
            overflowCount.store (overflowCount.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return false;
        }

        auto& slot = storage[(size_t) (size1 > 0 ? start1 : start2)];
        slot.bytes[0] = data[0];
        slot.bytes[1] = size > 1 ? data[1] : uint8_t { 0 };
        slot.bytes[2] = size > 2 ? data[2] : uint8_t { 0 };
        slot.size = (uint8_t) size;

        fifo.finishedWrite (1);
        return true;
    }

    /** Consumer only. Calls fn (const Message&) for every message that is ready, in FIFO order, and
        removes them. Returns the number of messages handed out. */
    template <typename Fn>
    int drain (Fn&& fn)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            fn (storage[(size_t) (start1 + i)]);

        for (int i = 0; i < size2; ++i)
            fn (storage[(size_t) (start2 + i)]);

        fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

    /** Consumer only. Drops everything that is ready. */
    void discardAll() noexcept { fifo.finishedRead (fifo.getNumReady()); }

    /** Consumer (exact) or any thread (approximate). */
    int getNumReady() const noexcept { return fifo.getNumReady(); }

    /** Any thread. Never reset. */
    uint32_t getOverflowCount() const noexcept { return overflowCount.load (std::memory_order_relaxed); }

private:
    juce::AbstractFifo fifo { Capacity };
    std::array<Message, (size_t) Capacity> storage {};
    std::atomic<uint32_t> overflowCount { 0 };

    static_assert (std::atomic<uint32_t>::is_always_lock_free);
};

/** Optional virtual MIDI output device (M1 test T7): FL Studio can record from it like from a
    hardware MIDI keyboard.

    Threading (docs/M1-DESIGN.md 4.3):
    - Audio thread: setEnabled() and push() only. Both are wait-free and never allocate.
    - Own juce::Thread ("VocalSynth virtual MIDI", the "port thread"): polls the enabled flag, asks the
      message thread for a device, drains the FIFO into the device with sendMessageNow(), and before
      giving the device back sends the note-offs / bend centre for what it left sounding.
    - Message thread: CREATES and DESTROYS every device (handleAsyncUpdate of a private AsyncUpdater).
      In JUCE 9, MidiOutput is built on ump::Output, whose Impl constructor/destructor and
      (dis)connection listeners assert the message thread and are not synchronised against it, and the
      native endpoint registries (std::map) are read on the message thread. So creating or destroying
      a device on any other thread would race with JUCE's own message-thread code.
    - UI thread: getStatus(), getOverflowCount(), getDeviceName().

    Handshake between the port thread and the message thread (handoffLock, held only for swapping
    pointers and flags, never across a native call, never on the audio thread):
    - Open: the port thread sets openRequested and triggers the AsyncUpdater. The message thread
      reserves a name, creates the device (outside every lock), and publishes it (or the failure) in
      the handoff slots. The port thread adopts it on its next poll and only then reports 'open'.
    - Close: the port thread stops using the device (drain, note-offs, bend centre), moves it into the
      'retired' slot and triggers the AsyncUpdater. The message thread destroys it and releases the name.
    - If the message thread does not answer within retryIntervalMs (no message loop), the status
      becomes 'failed'; the request stays pending and is completed when the message thread runs.

    The device factory may return nullptr (CI, Windows without Windows MIDI Services, name clash):
    the status becomes 'failed', pushed messages are discarded, nothing crashes. While disabled or
    failed, push() drops messages without counting them as overflow.

    Naming: a process-wide list of names in use (a static in the .cpp, message thread only) gives
    "VocalSynth FX", "VocalSynth FX 2", ... up to maxInstanceNumber. MidiInput/MidiOutput::
    getAvailableDevices() are not called, so names owned by other processes are not detected.

    Polling: every pollIntervalMs while open (this bounds the extra jitter), idlePollIntervalMs while
    off or failed; a failed creation is retried at most every retryIntervalMs.

    Destruction should happen on the message thread (the plugin wrappers do that). The port thread
    is stopped first; it closes the device as above. A device left over is destroyed directly on the
    message thread, or handed to the message thread with MessageManager::callAsync otherwise. If the
    port thread had to be killed after stopTimeoutMs, the devices are leaked on purpose: the killed
    thread may have been inside them. */
class VirtualMidiPort final : private juce::Thread,
                              private juce::AsyncUpdater
{
public:
    enum class Status
    {
        off,   ///< disabled
        open,  ///< device created and receiving
        failed ///< enabled, but the device could not be created (or the message thread did not answer)
    };

    /** The device side of the port. The default implementation wraps juce::MidiOutput::createNewDevice();
        tests inject a fake through the DeviceFactory. */
    class Device
    {
    public:
        /** Called on the message thread only. */
        virtual ~Device() = default;

        /** Called on the port thread only (never the audio thread). */
        virtual void sendMessageNow (const juce::MidiMessage& message) = 0;
    };

    /** Called on the message thread only. Returns nullptr if the device cannot be created. An
        exception is caught and treated like nullptr. */
    using DeviceFactory = std::function<std::unique_ptr<Device> (const juce::String& name)>;

    static constexpr int fifoCapacity = 4096;
    static constexpr int pollIntervalMs = 1;         ///< while open
    static constexpr int idlePollIntervalMs = 50;    ///< while off or failed
    static constexpr int retryIntervalMs = 3000;     ///< between failed attempts / message thread timeout
    static constexpr int stopTimeoutMs = 5000;       ///< the port thread makes no slow native calls any more
    static constexpr int maxInstanceNumber = 16;

    using Fifo = ShortMidiFifo<fifoCapacity>;
    using Message = Fifo::Message;

    /** baseName: "VocalSynth FX" or "VocalSynth Instrument". If another instance in this process
        already uses that name, an instance number is appended ("VocalSynth FX 2"). An empty factory
        means juce::MidiOutput::createNewDevice(). Starts the thread. */
    explicit VirtualMidiPort (juce::String baseName, DeviceFactory factory = {});

    /** Stops the thread (stopTimeoutMs), which closes the device (note-offs first, see above), then
        destroys what is left on the message thread. */
    ~VirtualMidiPort() override;

    /** Any thread (the processor calls it from processBlock with the virtualMidiOut parameter). */
    void setEnabled (bool shouldBeEnabled) noexcept { enabled.store (shouldBeEnabled, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

    /** Audio thread (single producer). Returns false if the message was dropped (FIFO full -> overflow
        counter incremented, or port not open). Messages longer than 3 bytes are dropped. */
    bool push (const juce::MidiMessage& message) noexcept;
    bool push (const uint8_t* data, int size) noexcept;

    Status getStatus() const noexcept { return status.load (std::memory_order_relaxed); }
    uint32_t getOverflowCount() const noexcept { return fifo.getOverflowCount(); }

    /** Name of the open device, empty if none. Message thread. */
    juce::String getDeviceName() const;

private:
    enum class OpenResult
    {
        none,      ///< no answer yet
        opened,    ///< openedDevice / openedName are set
        failed,    ///< the factory returned nullptr, threw, or no name was free
        cancelled  ///< the port was disabled before the message thread got to it
    };

    void run() override;
    void handleAsyncUpdate() override;

    // Port thread only
    void requestOpen();
    void collectOpenResult();
    void adoptDevice (std::unique_ptr<Device> newDevice, const juce::String& name);
    void closeDevice();
    void drainIntoDevice();
    void sendToDevice (const Message& message);
    void setDeviceName (const juce::String& name);

    // Message thread only (or the destructor)
    void openOnMessageThread();
    static void destroyDevice (std::unique_ptr<Device> deviceToDestroy, const juce::String& name);

    const juce::String baseName;
    const DeviceFactory factory;

    std::atomic<bool> enabled { false };
    std::atomic<Status> status { Status::off };

    Fifo fifo;

    // Port thread only (deviceName also read by the UI under deviceNameLock)
    std::unique_ptr<Device> device;
    juce::String deviceRegisteredName;             ///< name entry of 'device' in the process-wide list
    std::array<std::bitset<128>, 16> sentNotes {}; ///< notes sent and not yet turned off [channel - 1][note]
    std::bitset<16> sentBendNotCentred {};         ///< last bend sent on [channel - 1] was not centred
    double lastFailedAttemptMs = 0.0;
    double openRequestedAtMs = 0.0;
    bool openPending = false;                      ///< a request was sent and not answered yet
    juce::String deviceName;
    juce::CriticalSection deviceNameLock; // never taken on the audio thread

    // Handoff between the port thread and the message thread, guarded by handoffLock
    juce::CriticalSection handoffLock; // never taken on the audio thread, never held across a native call
    bool openRequested = false;
    OpenResult openResult = OpenResult::none;
    std::unique_ptr<Device> openedDevice;
    juce::String openedName;

    struct RetiredDevice
    {
        std::unique_ptr<Device> device;
        juce::String name; ///< registry entry to release after destruction
    };

    std::vector<RetiredDevice> retired; ///< handed back by the port thread, destroyed on the message thread

    static_assert (std::atomic<Status>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualMidiPort)
};

} // namespace vocalsynth
