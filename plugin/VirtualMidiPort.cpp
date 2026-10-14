#include "VirtualMidiPort.h"

#include <utility>

namespace vocalsynth
{

namespace
{
    /** Process-wide list of virtual port names in use by this process (docs/M1-DESIGN.md 4.3).

        Message thread only in practice (devices are created and destroyed there). The lock is kept
        so that the list stays consistent even if a destructor runs elsewhere; it is only held for
        the list operations, never across device creation or destruction.

        Names owned by OTHER processes are not detected: MidiInput/MidiOutput::getAvailableDevices()
        read a JUCE cache that the message thread rebuilds, and a clash with another process gives
        either a second device with the same name (ALSA, CoreMIDI) or a failed creation. */
    struct PortRegistry
    {
        juce::CriticalSection lock;
        juce::StringArray namesInUse;
    };

    PortRegistry& getPortRegistry()
    {
        static PortRegistry registry;
        return registry;
    }

    juce::String makeCandidateName (const juce::String& baseName, int instanceNumber)
    {
        return instanceNumber <= 1 ? baseName : baseName + " " + juce::String (instanceNumber);
    }

    juce::String reserveName (const juce::String& baseName, int maxInstanceNumber)
    {
        auto& registry = getPortRegistry();
        const juce::ScopedLock sl (registry.lock);

        for (int i = 1; i <= maxInstanceNumber; ++i)
        {
            const auto candidate = makeCandidateName (baseName, i);

            if (! registry.namesInUse.contains (candidate))
            {
                registry.namesInUse.add (candidate);
                return candidate;
            }
        }

        return {};
    }

    void releaseName (const juce::String& name)
    {
        if (name.isEmpty())
            return;

        auto& registry = getPortRegistry();
        const juce::ScopedLock sl (registry.lock);
        registry.namesInUse.removeString (name);
    }

    /** The real device: a juce::MidiOutput virtual output. Created and destroyed on the message
        thread (JUCE 9 asserts that for ump::Output), sendMessageNow() on the port thread. */
    class JuceVirtualMidiDevice final : public VirtualMidiPort::Device
    {
    public:
        explicit JuceVirtualMidiDevice (std::unique_ptr<juce::MidiOutput> o) : output (std::move (o)) {}

        void sendMessageNow (const juce::MidiMessage& message) override { output->sendMessageNow (message); }

    private:
        std::unique_ptr<juce::MidiOutput> output;
    };

    std::unique_ptr<VirtualMidiPort::Device> createJuceDevice (const juce::String& name)
    {
        auto output = juce::MidiOutput::createNewDevice (name);

        if (output == nullptr)
            return nullptr;

        return std::make_unique<JuceVirtualMidiDevice> (std::move (output));
    }
} // namespace

VirtualMidiPort::VirtualMidiPort (juce::String name, DeviceFactory deviceFactory)
    : juce::Thread ("VocalSynth virtual MIDI"),
      baseName (std::move (name)),
      factory (std::move (deviceFactory))
{
    // The poll interval bounds the timing jitter of the port, so the thread runs at high priority.
    // If the thread cannot be started the port simply stays 'off'.
    startThread (juce::Thread::Priority::high);
}

VirtualMidiPort::~VirtualMidiPort()
{
    // run() closes the device on its way out (note-offs, then the device goes to 'retired').
    const bool stoppedCleanly = stopThread (stopTimeoutMs);

    if (! stoppedCleanly)
    {
        // The port thread was killed, possibly inside sendMessageNow() or while holding
        // handoffLock. Touching the devices or taking the lock could hang the host, so the
        // devices are leaked on purpose (their names stay reserved).
        cancelPendingUpdate();
        (void) device.release();
        (void) openedDevice.release();

        for (auto& r : retired)
            (void) r.device.release();

        return;
    }

    // Only if the thread never ran: then nobody else uses the device.
    if (device != nullptr)
        closeDevice();

    cancelPendingUpdate();

    std::vector<RetiredDevice> leftovers;
    std::unique_ptr<Device> notAdopted;
    juce::String notAdoptedName;

    {
        const juce::ScopedLock sl (handoffLock);
        leftovers.swap (retired);
        notAdopted = std::move (openedDevice);
        notAdoptedName = std::exchange (openedName, {});
    }

    for (auto& r : leftovers)
        destroyDevice (std::move (r.device), r.name);

    destroyDevice (std::move (notAdopted), notAdoptedName);
}

bool VirtualMidiPort::push (const juce::MidiMessage& message) noexcept
{
    return push (message.getRawData(), message.getRawDataSize());
}

bool VirtualMidiPort::push (const uint8_t* data, int size) noexcept
{
    // While not open, messages are dropped without counting an overflow.
    if (status.load (std::memory_order_relaxed) != Status::open)
        return false;

    return fifo.push (data, size);
}

juce::String VirtualMidiPort::getDeviceName() const
{
    const juce::ScopedLock sl (deviceNameLock);
    return deviceName;
}

void VirtualMidiPort::setDeviceName (const juce::String& name)
{
    const juce::ScopedLock sl (deviceNameLock);
    deviceName = name;
}

//==============================================================================
// Message thread

void VirtualMidiPort::handleAsyncUpdate()
{
    std::vector<RetiredDevice> toDestroy;
    bool shouldOpen = false;

    {
        const juce::ScopedLock sl (handoffLock);
        toDestroy.swap (retired);
        shouldOpen = std::exchange (openRequested, false);
    }

    // Retired devices first, so that their names are free again for a new device.
    for (auto& r : toDestroy)
        destroyDevice (std::move (r.device), r.name);

    if (shouldOpen)
        openOnMessageThread();
}

void VirtualMidiPort::openOnMessageThread()
{
    if (! enabled.load (std::memory_order_relaxed))
    {
        {
            const juce::ScopedLock sl (handoffLock);
            openResult = OpenResult::cancelled;
        }

        notify();
        return;
    }

    // Reserve the name under the registry lock, create the device outside every lock.
    const auto name = reserveName (baseName, maxInstanceNumber);
    std::unique_ptr<Device> newDevice;

    if (name.isNotEmpty())
    {
        try
        {
            newDevice = factory ? factory (name) : createJuceDevice (name);
        }
        catch (...) // an exception must never reach the host
        {
            newDevice.reset();
        }

        if (newDevice == nullptr)
            releaseName (name);
    }

    std::unique_ptr<Device> stale;
    juce::String staleName;

    {
        const juce::ScopedLock sl (handoffLock);

        if (newDevice != nullptr)
        {
            // Cannot happen (one request at a time), but a device must never be lost or destroyed
            // on the wrong thread.
            jassert (openedDevice == nullptr);
            stale = std::move (openedDevice);
            staleName = std::exchange (openedName, {});

            openedDevice = std::move (newDevice);
            openedName = name;
            openResult = OpenResult::opened;
        }
        else
        {
            openResult = OpenResult::failed;
        }
    }

    destroyDevice (std::move (stale), staleName);
    notify(); // wake the port thread
}

void VirtualMidiPort::destroyDevice (std::unique_ptr<Device> deviceToDestroy, const juce::String& name)
{
    if (deviceToDestroy == nullptr && name.isEmpty())
        return;

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        deviceToDestroy.reset();
        releaseName (name);
        return;
    }

    // Only reached if a VirtualMidiPort is destroyed off the message thread: hand the device over.
    std::shared_ptr<Device> shared (std::move (deviceToDestroy));

    if (! juce::MessageManager::callAsync ([shared, name]() mutable
                                           {
                                               shared.reset();
                                               releaseName (name);
                                           }))
    {
        // No message loop at all: nothing better is left than destroying it here.
        shared.reset();
        releaseName (name);
    }
}

//==============================================================================
// Port thread

void VirtualMidiPort::run()
{
    while (! threadShouldExit())
    {
        if (enabled.load (std::memory_order_relaxed))
        {
            if (device == nullptr)
            {
                collectOpenResult();

                if (device == nullptr)
                {
                    const auto now = juce::Time::getMillisecondCounterHiRes();

                    if (openPending)
                    {
                        // The message thread did not answer (no message loop?): report it instead of
                        // staying 'off'. The request stays pending.
                        if (now - openRequestedAtMs >= (double) retryIntervalMs)
                            status.store (Status::failed, std::memory_order_relaxed);
                    }
                    else if (status.load (std::memory_order_relaxed) != Status::failed
                             || now - lastFailedAttemptMs >= (double) retryIntervalMs)
                    {
                        requestOpen();
                    }
                }
            }

            if (device != nullptr)
                drainIntoDevice();
            else
                fifo.discardAll();
        }
        else
        {
            if (device != nullptr)
                closeDevice();

            // A device that arrives after disabling goes straight back to the message thread.
            collectOpenResult();

            if (device != nullptr)
                closeDevice();

            status.store (Status::off, std::memory_order_relaxed); // also leaves 'failed'
            fifo.discardAll();
        }

        wait (device != nullptr ? (double) pollIntervalMs : (double) idlePollIntervalMs);
    }

    // A device created but not adopted yet stays in the handoff slot; the destructor destroys it.
    if (device != nullptr)
        closeDevice();
}

void VirtualMidiPort::requestOpen()
{
    {
        const juce::ScopedLock sl (handoffLock);
        openRequested = true;
        openResult = OpenResult::none;
    }

    openPending = true;
    openRequestedAtMs = juce::Time::getMillisecondCounterHiRes();
    triggerAsyncUpdate();
}

void VirtualMidiPort::collectOpenResult()
{
    if (! openPending)
        return;

    auto result = OpenResult::none;
    std::unique_ptr<Device> newDevice;
    juce::String name;

    {
        const juce::ScopedLock sl (handoffLock);
        result = std::exchange (openResult, OpenResult::none);

        if (result == OpenResult::opened)
        {
            newDevice = std::move (openedDevice);
            name = std::exchange (openedName, {});
        }
    }

    if (result == OpenResult::none)
        return;

    openPending = false;

    if (result == OpenResult::opened && newDevice != nullptr)
    {
        adoptDevice (std::move (newDevice), name);
    }
    else if (result == OpenResult::failed)
    {
        lastFailedAttemptMs = juce::Time::getMillisecondCounterHiRes();
        status.store (Status::failed, std::memory_order_relaxed);
    }
    // cancelled: the next poll asks again if the port is enabled by then
}

void VirtualMidiPort::adoptDevice (std::unique_ptr<Device> newDevice, const juce::String& name)
{
    device = std::move (newDevice);
    deviceRegisteredName = name;

    for (auto& channel : sentNotes)
        channel.reset();

    sentBendNotCentred.reset();
    setDeviceName (name);

    // Anything still in the FIFO was pushed by the audio thread while the previous device was being
    // closed; it must not leak into the new device.
    fifo.discardAll();
    status.store (Status::open, std::memory_order_relaxed);
}

void VirtualMidiPort::closeDevice()
{
    if (device != nullptr)
    {
        // 1. what the audio thread already pushed (usually the note-offs that matter)
        drainIntoDevice();

        // 2. no stuck notes / bend in the program that records from the port
        for (size_t ch = 0; ch < sentNotes.size(); ++ch)
        {
            if (sentNotes[ch].none())
                continue;

            for (size_t note = 0; note < 128; ++note)
                if (sentNotes[ch][note])
                    device->sendMessageNow (juce::MidiMessage::noteOff ((int) ch + 1, (int) note));
        }

        for (size_t ch = 0; ch < 16; ++ch)
            if (sentBendNotCentred[ch])
                device->sendMessageNow (juce::MidiMessage::pitchWheel ((int) ch + 1, 8192));
    }

    for (auto& channel : sentNotes)
        channel.reset();

    sentBendNotCentred.reset();

    // 3. hand the device to the message thread, which destroys it and releases the name
    {
        RetiredDevice r;
        r.device = std::move (device);
        r.name = std::exchange (deviceRegisteredName, {});

        const juce::ScopedLock sl (handoffLock);
        retired.push_back (std::move (r)); // port thread: allocation is fine here
    }

    setDeviceName ({});

    // 4. status, then discard what arrived meanwhile (push() stops accepting once status is off)
    status.store (Status::off, std::memory_order_relaxed);
    fifo.discardAll();
    triggerAsyncUpdate();
}

void VirtualMidiPort::drainIntoDevice()
{
    fifo.drain ([this] (const Message& m) { sendToDevice (m); });
}

void VirtualMidiPort::sendToDevice (const Message& m)
{
    if (device == nullptr || m.size < 1 || m.size > 3)
        return;

    const auto statusByte = m.bytes[0];
    const auto type = statusByte & 0xf0;
    const auto ch = (size_t) (statusByte & 0x0f);

    if (m.size == 3)
    {
        const auto note = (size_t) (m.bytes[1] & 0x7f);

        if (type == 0x90 && m.bytes[2] != 0)
            sentNotes[ch][note] = true;
        else if (type == 0x80 || type == 0x90)
            sentNotes[ch][note] = false;
        else if (type == 0xe0)
            sentBendNotCentred[ch] = ((int) (m.bytes[1] & 0x7f) | ((int) (m.bytes[2] & 0x7f) << 7)) != 8192;
    }

    device->sendMessageNow (juce::MidiMessage (m.bytes, (int) m.size));
}

} // namespace vocalsynth
